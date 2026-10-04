//// fMP4 helpers for the camera relay ////
//
// ffmpeg hands camstream.js a fragmented MP4: an init segment (ftyp + moov)
// and then one moof + mdat pair per group of pictures. This file reads just
// enough of it for the relay to work without decoding anything:
//
//   BoxReader         cuts the byte stream from ffmpeg into whole boxes
//   readVideoConfig   the codec string the browser needs, from the moov
//   inspectFrame      does a fragment hold a keyframe, and a rotation
//
// Only box headers and the first byte of each NAL unit are looked at, plus
// the SEI messages until the rotation has been found, so the per-fragment cost
// is a handful of reads on data that is already in memory.

const BOX_HEADER_SIZE = 8;          // 32-bit size + type
const LARGE_BOX_HEADER_SIZE = 16;   // size field of 1, then a 64-bit size
const LARGE_BOX_SIZE_FLAG = 1;
const MAX_BOX_SIZE = 16 * 1024 * 1024; // a box this big means a broken stream

// AVCDecoderConfigurationRecord, the payload of the avcC box
const AVCC_TYPE = 'avcC';
const AVCC_PROFILE_OFFSET = 1;
const AVCC_COMPATIBILITY_OFFSET = 2;
const AVCC_LEVEL_OFFSET = 3;
const AVCC_LENGTH_SIZE_OFFSET = 4;
const AVCC_LENGTH_SIZE_MASK = 0x03;
const AVCC_MIN_SIZE = 5;

// H.264 NAL unit types and the SEI message the camera rotates with
const NAL_TYPE_MASK = 0x1f;
const NAL_TYPE_IDR = 5;
const NAL_TYPE_SEI = 6;
const SEI_DISPLAY_ORIENTATION = 47;        // H.264 Annex D.1.27
const SEI_SIZE_CONTINUES = 0xff;           // type/size bytes add up while 255
const DISPLAY_ORIENTATION_MIN_SIZE = 3;    // flags + 16-bit rotation
const ORIENTATION_CANCEL_BIT = 0x80;
const ORIENTATION_HFLIP_BIT = 0x40;
const ORIENTATION_VFLIP_BIT = 0x20;
const ROTATION_UNITS_PER_TURN = 65536;     // rotation is in 1/65536 turns
const DEGREES_PER_TURN = 360;
const DEGREES_PER_QUARTER = 90;
const HALF_TURN_DEGREES = 180;
const EMULATION_PREVENTION = Buffer.from([0x00, 0x00, 0x03]);

// Cuts a byte stream into whole top-level MP4 boxes and hands each to
// onBox(type, box). Incoming chunks are only joined once a whole box has
// arrived, so a large keyframe split over many pipe reads is copied once.
class BoxReader {
    constructor(onBox) {
        this.onBox = onBox;
        this.chunks = [];
        this.bufferedLength = 0;
        this.neededLength = BOX_HEADER_SIZE;
    }

    push(chunk) {
        this.chunks.push(chunk);
        this.bufferedLength += chunk.length;
        if (this.bufferedLength < this.neededLength) {
            return;
        }
        let data = this.chunks[0];
        if (this.chunks.length > 1) {
            data = Buffer.concat(this.chunks, this.bufferedLength);
        }
        this.chunks = [];
        this.bufferedLength = 0;
        this.neededLength = BOX_HEADER_SIZE;

        let offset = 0;
        while (offset < data.length) {
            const remaining = data.length - offset;
            const boxSize = readBoxSize(data, offset);
            if (boxSize === null) {
                this.neededLength = LARGE_BOX_HEADER_SIZE; // 64-bit size not in yet
                break;
            }
            if (boxSize === 0) {
                this.neededLength = BOX_HEADER_SIZE;       // header not in yet
                break;
            }
            if (remaining < boxSize) {
                this.neededLength = boxSize;
                break;
            }
            const boxEnd = offset + boxSize;
            const type = data.toString('latin1', offset + 4, offset + 8);
            this.onBox(type, data.subarray(offset, boxEnd));
            offset = boxEnd;
        }
        if (offset < data.length) {
            const rest = data.subarray(offset);
            this.chunks.push(rest);
            this.bufferedLength = rest.length;
        }
    }
}

// The size of the box starting at offset: 0 when its header has not fully
// arrived, null when its 64-bit size has not. Throws on sizes that cannot
// be right, which ends the relay instead of buffering without limit.
function readBoxSize(data, offset) {
    const remaining = data.length - offset;
    if (remaining < BOX_HEADER_SIZE) {
        return 0;
    }
    let boxSize = data.readUInt32BE(offset);
    let headerSize = BOX_HEADER_SIZE;
    if (boxSize === LARGE_BOX_SIZE_FLAG) {
        if (remaining < LARGE_BOX_HEADER_SIZE) {
            return null;
        }
        boxSize = Number(data.readBigUInt64BE(offset + BOX_HEADER_SIZE));
        headerSize = LARGE_BOX_HEADER_SIZE;
    }
    if (boxSize < headerSize || boxSize > MAX_BOX_SIZE) {
        throw new Error(`bad MP4 box size ${boxSize}`);
    }
    return boxSize;
}

// how many bytes of header a box has: 16 with a 64-bit size, else 8
function boxHeaderSize(box) {
    if (box.readUInt32BE(0) === LARGE_BOX_SIZE_FLAG) {
        return LARGE_BOX_HEADER_SIZE;
    }
    return BOX_HEADER_SIZE;
}

function toHexByte(value) {
    return value.toString(16).padStart(2, '0');
}

// Reads the H.264 setup out of the moov: the MediaSource codec string
// (avc1.PPCCLL -- profile, compatibility flags, level) and how many bytes
// prefix each NAL unit in the frames. null when there is no H.264 track.
function readVideoConfig(moov) {
    const typeOffset = moov.indexOf(AVCC_TYPE);
    if (typeOffset < 0) {
        return null;
    }
    const recordStart = typeOffset + AVCC_TYPE.length;
    if (recordStart + AVCC_MIN_SIZE > moov.length) {
        return null;
    }
    const profile = moov[recordStart + AVCC_PROFILE_OFFSET];
    const compatibility = moov[recordStart + AVCC_COMPATIBILITY_OFFSET];
    const level = moov[recordStart + AVCC_LEVEL_OFFSET];
    const lengthSizeMinusOne = moov[recordStart + AVCC_LENGTH_SIZE_OFFSET] & AVCC_LENGTH_SIZE_MASK;
    return {
        codec: `avc1.${toHexByte(profile)}${toHexByte(compatibility)}${toHexByte(level)}`,
        nalLengthSize: lengthSizeMinusOne + 1
    };
}

// Walks the NAL units of one fragment's mdat (its frames' NAL units, each
// with a length prefix, back to back). Answers whether it holds a keyframe
// and, when findRotation is set, the
// clockwise rotation its display orientation message asks for (null when
// it carries none).
function inspectFrame(mdat, nalLengthSize, findRotation) {
    let offset = boxHeaderSize(mdat);
    let isKeyframe = false;
    let rotation = null;
    while (offset + nalLengthSize <= mdat.length) {
        const nalSize = mdat.readUIntBE(offset, nalLengthSize);
        const nalStart = offset + nalLengthSize;
        const nalEnd = nalStart + nalSize;
        if (nalSize === 0 || nalEnd > mdat.length) {
            break;
        }
        const nalType = mdat[nalStart] & NAL_TYPE_MASK;
        if (nalType === NAL_TYPE_IDR) {
            isKeyframe = true;
        } else if (nalType === NAL_TYPE_SEI && findRotation && rotation === null) {
            rotation = readSeiRotation(mdat.subarray(nalStart + 1, nalEnd));
        }
        offset = nalEnd;
        // the keyframe leads the fragment: once it (and the rotation, if
        // wanted) is found, the rest of the frames need not be walked
        const hasEverything = isKeyframe && (!findRotation || rotation !== null);
        if (hasEverything) {
            break;
        }
    }
    return { isKeyframe, rotation };
}

// Takes out the 0x03 bytes the encoder inserts after two zero bytes, so
// the SEI fields read as written. Most SEI units have none, and those are
// returned as they are without a copy.
function removeEmulationPrevention(escaped) {
    if (escaped.indexOf(EMULATION_PREVENTION) < 0) {
        return escaped;
    }
    const unescaped = [];
    let zeroCount = 0;
    for (const byte of escaped) {
        const isPreventionByte = zeroCount >= 2 && byte === 0x03;
        if (isPreventionByte) {
            zeroCount = 0;
            continue;
        }
        unescaped.push(byte);
        if (byte === 0x00) {
            zeroCount += 1;
        } else {
            zeroCount = 0;
        }
    }
    return Buffer.from(unescaped);
}

// Reads one SEI value written as a run of 0xff bytes plus a last byte
// (payload type and payload size are both stored this way).
function readSeiNumber(rbsp, position) {
    let value = 0;
    while (position.offset < rbsp.length && rbsp[position.offset] === SEI_SIZE_CONTINUES) {
        value += SEI_SIZE_CONTINUES;
        position.offset += 1;
    }
    if (position.offset >= rbsp.length) {
        return null;
    }
    value += rbsp[position.offset];
    position.offset += 1;
    return value;
}

// Looks through the messages of one SEI NAL unit (header byte already
// skipped) for a display orientation message.
function readSeiRotation(escapedPayload) {
    const rbsp = removeEmulationPrevention(escapedPayload);
    const position = { offset: 0 };
    // the last byte is the rbsp stop bit, not a message
    const messagesEnd = rbsp.length - 1;
    while (position.offset < messagesEnd) {
        const payloadType = readSeiNumber(rbsp, position);
        const payloadSize = readSeiNumber(rbsp, position);
        if (payloadType === null || payloadSize === null) {
            return null;
        }
        const payloadEnd = position.offset + payloadSize;
        if (payloadEnd > rbsp.length) {
            return null;
        }
        if (payloadType === SEI_DISPLAY_ORIENTATION) {
            return readDisplayOrientation(rbsp.subarray(position.offset, payloadEnd));
        }
        position.offset = payloadEnd;
    }
    return null;
}

// Turns a display orientation message into whole clockwise degrees:
// 0, 90, 180 or 270. H.264 states the rotation anticlockwise in 1/65536
// turns. Both flips together are a half turn and are folded in; a single
// mirror flip has no rotation equivalent and is left out.
function readDisplayOrientation(payload) {
    if (payload.length < DISPLAY_ORIENTATION_MIN_SIZE) {
        return null;
    }
    const flagsByte = payload[0];
    if (flagsByte & ORIENTATION_CANCEL_BIT) {
        return 0;
    }
    // the 16-bit rotation starts three bits in: 5 bits, 8 bits, 3 bits
    const rotationHighBits = (flagsByte & 0x1f) << 11;
    const rotationMiddleBits = payload[1] << 3;
    const rotationLowBits = payload[2] >> 5;
    const rotationUnits = rotationHighBits | rotationMiddleBits | rotationLowBits;

    const anticlockwiseDegrees = rotationUnits * DEGREES_PER_TURN / ROTATION_UNITS_PER_TURN;
    const clockwiseDegrees = DEGREES_PER_TURN - anticlockwiseDegrees;
    const quarterTurns = Math.round(clockwiseDegrees / DEGREES_PER_QUARTER);
    let rotation = quarterTurns * DEGREES_PER_QUARTER;

    const isFlippedBothWays = (flagsByte & ORIENTATION_HFLIP_BIT) && (flagsByte & ORIENTATION_VFLIP_BIT);
    if (isFlippedBothWays) {
        rotation += HALF_TURN_DEGREES;
    }
    return rotation % DEGREES_PER_TURN;
}

module.exports = { BoxReader, readVideoConfig, inspectFrame };
