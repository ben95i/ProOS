//// Automate REST API - Backup Wizard ////
//
// The backup wizard's routes. Its jobs run on the Proxmox host (pve/config/
// tank-backup.sh), started as every server command is: an empty trigger file
// ram/<command>.txt that the host's file-monitor claims and runs. The host
// writes back three files into the same RAM disk, which these routes read:
//   bkpjob.lock         "<job>|<pid>" while a job runs; one job at a time
//   bkpstatus.save.txt  every backup pool's state
//   bkplog.save.txt     the current job's log, begun afresh by each job: plain text
//                       lines and "@" event lines (@JOB, @PHASE, @STEP, @PROGRESS,
//                       @STEP_END, @POOL, @CANCEL, @REFUSED, @DONE; their fields are
//                       listed at the top of tank-backup.sh)
// None of it touches another command's output file.
//
// Mounted with one line in server.js:
//   const { registerBackupRoutes } = require('./backup');
//   registerBackupRoutes(app);
//
// GET  /api/backup/status
//   -> { running, state, updated, pools: [{ name, state, lastBackup, shares }] }
//      running: the job holding the lock ("standard") or null; state and pools
//      as the host last wrote them, a pool's state attached, unmounted,
//      connected or absent. With no status file yet (the host rebooted) a scan
//      is asked for, and updated is null until it lands.
// GET  /api/backup/log?id=<log id>&offset=<bytes read>
//   -> { id, offset, size, reset, text }
//      The log from offset on, whole lines only. A new job's log is a new file
//      with a new id; when the id asked for is not the current one, the answer
//      starts from the beginning with reset true. id is null with no log yet.
// POST /api/backup/<action>
//   attach (body: the passphrase, text/plain), standard, checksum,
//   standard_eject, checksum_eject, vms, detach, cancel, status
//   -> { result: 'started', action, requested }  requested: epoch ms, which the
//                                                job's @JOB line will not precede
//      { result: 'busy', running }               another job runs
//      { result: 'idle' }                        cancel with nothing running

const fsPromises = require('fs/promises');
const path = require('path');
const express = require('express');
const { param, query, validationResult } = require('express-validator');

//// settings ////

const ROUTE = '/api/backup';
// the host's RAM disk, bind-mounted into the automate LXC
const RAM_DIR = '/var/www/html/ram';
const LOCK_FILE = path.join(RAM_DIR, 'bkpjob.lock');
const STATUS_FILE = path.join(RAM_DIR, 'bkpstatus.save.txt');
const LOG_FILE = path.join(RAM_DIR, 'bkplog.save.txt');
const PASSWORD_FILE = path.join(RAM_DIR, 'bkppwd.save.txt');
// the most of the log one read answers
const MAX_LOG_CHUNK = 256 * 1024;
const MAX_PASSPHRASE_BYTES = 512;
const NEWLINE_BYTE = 0x0a;

// Each action's host command (the trigger file's name); a job is refused while
// another job runs, as the host would refuse it too.
const BACKUP_ACTIONS = {
    attach:         { command: 'bkp_attach', job: true },
    standard:       { command: 'bkp_standard', job: true },
    checksum:       { command: 'bkp_checksum', job: true },
    standard_eject: { command: 'bkp_standard_eject', job: true },
    checksum_eject: { command: 'bkp_checksum_eject', job: true },
    vms:            { command: 'bkp_vms', job: true },
    detach:         { command: 'bkp_detach', job: true },
    cancel:         { command: 'bkp_cancel', job: false },
    status:         { command: 'bkp_status', job: false }
};

//// host files ////

// A file's text, or null when it does not exist.
async function readTextOrNull(filePath) {
    try {
        return await fsPromises.readFile(filePath, 'utf8');
    } catch (error) {
        if (error.code === 'ENOENT') {
            return null;
        }
        throw error;
    }
}

// The job a lock file names: "standard|4242\n" -> 'standard'; null without one.
function jobOfLock(lockText) {
    if (lockText === null) {
        return null;
    }
    const jobName = lockText.trim().split('|')[0];
    if (jobName === '') {
        return 'busy';
    }
    return jobName;
}

// The job running now, from the lock file, or null.
async function runningJob() {
    const lockText = await readTextOrNull(LOCK_FILE);
    return jobOfLock(lockText);
}

// bkpstatus.save.txt text -> { state, updated, pools }
// "STATE|idle\nUPDATED|2026-11-27 14:03:22.1\nPOOL|hdd4tb-03|attached|2026-11-20 15:12|Data Regions Media"
// -> { state: 'idle', updated: '2026-11-27 14:03:22.1', pools: [{ name: 'hdd4tb-03',
//      state: 'attached', lastBackup: '2026-11-20 15:12', shares: ['Data', 'Regions', 'Media'] }] }
function parseStatus(statusText) {
    const status = { state: null, updated: null, pools: [] };
    statusText.split('\n').forEach(row => {
        const fields = row.trim().split('|');
        if (fields[0] === 'STATE') {
            status.state = fields[1];
        }
        if (fields[0] === 'UPDATED') {
            status.updated = fields[1];
        }
        if (fields[0] === 'POOL' && fields.length >= 4) {
            let shares = [];
            if (fields[4]) {
                shares = fields[4].split(' ').filter(share => share !== '');
            }
            status.pools.push({
                name: fields[1],
                state: fields[2],
                lastBackup: fields[3],
                shares: shares
            });
        }
    });
    return status;
}

// Drops the empty trigger file the host's file-monitor runs: ram/bkp_status.txt
async function triggerCommand(command) {
    await fsPromises.writeFile(path.join(RAM_DIR, `${command}.txt`), '');
}

// Writes the passphrase for bkp_attach, readable by its owner only; the job
// shreds it once read. An old file is removed first, so the mode is always set.
async function writePassphrase(passphrase) {
    await fsPromises.rm(PASSWORD_FILE, { force: true });
    await fsPromises.writeFile(PASSWORD_FILE, passphrase, { mode: 0o600, flag: 'wx' });
}

// The pools' state and the running job. A missing status file asks the host
// for a scan (unless one is already waiting).
async function readStatus() {
    const [statusText, lockText] = await Promise.all([
        readTextOrNull(STATUS_FILE),
        readTextOrNull(LOCK_FILE)
    ]);
    if (statusText === null) {
        const scanWaiting = await readTextOrNull(path.join(RAM_DIR, 'bkp_status.txt'));
        if (scanWaiting === null) {
            await triggerCommand(BACKUP_ACTIONS.status.command);
        }
    }
    const status = parseStatus(statusText || '');
    status.running = jobOfLock(lockText);
    return status;
}

// The log from an offset, whole lines only (see the route at the top). A line
// longer than one chunk, which the log never holds, is answered cut.
async function readLogChunk(requestedId, requestedOffset) {
    let logHandle;
    try {
        logHandle = await fsPromises.open(LOG_FILE, 'r');
    } catch (error) {
        if (error.code === 'ENOENT') {
            return { id: null, offset: 0, size: 0, reset: requestedId !== '', text: '' };
        }
        throw error;
    }
    try {
        const logStats = await logHandle.stat();
        const logId = String(logStats.ino);
        const logSize = logStats.size;
        let startOffset = requestedOffset;
        let reset = false;
        if (requestedId !== logId || startOffset > logSize) {
            startOffset = 0;
            reset = true;
        }
        const unreadBytes = logSize - startOffset;
        const readLength = Math.min(unreadBytes, MAX_LOG_CHUNK);
        const readBuffer = Buffer.alloc(readLength);
        const { bytesRead } = await logHandle.read(readBuffer, 0, readLength, startOffset);
        let chunk = readBuffer.subarray(0, bytesRead);
        const lastNewline = chunk.lastIndexOf(NEWLINE_BYTE);
        const chunkIsFull = bytesRead === MAX_LOG_CHUNK;
        // a line still being written waits for the next read
        if (lastNewline === -1 && !chunkIsFull) {
            chunk = chunk.subarray(0, 0);
        } else if (lastNewline !== -1) {
            chunk = chunk.subarray(0, lastNewline + 1);
        }
        return {
            id: logId,
            offset: startOffset + chunk.length,
            size: logSize,
            reset: reset,
            text: chunk.toString('utf8')
        };
    } finally {
        await logHandle.close();
    }
}

//// routes ////

// answers the validation errors, or null when the request is good
function badRequest(req, res) {
    const errs = validationResult(req);
    if (errs.isEmpty()) {
        return null;
    }
    return res.status(400).json({ errors: errs.array() });
}

// Starts an action (see the route at the top).
async function startAction(action, body) {
    const actionSpec = BACKUP_ACTIONS[action];
    const running = await runningJob();
    if (actionSpec.job && running !== null) {
        return { result: 'busy', running: running };
    }
    if (action === 'cancel' && running === null) {
        return { result: 'idle' };
    }
    if (action === 'attach') {
        await writePassphrase(body);
    }
    const requested = Date.now();
    await triggerCommand(actionSpec.command);
    return { result: 'started', action: action, requested: requested };
}

function registerBackupRoutes(app) {
    app.get(`${ROUTE}/status`, async (req, res) => {
        try {
            res.set('Cache-Control', 'no-store');
            res.json(await readStatus());
        } catch (error) {
            res.status(500).json(`backup status failed: ${error.message}`);
        }
    });

    app.get(`${ROUTE}/log`, [
        query('id').optional().matches(/^[0-9]*$/),
        query('offset').optional().isInt({ min: 0 })
    ], async (req, res) => {
        try {
            if (badRequest(req, res)) return;
            const requestedId = req.query.id || '';
            const requestedOffset = Number(req.query.offset || 0);
            res.set('Cache-Control', 'no-store');
            res.json(await readLogChunk(requestedId, requestedOffset));
        } catch (error) {
            res.status(500).json(`backup log read failed: ${error.message}`);
        }
    });

    app.post(`${ROUTE}/:action`, express.text({ type: '*/*', limit: '2kb' }), [
        param('action').isIn(Object.keys(BACKUP_ACTIONS))
    ], async (req, res) => {
        try {
            if (badRequest(req, res)) return;
            const action = req.params.action;
            let body = '';
            if (typeof req.body === 'string') {
                body = req.body;
            }
            if (action === 'attach') {
                if (body === '') {
                    return res.status(400).json('backup: passphrase missing');
                }
                if (Buffer.byteLength(body) > MAX_PASSPHRASE_BYTES) {
                    return res.status(400).json('backup: passphrase too long');
                }
            }
            res.json(await startAction(action, body));
        } catch (error) {
            res.status(500).json(`backup ${req.params.action} failed: ${error.message}`);
        }
    });
}

module.exports = { registerBackupRoutes };
