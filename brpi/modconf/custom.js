const { execFile } = require('child_process');
const util = require('util');
const execFileAsync = util.promisify(execFile);
const relaxScriptPath = '/opt/rpi/relaxrun.sh';

const PROCESS_TIMEOUT = 3000;
const MAX_QUEUE_SIZE = 50;

// Fixed Memory Leak: Bounded Async FIFO Queue
const fifoQueue = [];
let isQueueProcessing = false;

async function processQueue() {
    if (isQueueProcessing) return;
    isQueueProcessing = true;
    while (fifoQueue.length > 0) {
        const { executeTask, resolve, reject } = fifoQueue.shift();
        try {
            resolve(await executeTask());
        } catch (error) {
            reject(error);
        }
    }
    isQueueProcessing = false;
}

function enqueueTask(executeTask) {
    return new Promise((resolve, reject) => {
        if (fifoQueue.length >= MAX_QUEUE_SIZE) {
            return reject(new Error("Server busy: serial queue limit reached"));
        }
        fifoQueue.push({ executeTask, resolve, reject });
        processQueue();
    });
}

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
            // Added timeout to prevent serial deadlocks
            const { stdout } = await execFileAsync('/usr/bin/ztermcom', [cmd], { 
                timeout: PROCESS_TIMEOUT });
            return stdout;
        } catch (error) {
            return processProgramError(error);
        }
    };
    return enqueueTask(executeTask);
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
        if (ttyOut.length === 1) return ttyOut;
    }
    return "X";
}

async function exeCustom(ramDisk, arg1, arg2 = "") {
    switch (arg1) {
        case "relax": {
            if (await powerStatus() === "1") await coaxMode();
            else await ztermCom("01003");
            
            try {
                // Fixed Security Risk: No shell spawned, preventing command injection
                const { stdout } = await execFileAsync('/usr/bin/sudo', [relaxScriptPath, arg2], { timeout: 10000 });
                return stdout;
            } catch (e) {
                return `relaxrun failed: ${e.message}`;
            }
        }
        case "stoprelax":
            try {
                await execFileAsync('/usr/bin/sudo', [relaxScriptPath, 'stop'], { timeout: 5000 });
            } catch (e) {
                return `Failed to stop relaxrun: ${e.message}`;
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
                await execFileAsync('/usr/bin/sudo', [relaxScriptPath, 'stop'], { timeout: 5000 });
            } catch (e) {
                return `Failed to stop relaxrun: ${e.message}`;
            }
            return "OK";
        case "amptogglepwr": {
            if (await powerStatus() === "1") await ztermCom("01002");
            else await onLastInput();
            return "OK";
        }
        default:
            return "Bedroom Pi - Enter valid command.";
    }
}

module.exports = { exeCustom };