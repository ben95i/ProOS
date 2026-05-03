const { execFile, exec } = require('child_process');
const fsPromises = require('fs/promises');
const util = require('util');

const execFileAsync = util.promisify(execFile);
const execAsync = util.promisify(exec);

async function exeCustom(ramDisk, arg1, arg2) {
    const firstArg = arg1.replace(/\n/g, "");
    const secondArg = arg2.replace(/\n/g, "");
    return "Pi Custom Script: " + firstArg + '|' + secondArg;
}

module.exports = { exeCustom };
