const fs = require('fs');
const fsPromises = require('fs/promises');
const { execFile } = require('child_process');
const util = require('util');

const execFileAsync = util.promisify(execFile);

/**
 * Status Updater Module
 */

const STATS_FILE = "/opt/system/statsmenu.txt";

/**
 * Replicates the ping logic to determine device state.
 * Returns "grnind" for online and "blkind" for offline.
 */
async function updateState(host, action) {
    if (action === "ping") {
        try {
            // Equivalent to ping -4 -A -c 1 -i 0.25 -W 0.25
            await execFileAsync('/usr/bin/ping', ['-4', '-A', '-c', '1', '-i', '0.25', '-W', '0.25', host]);
            return "grnind"; // Green indicator (Online)
        } catch (error) {
            return "blkind"; // Black indicator (Offline)
        }
    }
    return ""; 
}

/**
 * Main function to update device statuses.
 */
async function runStatusUpdate(ramDisk) {
    const LOCK_FILE = ramDisk + "/status.lock";
    // Instance Locking Logic
    try {
        if (fs.existsSync(LOCK_FILE)) {
            const pid = fs.readFileSync(LOCK_FILE, 'utf8').trim();
            try {
                // Check if process is still running
                process.kill(pid, 0); 
                return("already running, exiting...");
            } catch (e) {
                // Process not running, safe to remove lock
                fs.unlinkSync(LOCK_FILE);
            }
        }
        
        // Write current PID to lock file
        fs.writeFileSync(LOCK_FILE, process.pid.toString());

        // Read statsmenu.txt into memory
        if (!fs.existsSync(STATS_FILE)) {
            return "File not found.";
        }

        const data = await fsPromises.readFile(STATS_FILE, 'utf8');
        const lines = data.split('\n').filter(line => line.trim() !== "");
        const output = [];

        // Process each line
        for (const line of lines) {
            // Split by delimiter '|'
            const [host, name, action] = line.split('|');

            if (host && name && action) {
                const state = await updateState(host, action);
                
                // Construct the NEWLINE: HOST|STATE|NAME|ACTION
                const newLine = `${host}|${state}|${name}|${action}`;
                output.push(newLine);
            }
        }

        // Return the full output string
        return output.join('\n');

    } catch (err) {
        console.error("Status update exception:", err.message);
    } finally {
        // Cleanup lock file
        if (fs.existsSync(LOCK_FILE)) {
            try {
                const currentPid = fs.readFileSync(LOCK_FILE, 'utf8').trim();
                if (currentPid === process.pid.toString()) {
                    fs.unlinkSync(LOCK_FILE);
                }
            } catch (e) {
                // Lock file might have been removed already
            }
        }
    }
}

module.exports = { runStatusUpdate };