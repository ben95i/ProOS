// Status updater: reads statsmenu.txt and probes every host in it, answering
// one row per host as HOST|STATE|NAME|ACTION in the file's own order.

const fsPromises = require('fs/promises');
const { execFile } = require('child_process');
const util = require('util');
const net = require('net');

const execFileAsync = util.promisify(execFile);

const MAX_PARALLEL = 12;
const MAX_WAIT = '1.0';
const PING_TIMEOUT = 3000; // in (ms)
const SSH_PORT = 22;
const TCP_PROBE_TIMEOUT = 1000; // in (ms)
const TCP_PROBE_ATTEMPTS = 2;
const ONLINE = 'grnind';  // green indicator
const OFFLINE = 'blkind'; // black indicator
const AMBIGUOUS = 'timeout';

// One TCP connection attempt: ONLINE on a completed handshake, OFFLINE on a
// definitive negative (RST, unreachable, DNS failure), AMBIGUOUS on silence.
function tcpAttempt(host) {
    return new Promise(resolve => {
        const socket = new net.Socket();
        let settled = false;
        const finish = result => {
            if (settled) return;
            settled = true;
            socket.destroy();
            resolve(result);
        };
        socket.setTimeout(TCP_PROBE_TIMEOUT);
        socket.once('connect', () => finish(ONLINE));
        socket.once('timeout', () => finish(AMBIGUOUS));
        socket.on('error', () => finish(OFFLINE));
        socket.connect({ host, port: SSH_PORT, family: 4 });
    });
}

// Only a silent timeout is retried -- that is the wifi dropped-SYN case. A
// definitive negative is answered straight away. Never rejects.
async function tcpProbe(host) {
    for (let i = 0; i < TCP_PROBE_ATTEMPTS; i++) {
        const result = await tcpAttempt(host);
        if (result === ONLINE) return ONLINE;
        if (result === OFFLINE) return OFFLINE;
    }
    return OFFLINE; // timed out on every attempt
}

// Probe types. Add new ones here; anything unknown yields "".
const ACTIONS = {
    async ping(host) {
        try {
            await execFileAsync(
                '/usr/bin/ping',
                ['-4', '-A', '-c', '1', '-i', MAX_WAIT, '-W', MAX_WAIT, host],
                { timeout: PING_TIMEOUT }
            );
            return ONLINE;
        } catch (err) {
            return OFFLINE;
        }
    },
    async ssh(host) {
        return tcpProbe(host);
    }
};

async function updateState(host, action) {
    const handler = ACTIONS[action];
    if (!handler) return '';
    return handler(host);
}

// stats file -> ordered entries. Blank lines and # comments are dropped.
// "media.home|Media|ssh" -> { host, name, action }
function parseEntries(data) {
    return data
        .split('\n')
        .map(line => line.trim())
        .filter(line => line !== '' && !line.startsWith('#'))
        .map(line => {
            const [host, name, action] = line.split('|').map(f => (f || '').trim());
            return { host, name, action };
        })
        .filter(e => e.host && e.name && e.action);
}

// Runs the probes through a fixed-size worker pool. Results are written back
// at the entry's original index, so the file's ordering is preserved.
async function runPool(entries, limit) {
    const results = new Array(entries.length);
    let cursor = 0;
    async function worker() {
        while (true) {
            const i = cursor++;
            if (i >= entries.length) return;
            const { host, action } = entries[i];
            try {
                results[i] = await updateState(host, action);
            } catch (err) {
                results[i] = OFFLINE;
            }
        }
    }
    const workers = [];
    const count = Math.min(limit, entries.length);
    for (let i = 0; i < count; i++) workers.push(worker());
    await Promise.all(workers);
    return results;
}

// the whole menu as one string; never writes to disk
async function runStatusUpdate(baseWWW) {
    let data;
    try {
        data = await fsPromises.readFile(`${baseWWW}/menus/statsmenu.txt`, 'utf8');
    } catch (err) {
        if (err.code === 'ENOENT') return 'statsmenu.txt file not found.';
        throw err;
    }
    const entries = parseEntries(data);
    if (entries.length === 0) return '';
    const states = await runPool(entries, MAX_PARALLEL);
    return entries
        .map((e, i) => `${e.host}|${states[i]}|${e.name}|${e.action}`)
        .join('\n');
}

module.exports = { runStatusUpdate };
