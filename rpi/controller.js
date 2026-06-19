const { execFile } = require('child_process');
const util = require('util');
const { exeCustom } = require('./custom');

const execFileAsync = util.promisify(execFile);

async function executeMain(ramDisk, firstArg, secondArg) {
    if (firstArg === 'server') {
        try {
            const { stdout } = await execFileAsync('/usr/bin/sudo', [
              '/opt/rpi/servercmds.sh', secondArg]);
            return stdout;
        } catch (error) {
            return('pi server exception: ', error.message + '|' + error.stderr);
        }
    }
    if (firstArg === 'confwpa') {
        try {
            const { stdout } = await execFileAsync('/usr/bin/sudo', [
              '/opt/rpi/servercmds.sh', 'confwpa', secondArg]);
            return stdout;
        } catch (error) {
            return('pi confwpa exception: ', error.message + '|' + error.stderr);
        }
    }
    const cust_resp = await exeCustom(ramDisk, firstArg, secondArg);
    return cust_resp;
}

module.exports = { executeMain };
