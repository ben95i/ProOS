const fsPromises = require('fs/promises');
const { execFile } = require('child_process');
const util = require('util');

const execFileAsync = util.promisify(execFile);

/**
 * Status Updater Module
 *
 * Reads STATS_FILE (read-only) and probes each host. Probes run in parallel
 * with a bounded concurrency of MAX_PARALLEL. Output preserves the original
 * file order and uses the format: HOST|STATE|NAME|ACTION
 */

const MAX_WAIT = '0.5';
const MAX_PARALLEL = 10;

const ONLINE = 'grnind';  // green indicator
const OFFLINE = 'blkind'; // black indicator

/**
 * Action handlers. Add new probe types here; anything unknown yields "".
 */
const ACTIONS = {
    async ping(host) {
        try {
            await execFileAsync(
                '/usr/bin/ping',
                ['-4', '-A', '-c', '1', '-i', MAX_WAIT, '-W', MAX_WAIT, host],
                { timeout: 5000 }
            );
            return ONLINE;
        } catch (err) {
            return OFFLINE;
        }
    },
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
