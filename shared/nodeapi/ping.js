// One ping, shared by controller.js (the PCs' power state) and netscan.js (the
// LAN Scanner's tracked hosts). Its own module, as the Pis replace controller.js.

const { execFile } = require('child_process');
const util = require('util');
const execFileAsync = util.promisify(execFile);

const PING_TIMEOUT = 3000;     // in (ms), the whole ping process
const MAX_PING_WAIT = "0.5";   // in (sec), for the one reply

/**
 * Pings an address once.
 * localPing('192.168.1.14') -> "1" when it answered, "0" when it did not
 * @param {string} ip - the IPv4 address or host name to ping
 * @returns {Promise<string>} "1" or "0"
 */
async function localPing(ip) {
    try {
        await execFileAsync('/usr/bin/ping',
            ['-4', '-A', '-c', '1', '-i', MAX_PING_WAIT, '-W', MAX_PING_WAIT, ip], {
            timeout: PING_TIMEOUT
        });
        return "1";
    } catch (error) {
        return "0";
    }
}

module.exports = { localPing };
