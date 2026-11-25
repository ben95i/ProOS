// LAN Scanner (Server > Options > Network > LAN Scanner): GET /api/netscan
// streams one scan of the LAN to the page as it happens, one JSON event per
// line. The tracked hosts in opts.json are checked at once, SSH first
// then a ping, and each is sent the moment its check ends; nmap runs beside
// them, and when it is done the hosts it found are sent, a tracked one again
// with its MAC and maker filled in. A page opening while a scan runs joins it.

const { execFile } = require('child_process');
const dns = require('dns').promises;
const net = require('net');
const util = require('util');
const { localPing } = require('./ping');
// The hosts every scan lists, up or down: ['ledwall.home', ...]
const { TRACKED_HOSTS } = require('./options');
const execFileAsync = util.promisify(execFile);

const ROUTE = '/api/netscan';
const NET_RANGE = '192.168.1.0/24';
// how long a tracked host's name may take to resolve
const LOOKUP_TIMEOUT = 3000; // in (ms)
// a hung nmap would hold every later scan, as they join the running one
const NMAP_TIMEOUT = 60000; // in (ms)
// A tracked host is first asked for SSH: an answer on port 22 that starts
// with the SSH banner within the timeout.
const SSH_PORT = 22;
const SSH_TIMEOUT = 1500; // in (ms)
const SSH_BANNER_PREFIX = 'SSH-';
// what localPing() answers for a host that replied
const PING_ANSWERED = '1';
// A host's key, which the page draws one row per: a tracked host by its
// name, any other by its address.
const TRACKED_KEY_PREFIX = 'tracked:';
const DEVICE_KEY_PREFIX = 'ip:';

// the scan in progress, joined by every request that arrives while it runs,
// so two presses never start two nmaps
let runningScan = null;

/**
 * One scan's events and the pages following it. Every line sent is kept, so
 * a page joining late is played them all before the live ones.
 */
class NetworkScan {
    constructor() {
        this.lines = [];
        this.viewers = new Set();
        this.finished = false;
    }

    // Sends an event to every page following and keeps it for later ones.
    // scan.send({ type: 'start', range: '192.168.1.0/24' })
    send(event) {
        if (this.finished) {
            return;
        }
        const line = JSON.stringify(event) + '\n';
        this.lines.push(line);
        for (const viewer of this.viewers) {
            if (!viewer.destroyed) {
                viewer.write(line);
            }
        }
    }

    // Sends the last event, 'done' or 'error', and ends every response.
    finish(event) {
        this.send(event);
        this.finished = true;
        for (const viewer of this.viewers) {
            viewer.end();
        }
        this.viewers.clear();
    }

    // Plays a page every event so far, then keeps it on the live ones until
    // the scan ends or the page goes.
    addViewer(res) {
        res.writeHead(200, {
            'Content-Type': 'application/x-ndjson',
            'Cache-Control': 'no-store',
            'X-Accel-Buffering': 'no'
        });
        if (res.socket) {
            res.socket.setNoDelay(true);
        }
        for (const line of this.lines) {
            res.write(line);
        }
        if (this.finished) {
            res.end();
            return;
        }
        this.viewers.add(res);
        res.on('close', () => {
            this.viewers.delete(res);
        });
    }
}

// The tracked hosts' names, each once whatever its case:
// ['ledwall.home', 'LEDwall.home', 'ktchpi.home'] -> ['ledwall.home', 'ktchpi.home']
function readTrackedHosts() {
    const names = [];
    const seenNames = new Set();
    TRACKED_HOSTS.forEach(name => {
        const nameKey = name.toLowerCase();
        if (seenNames.has(nameKey)) {
            return;
        }
        seenNames.add(nameKey);
        names.push(name);
    });
    return names;
}

// A host name's IPv4 addresses, or [] when it does not resolve in time.
// resolveHost('ledwall.home') -> ['192.168.1.12']
async function resolveHost(name) {
    let timer = null;
    const timeout = new Promise(resolve => {
        timer = setTimeout(() => resolve([]), LOOKUP_TIMEOUT);
    });
    const lookup = dns.lookup(name, { family: 4, all: true })
        .then(results => results.map(result => result.address))
        .catch(() => []);
    try {
        return await Promise.race([lookup, timeout]);
    } finally {
        clearTimeout(timer);
    }
}

// The tracked hosts' names, resolved all at once, each with the host record
// the page is sent, offline until a check or nmap finds it:
// -> [{ name: 'ledwall.home', addresses: ['192.168.1.12'], matched: false,
//       checked: null, host: { key: 'tracked:ledwall.home', ip: '192.168.1.12',
//       hostname: 'ledwall.home', vendor: null, mac: null, online: false,
//       tracked: true, ssh: false } }]
async function resolveTrackedHosts() {
    const names = readTrackedHosts();
    return Promise.all(names.map(async name => {
        const addresses = await resolveHost(name);
        let address = null;
        if (addresses.length > 0) {
            address = addresses[0];
        }
        return {
            name: name,
            addresses: addresses,
            // set once a device nmap found is this host
            matched: false,
            // its SSH and ping check, once started
            checked: null,
            host: {
                key: TRACKED_KEY_PREFIX + name.toLowerCase(),
                ip: address,
                hostname: name,
                vendor: null,
                mac: null,
                online: false,
                tracked: true,
                ssh: false
            }
        };
    }));
}

// nmap output -> [{ ip, hostname, vendor, mac }], vendor and mac null when
// nmap did not report them (the scanning host's own address has neither)
function parseNmapOutput(stdout) {
    const hostBlocks = stdout.split('Nmap scan report for ').slice(1);
    return hostBlocks.map(block => {
        const lines = block.split('\n');
        const firstLine = lines[0].trim();
        let ip = firstLine;
        let hostname = '';
        let vendor = null;
        let mac = null;
        // "name (1.2.3.4)" carries a hostname, a bare address does not
        if (firstLine.includes('(')) {
            const parts = firstLine.split(' (');
            hostname = parts[0];
            ip = parts[1].replace(')', '');
        }
        const macLine = lines.find(line => line.startsWith('MAC Address:'));
        if (macLine) {
            const macMatch = macLine.match(/MAC Address:\s+([0-9A-Fa-f:]+)/);
            if (macMatch) mac = macMatch[1];
            const vendorMatch = macLine.match(/\(([^)]+)\)/);
            if (vendorMatch) vendor = vendorMatch[1];
        }
        return { ip, hostname, vendor, mac };
    });
}

// True when the address answers on the SSH port with an SSH banner
// ("SSH-2.0-OpenSSH_10.0"), false when it refuses, says something else or
// stays quiet past the timeout. It never logs in.
// sshAvailable('192.168.1.12') -> true
function sshAvailable(ip) {
    return new Promise(resolve => {
        const socket = net.connect({ host: ip, port: SSH_PORT });
        let received = '';
        let settled = false;
        const finish = (answer) => {
            if (settled) {
                return;
            }
            settled = true;
            socket.destroy();
            resolve(answer);
        };
        socket.setTimeout(SSH_TIMEOUT, () => finish(false));
        socket.on('data', chunk => {
            received += chunk.toString('latin1');
            if (received.length >= SSH_BANNER_PREFIX.length) {
                finish(received.startsWith(SSH_BANNER_PREFIX));
            }
        });
        socket.on('error', () => finish(false));
        socket.on('close', () => finish(false));
    });
}

// sends one host's record as it stands
function sendHost(scan, host) {
    scan.send({ type: 'host', host: { ...host } });
}

// Checks one tracked host and sends it: SSH first (ssh: true, and up), then,
// when SSH does not answer, a ping. A name that did not resolve is sent as
// offline unchecked.
async function checkTrackedHost(scan, tracked) {
    const host = tracked.host;
    if (host.ip !== null) {
        host.ssh = await sshAvailable(host.ip);
        if (host.ssh) {
            host.online = true;
        } else {
            const pingAnswer = await localPing(host.ip);
            if (pingAnswer === PING_ANSWERED) {
                host.online = true;
            }
        }
    }
    sendHost(scan, host);
}

// True when a device nmap found is the tracked host: one of its addresses,
// or the name nmap found for it.
function isTrackedDevice(device, tracked) {
    if (tracked.addresses.includes(device.ip)) {
        return true;
    }
    const deviceName = device.hostname.toLowerCase();
    const trackedName = tracked.name.toLowerCase();
    return deviceName !== '' && deviceName === trackedName;
}

// Sends a device nmap found. A tracked one is sent again once its own check
// has been sent, with nmap's address, MAC and maker, and up, as nmap saw it;
// one whose name did not resolve is asked for SSH now it has an address.
async function sendDevice(scan, device, trackedHosts) {
    const tracked = trackedHosts.find(entry => !entry.matched && isTrackedDevice(device, entry));
    if (!tracked) {
        sendHost(scan, {
            key: DEVICE_KEY_PREFIX + device.ip,
            ...device,
            online: true,
            tracked: false,
            ssh: false
        });
        return;
    }
    tracked.matched = true;
    await tracked.checked;
    const host = tracked.host;
    const wasChecked = host.ip !== null;
    host.ip = device.ip;
    host.vendor = device.vendor;
    host.mac = device.mac;
    host.online = true;
    if (!wasChecked) {
        host.ssh = await sshAvailable(host.ip);
    }
    sendHost(scan, host);
}

// One scan, start to last host. nmap and the tracked hosts' checks run side
// by side; an nmap failure is thrown once the tracked hosts are all sent.
async function runScan(scan) {
    scan.send({ type: 'start', range: NET_RANGE });
    let nmapError = null;
    const nmapRun = execFileAsync('/usr/bin/sudo', ['/usr/bin/nmap', '-sn', NET_RANGE], {
        timeout: NMAP_TIMEOUT
    }).then(result => parseNmapOutput(result.stdout)).catch(error => {
        nmapError = error;
        return [];
    });
    const trackedHosts = await resolveTrackedHosts();
    const trackedChecks = trackedHosts.map(tracked => {
        tracked.checked = checkTrackedHost(scan, tracked);
        return tracked.checked;
    });
    const devices = await nmapRun;
    const deviceSends = devices.map(device => sendDevice(scan, device, trackedHosts));
    await Promise.all(trackedChecks.concat(deviceSends));
    if (nmapError !== null) {
        throw nmapError;
    }
}

/**
 * Mounts GET /api/netscan, which answers a scan as JSON lines:
 *   {"type":"start","range":"192.168.1.0/24"}
 *   {"type":"host","host":{"key":"tracked:ledwall.home","ip":"192.168.1.12",
 *     "hostname":"ledwall.home","vendor":"Raspberry Pi Trading",
 *     "mac":"B8:27:EB:00:00:01","online":true,"tracked":true,"ssh":true}}
 *   {"type":"done"}  or  {"type":"error","message":"Network scan failed: ..."}
 * A host is sent again whenever it changes, under the same key. Untracked
 * hosts are not checked (ssh: false); their keys are "ip:<address>".
 * @param {object} app - the Express app
 */
function registerNetscanRoutes(app) {
    app.get(ROUTE, (req, res) => {
        if (runningScan === null) {
            const scan = new NetworkScan();
            runningScan = scan;
            runScan(scan)
                .then(() => scan.finish({ type: 'done' }))
                .catch(error => scan.finish({
                    type: 'error',
                    message: `Network scan failed: ${error.message}`
                }))
                .finally(() => {
                    if (runningScan === scan) {
                        runningScan = null;
                    }
                });
        }
        runningScan.addViewer(res);
    });
}

module.exports = { registerNetscanRoutes };
