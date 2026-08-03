const fsPromises = require('fs/promises');
const { execFile } = require('child_process');
const util = require('util');
const net = require('net');

const execFileAsync = util.promisify(execFile);

const MAX_PARALLEL = 12;
const MAX_WAIT = '1.0';
const SSH_PORT = 22;
const TCP_PROBE_TIMEOUT = 450; // in (ms)
const TCP_PROBE_ATTEMPTS = 2;
const ONLINE = 'grnind';  // green indicator
const OFFLINE = 'blkind'; // black indicator

/**
 * Probe a TCP port. A completed handshake is ONLINE; a definitive negative
 * (RST/refused, unreachable, DNS failure) is OFFLINE immediately with no
 * retry. Only a silent timeout is ambiguous — that's the wifi dropped-SYN
 * case — so that alone is retried. Never rejects; only ONLINE/OFFLINE escape.
 */
function tcpProbe(host) {
    const attempt = () => new Promise(resolve => {
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
        socket.once('timeout', () => finish('timeout')); // ambiguous may retry
        socket.on('error', () => finish(OFFLINE));        // definitive no retry
        socket.connect({ host, port: SSH_PORT, family: 4 });
    });
    return (async () => {
        for (let i = 0; i < TCP_PROBE_ATTEMPTS; i++) {
            const result = await attempt();
            if (result === ONLINE) return ONLINE;
            if (result === OFFLINE) return OFFLINE; // RST/unreachable done
            // 'timeout' silent, might be a dropped SYN loop and retry
        }
        return OFFLINE; // timed out on every attempt
    })();
}

/**
 * Status Updater Module
 *
 * Reads STATS_FILE (read-only) and probes each host. Probes run in parallel
 * with a bounded concurrency of MAX_PARALLEL. Output preserves the original
 * file order and uses the format: HOST|STATE|NAME|ACTION
 */

/**
 * Action handlers. Add new probe types here; anything unknown yields "".
 */
const ACTIONS = {
    async ping(host) {
        try {
            await execFileAsync(
                '/usr/bin/ping',
                ['-4', '-A', '-c', '1', '-i', MAX_WAIT, '-W', MAX_WAIT, host],
                { timeout: 3000 }
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

/**
 * Parse the stats file into ordered entry objects.
 */
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

/**
 * Run tasks with a fixed-size worker pool. Results are written back into
 * `results` at the entry's original index, so ordering is preserved.
 */
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

/**
 * Main entry point. Returns the full output string; never writes to disk.
 */
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
