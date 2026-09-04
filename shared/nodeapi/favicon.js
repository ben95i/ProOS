//// Automate REST API - Favicon Fetch ////
// by Ben Provenzano III //
//
// runFaviconLookup(pageUrl, html) -> a site's favicon as a URL-safe base64
// PNG, 24x24 pixels and 32 colours or fewer, or "" when none could be built.
// Never throws.
//
// The output is an indexed (colour type 3) PNG rather than a JPEG: at this
// size a 32 entry palette plus deflate is roughly a third of the bytes, and
// it holds the 32 colours exactly, which a JPEG's DCT cannot.
//
// Decoding is pure JavaScript, no native modules: ICO (BMP or embedded PNG),
// PNG and JPEG. Anything else (SVG, GIF) is skipped and the next candidate
// icon is tried instead.

const zlib = require('zlib');
const jpeg = require('jpeg-js');
const { PNG } = require('pngjs');

const ICON_SIZE = 24;       // output width / height in pixels
const MAX_COLORS = 32;      // palette size the image is reduced to
const FETCH_TIMEOUT = 6000; // per-request timeout in (ms)
const MAX_ICON_BYTES = 512 * 1024;
const USER_AGENT = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 ' +
    '(KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36';
// BT.601 weights, for brightness as the eye reads it
const LUMA_RED = 0.299;
const LUMA_GREEN = 0.587;
const LUMA_BLUE = 0.114;
const PALE_ICON_LUMA = 200; // above this the icon is backed with dark instead
const DARK_BACKING = 32;
const LIGHT_BACKING = 255;
const OPAQUE_ALPHA = 128;   // alpha at or above this counts as opaque

//// candidate icon URLs ////

// Every <link rel="...icon"> in the page markup, best first, then the
// well-known /favicon.ico. An icon at least as big as the target scales down
// cleanly, so the smallest of those is preferred, then the largest of what is
// left; SVG goes last because it cannot be decoded here.
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
    found.sort((a, b) => {
        if (a.svg !== b.svg) return a.svg ? 1 : -1;
        return compareBySize(a.size, b.size);
    });
    const urls = found.filter((item) => !item.svg).map((item) => item.url);
    urls.push(new URL('/favicon.ico', base).href);
    return [...new Set(urls)]; // de-duplicate, keeping order
}

// Sorts two icon sizes: anything at least ICON_SIZE comes first, smallest of
// those leading; below that, largest first.
function compareBySize(aSize, bSize) {
    const aBig = aSize >= ICON_SIZE;
    const bBig = bSize >= ICON_SIZE;
    if (aBig !== bBig) return aBig ? -1 : 1;
    if (aBig) return aSize - bSize;
    return bSize - aSize;
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
// every decoder returns { width, height, data }, where data is RGBA bytes

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

// One pixel out of a BMP scanline, by its bit depth. Answers null when the
// row runs past the end of the buffer.
function readBitmapPixel(buf, srcRow, x, bitCount, paletteStart) {
    if (bitCount === 32) {
        const at = srcRow + (x * 4);
        if (at + 3 >= buf.length) return null;
        return { red: buf[at + 2], green: buf[at + 1], blue: buf[at], alpha: buf[at + 3] };
    }
    if (bitCount === 24) {
        const at = srcRow + (x * 3);
        if (at + 2 >= buf.length) return null;
        return { red: buf[at + 2], green: buf[at + 1], blue: buf[at], alpha: 255 };
    }
    if (bitCount === 8 || bitCount === 4 || bitCount === 1) {
        const perByte = 8 / bitCount;
        const at = srcRow + Math.floor(x / perByte);
        if (at >= buf.length) return null;
        const shift = (perByte - 1 - (x % perByte)) * bitCount;
        const index = (buf[at] >> shift) & ((1 << bitCount) - 1);
        const pal = paletteStart + (index * 4);
        if (pal + 2 >= buf.length) return null;
        return { red: buf[pal + 2], green: buf[pal + 1], blue: buf[pal], alpha: 255 };
    }
    return null; // 16bpp and friends are not worth the trouble
}

// BITMAPINFOHEADER image as stored inside an .ico entry: bottom-up rows,
// padded to a 4 byte boundary, with a 1bpp transparency mask after them.
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
            const pixel = readBitmapPixel(buf, srcRow, x, bitCount, paletteStart);
            if (pixel === null) return null;
            let alpha = pixel.alpha;
            // 32bpp icons carry their own alpha, everything else uses the mask
            if (bitCount !== 32) {
                const at = maskRow + Math.floor(x / 8);
                if (at < buf.length) {
                    const bit = (buf[at] >> (7 - (x % 8))) & 1;
                    alpha = bit === 1 ? 0 : 255;
                }
            }
            const dst = ((y * width) + x) * 4;
            out[dst] = pixel.red;
            out[dst + 1] = pixel.green;
            out[dst + 2] = pixel.blue;
            out[dst + 3] = alpha;
        }
    }
    return { width: width, height: height, data: out };
}

// An .ico holds several images; the same size rule as the link tags picks one.
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
    entries.sort((a, b) => compareBySize(a.width, b.width));
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

// The output has no alpha, so transparency is flattened onto a flat backing
// colour. A pale icon (a white glyph on nothing) would vanish on white, so
// the backing follows the average brightness of the opaque pixels.
function backingColor(image) {
    let total = 0;
    let opaque = 0;
    for (let i = 0; i < image.data.length; i += 4) {
        if (image.data[i + 3] < OPAQUE_ALPHA) continue;
        total += (image.data[i] * LUMA_RED) + (image.data[i + 1] * LUMA_GREEN) +
            (image.data[i + 2] * LUMA_BLUE);
        opaque++;
    }
    if (opaque === 0) return LIGHT_BACKING;
    const averageLuma = total / opaque;
    if (averageLuma > PALE_ICON_LUMA) {
        return DARK_BACKING;
    }
    return LIGHT_BACKING;
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
                    const behind = backing * (1 - alpha);
                    red += (image.data[at] * alpha) + behind;
                    green += (image.data[at + 1] * alpha) + behind;
                    blue += (image.data[at + 2] * alpha) + behind;
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

// the box with the widest spread on any one channel, and which channel it is
function widestBox(boxes) {
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
            const spread = high - low;
            if (spread > widest) {
                widest = spread;
                target = index;
                channel = ch;
            }
        }
    });
    return { widest: widest, target: target, channel: channel };
}

// Median cut: repeatedly split the colour box with the widest channel spread
// until MAX_COLORS boxes remain, then flatten each box to its mean. Answers
// the palette and one palette index per pixel, ready for the PNG.
function quantizeColors(rgba) {
    const pixels = [];
    for (let i = 0; i < rgba.length; i += 4) {
        pixels.push([rgba[i], rgba[i + 1], rgba[i + 2], 0]);
    }
    // The boxes are sorted in place while splitting, so they start from a copy
    // of the list -- the entries are still the same colour triples, so tagging
    // a box below is seen by the pixels in their original order.
    let boxes = [pixels.slice()];
    while (boxes.length < MAX_COLORS) {
        const pick = widestBox(boxes);
        if (pick.target < 0 || pick.widest <= 0) break; // nothing left to split
        const box = boxes[pick.target];
        box.sort((a, b) => a[pick.channel] - b[pick.channel]);
        const middle = Math.floor(box.length / 2);
        boxes.splice(pick.target, 1, box.slice(0, middle), box.slice(middle));
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

// The palette indices, one scanline at a time, each with filter 0. The other
// filters predict from neighbouring bytes, which is worthless once those
// bytes are palette indices rather than colours -- measured, they cost 20-25%
// more here.
function pngScanlines(quantized, bitDepth) {
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
    return raw;
}

// colour type 3 (indexed) PNG
function encodeIndexedPNG(quantized) {
    // a palette of 16 or fewer packs two pixels to the byte
    const bitDepth = quantized.colors <= 16 ? 4 : 8;
    const raw = pngScanlines(quantized, bitDepth);
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

// Tries each candidate icon in turn and answers the first that decodes, as a
// URL-safe base64 PNG string, or "" when none of them could be built.
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
