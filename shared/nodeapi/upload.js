//// Automate REST API - File Upload ////
//
// Receives files in 1MB chunks and rebuilds each one under UPLOAD_DIR with
// its original filename. Every file type is accepted except the few in
// BLOCKED_EXT; the other limits are the size of one file and how many
// uploads may be part way through at once.
//
// Mounted with one line in server.js:
//   const { registerUploadRoutes } = require('./upload');
//   registerUploadRoutes(app);
//
// The protocol, all on POST /api/upload:
//   ?action=start&file=<url-safe base64 name>&size=<bytes>  -> { id, name }
//   ?action=chunk&id=<id>&index=<n>   body: raw bytes       -> { received }
//   ?action=finish&id=<id>                                  -> { name, size }
//   ?action=abort&id=<id>                                   -> { ok: true }
//
// Chunks are appended to a .part file in UPLOAD_TEMP as they land, so a
// video is never held in memory. The index has to be the next one expected;
// a repeat of the chunk already written answers as though it landed, which
// is what makes the browser's retry safe.

const fsPromises = require('fs/promises');
const path = require('path');
const crypto = require('crypto');
const express = require('express');

//// settings ////

// Where finished files land, and the scratch folder beside it. The two are
// kept on the same filesystem so the finish step is a rename rather than a
// copy: the file appears under its own name whole, never half written.
const UPLOAD_DIR = '/mnt/upload';
const UPLOAD_TEMP = '/mnt/upload/.part';

const CHUNK_LIMIT = 2 * 1024 * 1024;            // largest chunk accepted (1MB + slack)
const MAX_FILE_BYTES = 1024 * 1024 * 1024;      // largest file accepted, 1GB
const MAX_OPEN_SESSIONS = 8;                    // uploads part way through at once
const SESSION_EXPIRE_MS = 30 * 60 * 1000;       // an abandoned upload is swept after this
const SWEEP_EVERY_MS = 5 * 60 * 1000;
const MAX_NAME_LENGTH = 120;
// File types that are never written, matched on the extension in lower case.
// Executables, and anything a web server might be talked into running.
const BLOCKED_EXT = new Set([
    // executables and libraries
    '.exe', '.dll', '.com', '.scr', '.msi', '.so',
    // scripts a shell or the host runs
    '.bat', '.cmd', '.vbs', '.ps1', '.sh',
    // server-side pages
    '.php', '.phtml', '.php5', '.php7', '.phar',
    // markup and scripts a browser runs
    '.html', '.htm', '.js', '.mjs'
]);

// id -> { name, size, received, index, tempPath, lastSeen }
const sessions = new Map();

//// filenames ////

// Cuts a browser filename down to something safe to write: no path, no
// separators, printable ASCII only, and a length the filesystem will take.
// safeFilename('../My Photo (1).JPG') -> 'My Photo (1).JPG'
function safeFilename(raw) {
    const withoutPath = String(raw).split(/[\\/]/).pop();
    const cleaned = withoutPath
        .normalize('NFD')
        .replace(/[\u0300-\u036f]/g, '')  // accents off their base letter
        .replace(/[^\x20-\x7E]/g, '')     // printable ASCII only
        .replace(/[<>:"|?*\x00-\x1F]/g, '')
        .replace(/\s{2,}/g, ' ')
        .replace(/^\.+/, '')              // no leading dots, no hidden files
        .trim();
    if (cleaned === '') {
        return '';
    }
    const ext = path.extname(cleaned).toLowerCase();
    const stem = path.basename(cleaned, path.extname(cleaned));
    if (stem === '') {
        return '';
    }
    // the extension is kept whole; the stem is what gets cut
    const roomForStem = MAX_NAME_LENGTH - ext.length;
    return stem.substring(0, roomForStem) + ext;
}

// A name nothing is using yet: 'a.jpg' -> 'a (1).jpg' when it is taken.
async function freeFilename(name) {
    const ext = path.extname(name);
    const stem = path.basename(name, ext);
    let candidate = name;
    let counter = 0;
    while (counter < 1000) {
        try {
            await fsPromises.access(path.join(UPLOAD_DIR, candidate));
        } catch (err) {
            return candidate; // nothing there, so the name is free
        }
        counter += 1;
        candidate = `${stem} (${counter})${ext}`;
    }
    // a thousand of the same name is not a real case; fall back to unique
    return `${stem}-${Date.now()}${ext}`;
}

//// sessions ////

async function ensureFolders() {
    await fsPromises.mkdir(UPLOAD_DIR, { recursive: true });
    await fsPromises.mkdir(UPLOAD_TEMP, { recursive: true });
}

function isSessionId(value) {
    return typeof value === 'string' && /^[a-f0-9]{32}$/.test(value);
}

// Drops a session and the part file behind it.
async function dropSession(id) {
    const session = sessions.get(id);
    if (!session) {
        return;
    }
    sessions.delete(id);
    try {
        await fsPromises.unlink(session.tempPath);
    } catch (err) {
        // already gone, which is the state this was aiming for
    }
}

// Clears out uploads a browser started and never finished.
async function sweepSessions() {
    const now = Date.now();
    const stale = [];
    sessions.forEach((session, id) => {
        if (now - session.lastSeen > SESSION_EXPIRE_MS) {
            stale.push(id);
        }
    });
    for (const id of stale) {
        await dropSession(id);
    }
}

const sweepTimer = setInterval(() => {
    sweepSessions().catch(() => {});
}, SWEEP_EVERY_MS);
// the sweeper must not be the reason the process stays alive
if (typeof sweepTimer.unref === 'function') {
    sweepTimer.unref();
}

//// the four actions ////

// Opens a session and answers its id. The name is settled here, before any
// bytes arrive, so two browsers uploading the same filename cannot both be
// told they got it.
async function startUpload(query) {
    const size = Number(query.size);
    if (!Number.isInteger(size) || size < 0 || size > MAX_FILE_BYTES) {
        return { status: 400, body: 'invalid file size' };
    }
    if (sessions.size >= MAX_OPEN_SESSIONS) {
        return { status: 429, body: 'too many uploads in progress' };
    }
    const base64In = String(query.file || '')
        .replace(/-/g, '+').replace(/_/g, '/').replace(/@/g, '=');
    const rawName = Buffer.from(base64In, 'base64').toString('utf8');
    const safeName = safeFilename(rawName);
    if (safeName === '') {
        return { status: 400, body: 'invalid filename' };
    }
    const ext = path.extname(safeName).toLowerCase();
    if (BLOCKED_EXT.has(ext)) {
        return { status: 415, body: `file type not accepted: ${ext}` };
    }
    await ensureFolders();
    const id = crypto.randomBytes(16).toString('hex');
    const name = await freeFilename(safeName);
    const tempPath = path.join(UPLOAD_TEMP, `${id}.part`);
    // an empty part file, so a chunk only ever appends
    await fsPromises.writeFile(tempPath, '');
    sessions.set(id, {
        name: name,
        size: size,
        received: 0,
        index: 0, // the chunk number expected next
        tempPath: tempPath,
        lastSeen: Date.now()
    });
    return { status: 200, body: { id: id, name: name } };
}

// Appends one chunk. The index has to be the one expected; the chunk just
// written is accepted again as a no-op, so a browser that never saw the
// answer can send it a second time.
async function chunkUpload(query, body) {
    const id = query.id;
    if (!isSessionId(id) || !sessions.has(id)) {
        return { status: 404, body: 'unknown upload' };
    }
    const session = sessions.get(id);
    const index = Number(query.index);
    if (!Number.isInteger(index) || index < 0) {
        return { status: 400, body: 'invalid chunk index' };
    }
    if (!Buffer.isBuffer(body) || body.length === 0) {
        return { status: 400, body: 'empty chunk' };
    }
    if (body.length > CHUNK_LIMIT) {
        return { status: 413, body: 'chunk too large' };
    }
    session.lastSeen = Date.now();
    if (index === session.index - 1) {
        // a repeat of the chunk already written: nothing to do
        return { status: 200, body: { received: session.received } };
    }
    if (index !== session.index) {
        return { status: 409, body: `expected chunk ${session.index}` };
    }
    const wouldHold = session.received + body.length;
    if (wouldHold > session.size) {
        await dropSession(id);
        return { status: 400, body: 'more data than the file declared' };
    }
    await fsPromises.appendFile(session.tempPath, body);
    session.received = wouldHold;
    session.index = index + 1;
    return { status: 200, body: { received: session.received } };
}

// Closes a session: the part file becomes the finished file. A part file
// that is not the length it said it would be is thrown away instead.
async function finishUpload(query) {
    const id = query.id;
    if (!isSessionId(id) || !sessions.has(id)) {
        return { status: 404, body: 'unknown upload' };
    }
    const session = sessions.get(id);
    if (session.received !== session.size) {
        await dropSession(id);
        return { status: 400, body: 'upload is incomplete' };
    }
    const finalPath = path.join(UPLOAD_DIR, session.name);
    try {
        // same filesystem, so this is a rename rather than a copy: the file
        // appears under its own name whole, never half written
        await fsPromises.rename(session.tempPath, finalPath);
    } catch (err) {
        await dropSession(id);
        return { status: 500, body: `could not save file: ${err.message}` };
    }
    sessions.delete(id);
    return { status: 200, body: { name: session.name, size: session.size } };
}

// The browser gave up, so the part file goes.
async function abortUpload(query) {
    const id = query.id;
    if (isSessionId(id)) {
        await dropSession(id);
    }
    return { status: 200, body: { ok: true } };
}

//// route ////

const ACTIONS = {
    start: (req) => startUpload(req.query),
    chunk: (req) => chunkUpload(req.query, req.body),
    finish: (req) => finishUpload(req.query),
    abort: (req) => abortUpload(req.query)
};

/**
 * Adds POST /api/upload to an express app.
 * registerUploadRoutes(app)
 */
function registerUploadRoutes(app) {
    app.post('/api/upload',
        express.raw({ type: '*/*', limit: CHUNK_LIMIT }),
        async (req, res) => {
            try {
                const handler = ACTIONS[req.query.action];
                if (!handler) {
                    return res.status(400).json('invalid upload action');
                }
                const answer = await handler(req);
                return res.status(answer.status).json(answer.body);
            } catch (err) {
                res.status(500).json(`upload error: ${err.message}`);
            }
        });
}

module.exports = { registerUploadRoutes, UPLOAD_DIR };
