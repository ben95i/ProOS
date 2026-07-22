const { execFile } = require('child_process');
const util = require('util');
const { exeCustom } = require('./custom');

const execFileAsync = util.promisify(execFile);
const PROCESS_TIMEOUT = 5000;

async function executeMain(baseWWW, firstArg, secondArg) {
    if (firstArg === 'server') {
        try {
            const { stdout } = await execFileAsync('/usr/bin/sudo', ['/opt/rpi/servercmds.sh', secondArg], { timeout: PROCESS_TIMEOUT });
            return stdout;
        } catch (error) {
            return `pi server exception: ${error.message}|${error.stderr}`;
        }
    }
    if (firstArg === 'confwpa') {
        try {
            const { stdout } = await execFileAsync('/usr/bin/sudo', ['/opt/rpi/servercmds.sh', 'confwpa', secondArg], { timeout: PROCESS_TIMEOUT });
            return stdout;
        } catch (error) {
            return `pi confwpa exception: ${error.message}|${error.stderr}`;
        }
    }
    return await exeCustom(baseWWW, firstArg, secondArg);
}

module.exports = { executeMain };