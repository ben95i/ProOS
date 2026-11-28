const { execFile } = require('child_process');
const util = require('util');
const { exeCustom } = require('./custom');

const execFileAsync = util.promisify(execFile);
// A server command's output streams to the page from its own file (svrcmd.js), so the
// page does not wait on this answer and a slow command (wifiscan) may run its course.
const SERVER_CMD_TIMEOUT = 120000; // in (ms)

async function executeMain(baseWWW, firstArg, secondArg) {
    if (firstArg === 'server') {
        try {
            const { stdout } = await execFileAsync('/usr/bin/sudo', ['/opt/rpi/servercmds.sh', secondArg], { timeout: SERVER_CMD_TIMEOUT });
            return stdout;
        } catch (error) {
            return `pi server exception: ${error.message}|${error.stderr}`;
        }
    }
    if (firstArg === 'confwpa') {
        try {
            const { stdout } = await execFileAsync('/usr/bin/sudo', ['/opt/rpi/servercmds.sh', 'confwpa', secondArg], { timeout: SERVER_CMD_TIMEOUT });
            return stdout;
        } catch (error) {
            return `pi confwpa exception: ${error.message}|${error.stderr}`;
        }
    }
    return await exeCustom(baseWWW, firstArg, secondArg);
}

module.exports = { executeMain };