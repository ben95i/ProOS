const { execFile, exec } = require('child_process');
const util = require('util');
const execFileAsync = util.promisify(execFile);
const execAsync = util.promisify(exec);
const relaxCmd = '/usr/bin/sudo /opt/rpi/relaxrun.sh'

let serialQueue = Promise.resolve();

/**
 * Formats errors thrown by Node.js child_process operations
 * * @param {Error|any} error - The caught error object.
 * @param {string} [separator=' | '] - The string used to join the details. Use '\n' for multi-line.
 * @returns {string} A comprehensively formatted error string.
 */
function processProgramError(error, separator = ' : ') {
    if (!error) return 'Unknown error occurred';
    if (typeof error === 'string') return error;
    const stderr = (typeof error.stderr === 'string' ? error.stderr : '').trim();
    const stdout = (typeof error.stdout === 'string' ? error.stdout : '').trim(); 
    const code = error.code || 'UNKNOWN';
    const signal = error.signal || 'NONE';
    const baseMessage = (error.message || '').trim();
    return [
        `Command failed with code ${code} (Signal: ${signal})`,
        baseMessage ? `Message: ${baseMessage}` : null,
        stderr ? `stderr: ${stderr}` : null,
        stdout ? `stdout: ${stdout}` : null
    ].filter(Boolean).join(separator);
}

async function ztermCom(cmd) {
    const executeTask = async () => {
        try {
            const { stdout } = await execFileAsync('/usr/bin/ztermcom', [cmd]);
            return stdout;
        } catch (error) {
            return processProgramError(error);
        }
    };
    const task = serialQueue.then(() => executeTask());
    serialQueue = task.catch(() => {});
    return task;
}

const optAMode = async () => await ztermCom("02002");
const optBMode = async () => await ztermCom("02001");
const coaxMode = async () => await ztermCom("02003");
const powerStatus = async () => await decodeComResp("01005");
const onLastInput = async () => await ztermCom("01001");

async function decodeComResp(respCmd) {
    const ttyRaw = await ztermCom(respCmd);
    const firstDelim = ttyRaw.indexOf('|');
    const lastDelim = ttyRaw.lastIndexOf('|');
    if (firstDelim !== -1 && lastDelim !== -1 && firstDelim !== lastDelim) {
        const ttyOut = ttyRaw.substring(firstDelim + 1, lastDelim);
        if (ttyOut.length === 1) {
            return ttyOut;
        }
    }
    return "X";
}

/**
 * Main amplifier controller to be called from automation.js
 * @param {string} ramDisk - The path of the primary RAM disk
 * @param {string} arg1 - The primary command (e.g., 'relax', 'opt-a')
 * @param {string} arg2 - The secondary argument (used for 'relax')
 */
async function exeCustom(ramDisk, arg1, arg2 = "") {
    switch (arg1) {
        case "relax": {
            // Turn on amp and set input
            if (await powerStatus() === "1") {
                // Amp is online (set to coax input)
                await coaxMode();
            } else {
                // Amp is offline (power on then set to coax input)
                await ztermCom("01003");
            }
            try {
                return(await execAsync(`${relaxCmd} ${arg2}`));
            } catch (e) {
                return("relaxrun failed:", e.message);
            }
        }
        case "stoprelax":
            try {
                await execAsync(`${relaxCmd} stop`);
            } catch (e) {
                return("Failed to stop relaxrun:", e.message);
            }
            await optAMode();
            return "OK";
        case "opt-a":        return await optAMode();
        case "opt-b":        return await optBMode();
        case "coaxial":      return await coaxMode();
        case "aux":          return await ztermCom("02004");
        case "volup":        return await ztermCom("03002");
        case "voldwn":       return await ztermCom("03003");
        case "volmute":      return await ztermCom("03001");
        case "ampon-lastin": return await onLastInput();
        case "inputstate":   return await ztermCom("01006");
        case "volstate":     return await ztermCom("01007");
        case "ampstate":     return await powerStatus();        
        case "ampstateon":   return await ztermCom("01004");
        case "ampstateoff":
            await ztermCom("01002");
            try {
                await execAsync(`${relaxCmd} stop`);
            } catch (e) {
                return("Failed to stop relaxrun:", e.message);
            }
            return "OK";
        case "amptogglepwr": {
            if (await powerStatus() === "1") {
                await ztermCom("01002"); // Off
            } else {
                await onLastInput(); // On (Last Input)
            }
            return "OK";
        }
        default:
            return("Bedroom Pi - Enter valid command.");
    }
}

module.exports = { exeCustom };