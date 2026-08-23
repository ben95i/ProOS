//// Automate REST API - Favicon Fetch ////
// by Ben Provenzano III //
//
// Pulls a site's favicon and returns it as a URL-safe base64 PNG,
// 24x24 pixels, quantized to 32 colours or less.
//
// The output is an indexed (colour type 3) PNG rather than a JPEG: at this
// size a 32 entry palette plus deflate is roughly a third of the bytes a
// JPEG of the same image needs, and it holds the 32 colours exactly, which
// a JPEG cannot -- its DCT re-introduces intermediate values. The encoder
// is a few lines against node's own zlib, so nothing is added to install.
//
// Decoding is pure JavaScript (no native modules): ICO (BMP or embedded
// PNG), PNG and JPEG are supported. Anything else (SVG, GIF) is skipped
// and the next candidate icon is tried instead.

const zlib = require('zlib');
const jpeg = require('jpeg-js');
const { PNG } = require('pngjs');

const ICON_SIZE = 24;      // output width / height in pixels
const MAX_COLORS = 32;     // palette size the image is reduced to
const FETCH_TIMEOUT = 6000; // per-request timeout in (ms)
const MAX_ICON_BYTES = 512 * 1024;
const USER_AGENT = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 ' +
    '(KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36';

//// candidate icon URLs ////

// pull every <link rel="...icon..."> out of the page markup, best first,
// then fall back to the well-known /favicon.ico location
function iconCandidates(html, pageUrl) {
    const base = new URL(pageUrl);
    const found = [];
    const linkTags = html.match(/<link\b[^>]*>/gi) || [];
    linkTags.forEach((tag) => {
        const relMatch = tag.match(/\brel\s*=\s*["']?([^"'>]+)/i);
        if (!relMatch || !/\bicon\b/i.test(relMatch[1])) return;
        const hrefMatch = tag.match(/\bhref\s*=\s*["']([^"']+)["']/i);
        if (!hrefMatch) return;
        const typeMatch = tag.match(/\btype\s*=\s*["']?([^"'>\s]+)/i);
        const sizeMatch = tag.match(/\bsizes\s*=\s*["']?(\d+)/i);
        try {
            found.push({
                url: new URL(hrefMatch[1], base).href,
                size: sizeMatch ? parseInt(sizeMatch[1], 10) : 0,
                svg: /svg/i.test(typeMatch ? typeMatch[1] : hrefMatch[1])
            });
        } catch (e) { /* malformed href, skip it */ }
    });
    // an icon at least as big as the target scales down cleanly; prefer the
    // smallest of those, then the largest of what is left. SVG goes last
    // because it cannot be decoded here.
    found.sort((a, b) => {
        if (a.svg !== b.svg) return a.svg ? 1 : -1;
        const aBig = a.size >= ICON_SIZE;
        const bBig = b.size >= ICON_SIZE;
        if (aBig !== bBig) return aBig ? -1 : 1;
        return aBig ? a.size - b.size : b.size - a.size;
    });
    const urls = found.filter((item) => !item.svg).map((item) => item.url);
    urls.push(new URL('/favicon.ico', base).href);
    // de-duplicate, keeping order
    return [...new Set(urls)];
}

async function fetchIconBytes(url) {
    const response = await fetch(url, {
        method: 'GET',
        redirect: 'follow',
        headers: { 'User-Agent': USER_AGENT, 'Accept': 'image/*' },
        signal: AbortSignal.timeout(FETCH_TIMEOUT)
    });
    if (!response.ok) return null;
    const buffer = Buffer.from(await response.arrayBuffer());
    if (buffer.length === 0 || buffer.length > MAX_ICON_BYTES) return null;
    return buffer;
}

//// decoders ////
// every decoder returns { width, height, data } where data is RGBA bytes

function isPNG(buf) {
    return buf.length > 8 && buf[0] === 0x89 && buf[1] === 0x50 &&
        buf[2] === 0x4E && buf[3] === 0x47;
}

function isJPEG(buf) {
    return buf.length > 3 && buf[0] === 0xFF && buf[1] === 0xD8;
}

function isICO(buf) {
    return buf.length > 6 && buf.readUInt16LE(0) === 0 &&
        (buf.readUInt16LE(2) === 1 || buf.readUInt16LE(2) === 2);
}

function decodePNG(buf) {
    const png = PNG.sync.read(buf);
    return { width: png.width, height: png.height, data: png.data };
}

function decodeJPEG(buf) {
    const img = jpeg.decode(buf, { useTArray: true });
    return { width: img.width, height: img.height, data: img.data };
}

// BITMAPINFOHEADER image as stored inside an .ico entry: bottom-up rows,
// padded to a 4 byte boundary, with a 1bpp transparency mask after them
function decodeICOBitmap(buf) {
    const headerSize = buf.readUInt32LE(0);
    const width = buf.readInt32LE(4);
    const height = Math.abs(buf.readInt32LE(8)) / 2; // XOR image + AND mask
    const bitCount = buf.readUInt16LE(14);
    const compression = buf.readUInt32LE(16);
    if (compression !== 0 || width <= 0 || height <= 0) return null;
    let paletteCount = buf.readUInt32LE(32);
    if (bitCount <= 8 && paletteCount === 0) paletteCount = 1 << bitCount;
    const paletteStart = headerSize;
    const pixelStart = paletteStart + (paletteCount * 4);
    const rowBytes = Math.ceil((width * bitCount) / 32) * 4;
    const maskRowBytes = Math.ceil(width / 32) * 4;
    const maskStart = pixelStart + (rowBytes * height);
    const out = Buffer.alloc(width * height * 4);
    for (let y = 0; y < height; y++) {
        const srcRow = pixelStart + ((height - 1 - y) * rowBytes);
        const maskRow = maskStart + ((height - 1 - y) * maskRowBytes);
        for (let x = 0; x < width; x++) {
            let red = 0, green = 0, blue = 0, alpha = 255;
            if (bitCount === 32) {
                const at = srcRow + (x * 4);
                if (at + 3 >= buf.length) return null;
                blue = buf[at]; green = buf[at + 1]; red = buf[at + 2];
                alpha = buf[at + 3];
            } else if (bitCount === 24) {
                const at = srcRow + (x * 3);
                if (at + 2 >= buf.length) return null;
                blue = buf[at]; green = buf[at + 1]; red = buf[at + 2];
            } else if (bitCount === 8 || bitCount === 4 || bitCount === 1) {
                const perByte = 8 / bitCount;
                const at = srcRow + Math.floor(x / perByte);
                if (at >= buf.length) return null;
                const shift = (perByte - 1 - (x % perByte)) * bitCount;
                const index = (buf[at] >> shift) & ((1 << bitCount) - 1);
                const pal = paletteStart + (index * 4);
                if (pal + 2 >= buf.length) return null;
                blue = buf[pal]; green = buf[pal + 1]; red = buf[pal + 2];
            } else {
                return null; // 16bpp and friends are not worth the trouble
            }
            // 32bpp icons carry their own alpha, everything else uses the mask
            if (bitCount !== 32) {
                const at = maskRow + Math.floor(x / 8);
                if (at < buf.length) {
                    const bit = (buf[at] >> (7 - (x % 8))) & 1;
                    alpha = bit === 1 ? 0 : 255;
                }
            }
            const dst = ((y * width) + x) * 4;
            out[dst] = red; out[dst + 1] = green;
            out[dst + 2] = blue; out[dst + 3] = alpha;
        }
    }
    return { width: width, height: height, data: out };
}

function decodeICO(buf) {
    const count = buf.readUInt16LE(4);
    if (count === 0) return null;
    const entries = [];
    for (let i = 0; i < count; i++) {
        const at = 6 + (i * 16);
        if (at + 16 > buf.length) break;
        entries.push({
            width: buf[at] === 0 ? 256 : buf[at],
            length: buf.readUInt32LE(at + 8),
            offset: buf.readUInt32LE(at + 12)
        });
    }
    // same rule as the link tags: smallest entry that is still >= 16px
    entries.sort((a, b) => {
        const aBig = a.width >= ICON_SIZE;
        const bBig = b.width >= ICON_SIZE;
        if (aBig !== bBig) return aBig ? -1 : 1;
        return aBig ? a.width - b.width : b.width - a.width;
    });
    for (const entry of entries) {
        const end = entry.offset + entry.length;
        if (entry.offset < 6 || end > buf.length) continue;
        const slice = buf.subarray(entry.offset, end);
        try {
            const image = isPNG(slice) ? decodePNG(slice) : decodeICOBitmap(slice);
            if (image) return image;
        } catch (e) { /* try the next entry */ }
    }
    return null;
}

function decodeImage(buf) {
    if (isPNG(buf)) return decodePNG(buf);
    if (isJPEG(buf)) return decodeJPEG(buf);
    if (isICO(buf)) return decodeICO(buf);
    return null; // SVG, GIF, HTML error pages, etc.
}

//// pixel processing ////

// JPEG has no alpha, so transparency is flattened onto a flat backing
// colour. Pale icons (white glyph on nothing) would vanish on white, so
// the backing colour follows the average brightness of the opaque pixels.
function backingColor(image) {
    let total = 0;
    let opaque = 0;
    for (let i = 0; i < image.data.length; i += 4) {
        if (image.data[i + 3] < 128) continue;
        total += (image.data[i] * 0.299) + (image.data[i + 1] * 0.587) +
            (image.data[i + 2] * 0.114);
        opaque++;
    }
    if (opaque === 0) return 255;
    return (total / opaque) > 200 ? 32 : 255;
}

// box-average down to ICON_SIZE, flattening alpha as it goes
function resizeToIcon(image) {
    const backing = backingColor(image);
    const out = new Uint8Array(ICON_SIZE * ICON_SIZE * 4);
    for (let y = 0; y < ICON_SIZE; y++) {
        const srcTop = Math.floor((y * image.height) / ICON_SIZE);
        const srcBottom = Math.max(srcTop + 1,
            Math.floor(((y + 1) * image.height) / ICON_SIZE));
        for (let x = 0; x < ICON_SIZE; x++) {
            const srcLeft = Math.floor((x * image.width) / ICON_SIZE);
            const srcRight = Math.max(srcLeft + 1,
                Math.floor(((x + 1) * image.width) / ICON_SIZE));
            let red = 0, green = 0, blue = 0, samples = 0;
            for (let sy = srcTop; sy < srcBottom; sy++) {
                for (let sx = srcLeft; sx < srcRight; sx++) {
                    const at = ((sy * image.width) + sx) * 4;
                    const alpha = image.data[at + 3] / 255;
                    red += (image.data[at] * alpha) + (backing * (1 - alpha));
                    green += (image.data[at + 1] * alpha) + (backing * (1 - alpha));
                    blue += (image.data[at + 2] * alpha) + (backing * (1 - alpha));
                    samples++;
                }
            }
            const dst = ((y * ICON_SIZE) + x) * 4;
            out[dst] = Math.round(red / samples);
            out[dst + 1] = Math.round(green / samples);
            out[dst + 2] = Math.round(blue / samples);
            out[dst + 3] = 255;
        }
    }
    return out;
}

// median cut: repeatedly split the colour box with the widest channel
// spread until MAX_COLORS boxes remain, then flatten each box to its mean.
// Answers the palette and one palette index per pixel, ready for the PNG.
function quantizeColors(rgba) {
    const pixels = [];
    for (let i = 0; i < rgba.length; i += 4) {
        pixels.push([rgba[i], rgba[i + 1], rgba[i + 2], 0]);
    }
    // the boxes are sorted in place while splitting, so they start from a
    // copy of the list -- the entries are still the same colour triples, so
    // tagging a box below is seen by the pixels in their original order
    let boxes = [pixels.slice()];
    while (boxes.length < MAX_COLORS) {
        let widest = -1;
        let target = -1;
        let channel = 0;
        boxes.forEach((box, index) => {
            if (box.length < 2) return;
            for (let ch = 0; ch < 3; ch++) {
                let low = 255, high = 0;
                box.forEach((px) => {
                    if (px[ch] < low) low = px[ch];
                    if (px[ch] > high) high = px[ch];
                });
                if ((high - low) > widest) {
                    widest = high - low;
                    target = index;
                    channel = ch;
                }
            }
        });
        if (target < 0 || widest <= 0) break; // nothing left worth splitting
        const box = boxes[target];
        box.sort((a, b) => a[channel] - b[channel]);
        const middle = Math.floor(box.length / 2);
        boxes.splice(target, 1, box.slice(0, middle), box.slice(middle));
    }
    // every pixel takes the mean of the box holding it, by index
    const palette = [];
    boxes.forEach((box, slot) => {
        let red = 0, green = 0, blue = 0;
        box.forEach((px) => { red += px[0]; green += px[1]; blue += px[2]; });
        palette.push(
            Math.round(red / box.length),
            Math.round(green / box.length),
            Math.round(blue / box.length)
        );
        box.forEach((px) => { px[3] = slot; });
    });
    return {
        indices: pixels.map((px) => px[3]),
        palette: palette,
        colors: boxes.length
    };
}

//// indexed PNG encoder ////

const CRC_TABLE = (() => {
    const table = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
        }
        table[n] = c;
    }
    return table;
})();

function crc32(buf) {
    let c = -1;
    for (let i = 0; i < buf.length; i++) {
        c = CRC_TABLE[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
    }
    return (c ^ -1) >>> 0;
}

// length + type + payload + CRC over type and payload
function pngChunk(type, data) {
    const out = Buffer.alloc(data.length + 12);
    out.writeUInt32BE(data.length, 0);
    out.write(type, 4, 'ascii');
    data.copy(out, 8);
    out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
    return out;
}

// colour type 3 PNG. Every scanline uses filter 0: the others predict from
// neighbouring bytes, which is worthless once those bytes are palette
// indices rather than colours -- measured, they cost 20-25% more here.
function encodeIndexedPNG(quantized) {
    // a palette of 16 or fewer packs two pixels to the byte
    const bitDepth = quantized.colors <= 16 ? 4 : 8;
    const perByte = 8 / bitDepth;
    const rowBytes = Math.ceil(ICON_SIZE / perByte);
    const raw = Buffer.alloc((rowBytes + 1) * ICON_SIZE);
    for (let y = 0; y < ICON_SIZE; y++) {
        const rowStart = y * (rowBytes + 1);
        raw[rowStart] = 0; // filter: none
        for (let x = 0; x < ICON_SIZE; x++) {
            const index = quantized.indices[(y * ICON_SIZE) + x];
            if (bitDepth === 8) {
                raw[rowStart + 1 + x] = index;
            } else {
                const shift = (perByte - 1 - (x % perByte)) * bitDepth;
                raw[rowStart + 1 + Math.floor(x / perByte)] |= index << shift;
            }
        }
    }
    const header = Buffer.alloc(13);
    header.writeUInt32BE(ICON_SIZE, 0);
    header.writeUInt32BE(ICON_SIZE, 4);
    header[8] = bitDepth;
    header[9] = 3; // colour type: indexed
    header[10] = 0; header[11] = 0; header[12] = 0;
    return Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
        pngChunk('IHDR', header),
        pngChunk('PLTE', Buffer.from(quantized.palette)),
        pngChunk('IDAT', zlib.deflateSync(raw, { level: 9, memLevel: 9 })),
        pngChunk('IEND', Buffer.alloc(0))
    ]);
}

// same alphabet the website's base64URLSafeEncode() produces
function base64URLSafe(buffer) {
    return buffer.toString('base64')
        .replace(/\+/g, '-')
        .replace(/\//g, '_')
        .replace(/=/g, '@');
}

//// entry point ////

// returns a URL-safe base64 PNG string, or "" when no icon could be built
async function runFaviconLookup(pageUrl, html) {
    try {
        const candidates = iconCandidates(html || '', pageUrl);
        for (const iconUrl of candidates) {
            let image = null;
            try {
                const bytes = await fetchIconBytes(iconUrl);
                if (bytes) image = decodeImage(bytes);
            } catch (e) { /* try the next candidate */ }
            if (!image || image.width < 1 || image.height < 1) continue;
            const quantized = quantizeColors(resizeToIcon(image));
            return base64URLSafe(encodeIndexedPNG(quantized));
        }
        return "";
    } catch (e) {
        return "";
    }
}

module.exports = { runFaviconLookup };
