const { execFile, exec } = require('child_process');
const util = require('util');
const execFileAsync = util.promisify(execFile);


// Helper function to pad or truncate text to a fixed width
function formatCol(text, width) {
    const safeText = String(text || ''); 
    if (safeText.length > width) {
        return safeText.substring(0, width - 3) + '...';
    }
    return safeText.padEnd(width, ' ');
}

/**
 * Scans a local network and returns a formatted string table.
 * @param {string} targetRange - The IP CIDR range to scan (e.g., '192.168.1.0/24')
 * @returns {Promise<string>} The formatted network table
 */
async function runScanNetwork(targetRange) {
    try {
        // execFileAsync is safe from shell injection. 
        // Max buffer defaults to 1MB, which is plenty for a /24 subnet.
        const { stdout } = await execFileAsync('/usr/bin/sudo', ['/usr/bin/nmap', '-sn', targetRange]);

        const hostBlocks = stdout.split('Nmap scan report for ').slice(1);
        const devices = [];

        // 1. Parse Nmap Output
        for (const block of hostBlocks) {
            const lines = block.split('\n');
            const firstLine = lines[0].trim();
            
            let ip = '';
            let hostname = '';
            let vendor = '---';
            let mac = '---';
            
            // Extract IP and Hostname
            if (firstLine.includes('(')) {
                const parts = firstLine.split(' (');
                hostname = parts[0];
                ip = parts[1].replace(')', '');
            } else {
                ip = firstLine;
            }

            // Extract MAC Address and Vendor
            const macLine = lines.find(line => line.startsWith('MAC Address:'));
            if (macLine) {
                const macMatch = macLine.match(/MAC Address:\s+([0-9A-Fa-f:]+)/);
                if (macMatch) mac = macMatch[1];

                const vendorMatch = macLine.match(/\(([^)]+)\)/);
                if (vendorMatch) vendor = vendorMatch[1];
            }

            devices.push({ ip, hostname, vendor, mac });
        }

        // 2. Safe Numerical Sorting by IP
        devices.sort((a, b) => {
            const aParts = a.ip.split('.');
            const bParts = b.ip.split('.');

            // Fallback: If an IP is malformed or IPv6, push it to the bottom
            if (aParts.length !== 4) return 1;
            if (bParts.length !== 4) return -1;

            const aOctets = aParts.map(Number);
            const bOctets = bParts.map(Number);

            for (let i = 0; i < 4; i++) {
                if (aOctets[i] !== bOctets[i]) {
                    // Fallback: Handle NaN just in case the string was letters
                    if (isNaN(aOctets[i])) return 1;
                    if (isNaN(bOctets[i])) return -1;
                    return aOctets[i] - bOctets[i];
                }
            }
            return 0; // IPs are identical
        });

        // 3. Assemble the Visual Table
        const activeHosts = [];
        
        // Define exact character widths for the text
        const wIP = 15;
        const wHost = 15; // Expanded slightly so hostnames clip less often
        const wVendor = 21;
        const wMac = 19;  // Exactly enough for a MAC address (17 chars + 2 padding)

        // Explicit 1-space margin to insert between columns
        const colSeparator = ' '; 

        // Build Header
        const headerRow = formatCol('IP ADDRESS', wIP) + colSeparator +
                          formatCol('HOSTNAME', wHost) + colSeparator +
                          formatCol('MANUFACTURER', wVendor) + colSeparator +
                          formatCol('MAC ADDRESS', wMac);
        
        // Build a dashed line that matches the total width of columns + separators
        const totalWidth = wIP + wHost + wMac + wVendor + (colSeparator.length * 3);
        const dividerRow = '-'.repeat(totalWidth);

        activeHosts.push(headerRow);
        activeHosts.push(dividerRow);

        // Build Rows
        for (const device of devices) {
            const row = formatCol(device.ip, wIP) + colSeparator +
                        formatCol(device.hostname, wHost) + colSeparator +
                        formatCol(device.vendor, wVendor) + colSeparator +
                        formatCol(device.mac, wMac);
            
            activeHosts.push(row);
        }

        // Add Footer
        activeHosts.push(`Total active hosts: ${devices.length}`);
        
        return activeHosts.join('\n');
        
    } catch (error) {
        // Throw the error so the calling function knows the scan actually failed
        return(`Nmap scan failed: ${error.message}`);
    }
}

module.exports = { runScanNetwork };
