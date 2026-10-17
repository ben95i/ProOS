const { execFile } = require('child_process');
const util = require('util');
const execFileAsync = util.promisify(execFile);

// column widths of the printed table
const COL_IP = 15;
const COL_HOST = 20;
const COL_VENDOR = 25;
const COL_MAC = 17;
const COL_SEPARATOR = ' ';

// pads or truncates text to a fixed width: formatCol('abc', 5) -> 'abc  '
function formatCol(text, width) {
    const safeText = String(text || '');
    if (safeText.length > width) {
        return safeText.substring(0, width - 3) + '...';
    }
    return safeText.padEnd(width, ' ');
}

// one table row from a device's four fields
function deviceRow(ip, hostname, vendor, mac) {
    return formatCol(ip, COL_IP) + COL_SEPARATOR +
           formatCol(hostname, COL_HOST) + COL_SEPARATOR +
           formatCol(vendor, COL_VENDOR) + COL_SEPARATOR +
           formatCol(mac, COL_MAC);
}

// nmap output -> [{ ip, hostname, vendor, mac }]
function parseNmapOutput(stdout) {
    const hostBlocks = stdout.split('Nmap scan report for ').slice(1);
    return hostBlocks.map(block => {
        const lines = block.split('\n');
        const firstLine = lines[0].trim();
        let ip = firstLine;
        let hostname = '';
        let vendor = '---';
        let mac = '---';
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

// Sorts by address, octet by octet. Anything that is not four numeric octets
// (a malformed address, or IPv6) is pushed to the bottom.
function compareByAddress(a, b) {
    const aParts = a.ip.split('.');
    const bParts = b.ip.split('.');
    if (aParts.length !== 4) return 1;
    if (bParts.length !== 4) return -1;
    const aOctets = aParts.map(Number);
    const bOctets = bParts.map(Number);
    for (let i = 0; i < 4; i++) {
        if (aOctets[i] === bOctets[i]) {
            continue;
        }
        if (isNaN(aOctets[i])) return 1;
        if (isNaN(bOctets[i])) return -1;
        return aOctets[i] - bOctets[i];
    }
    return 0; // the addresses are identical
}

/**
 * Scans a local network and returns a formatted string table.
 * runScanNetwork('192.168.1.0/24')
 * @param {string} targetRange - the IP CIDR range to scan
 * @returns {Promise<string>} the formatted table, or the failure message
 */
async function runScanNetwork(targetRange) {
    try {
        // execFileAsync is safe from shell injection, and the default 1MB
        // buffer is plenty for a /24 subnet
        const { stdout } = await execFileAsync('/usr/bin/sudo',
            ['/usr/bin/nmap', '-sn', targetRange]);
        const devices = parseNmapOutput(stdout);
        devices.sort(compareByAddress);
        const activeHosts = [];
        activeHosts.push(deviceRow('IP ADDRESS', 'HOSTNAME', 'MANUFACTURER', 'MAC ADDRESS'));
        const totalWidth = COL_IP + COL_HOST + COL_MAC + COL_VENDOR +
            (COL_SEPARATOR.length * 3);
        activeHosts.push('-'.repeat(totalWidth));
        devices.forEach(device => {
            activeHosts.push(deviceRow(device.ip, device.hostname, device.vendor, device.mac));
        });
        activeHosts.push(`Total active hosts: ${devices.length}`);
        return activeHosts.join('\n');
    } catch (error) {
        return (`Nmap scan failed: ${error.message}`);
    }
}

module.exports = { runScanNetwork };
