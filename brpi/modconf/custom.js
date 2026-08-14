const { execFile } = require('child_process');
const util = require('util');
const execFileAsync = util.promisify(execFile);

const PROCESS_TIMEOUT = 3500;
const MAX_QUEUE_SIZE = 10;

const USB_SERIAL = '/dev/ttyUSB0';
const HW_SERIAL = '/dev/ttyAMA0';

const LED_PIN = 12; // GPIO LED
const LED_FLASH_TIME = 80; // in (ms)
let isLEDflashing = false;

const SUDO = '/usr/bin/sudo';
const ZTERM = '/usr/bin/ztermcom';
const RELAX = '/opt/rpi/relaxrun.sh';

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

// FIFO queue
async function ztermCom(port, cmd) {
    const executeTask = async () => {
        try { // serial transmission
            const { stdout } = await execFileAsync(ZTERM, [port, cmd], { timeout: PROCESS_TIMEOUT });
            return stdout;
        } catch (error) {
            return processProgramError(error);
        }
    };
    return enqueueTask(executeTask);
}

async function callGPIO(pin, state) {
    try {
        await fetch(`http://127.0.0.1:8770/gpio?pin=${pin}&state=${state}`, { 
            signal: AbortSignal.timeout(500) 
        });
    } catch (error) {
        console.error(`GPIO request failed for pin ${pin}: ${error.message}`);
    }
}

async function flashLED() {
    if (isLEDflashing) return;
    isLEDflashing = true;
    await callGPIO(LED_PIN, "HIGH");
    await new Promise(resolve => setTimeout(resolve, LED_FLASH_TIME));
    await callGPIO(LED_PIN, "LOW");
    isLEDflashing = false;
}

function padOctet(value) {
  return String(value).padStart(3, "0");
}

function hexToRgb(hex_in) {
  const hex = hex_in.replace(/^#/, '');
  return { 
    red: padOctet(parseInt(hex.substring(0, 2), 16)), 
    green: padOctet(parseInt(hex.substring(2, 4), 16)), 
    blue: padOctet(parseInt(hex.substring(4, 6), 16)) 
  };
}

const optAMode = async () => await ztermCom(USB_SERIAL, "02002");
const optBMode = async () => await ztermCom(USB_SERIAL, "02001");
const coaxMode = async () => await ztermCom(USB_SERIAL, "02003");
const powerStatus = async () => await decodeComResp(USB_SERIAL, "01005");
const onLastInput = async () => await ztermCom(USB_SERIAL, "01001");

async function decodeComResp(port, cmd) {
    const ttyRaw = await ztermCom(port, cmd);
    const firstDelim = ttyRaw.indexOf('|');
    const lastDelim = ttyRaw.lastIndexOf('|');
    if (firstDelim !== -1 && lastDelim !== -1 && firstDelim !== lastDelim) {
        const ttyOut = ttyRaw.substring(firstDelim + 1, lastDelim);
        if (ttyOut.length === 1) return ttyOut;
    }
    return "invalid response: " + ttyRaw;
}

async function exeCustom(ramDisk, arg1, arg2 = "") {
    flashLED();
    switch (arg1) {
        case "bed_leds_hex_set": {
            const color = hexToRgb(arg2);
            await ztermCom(HW_SERIAL, `12${padOctet(color.red)}`);
            await ztermCom(HW_SERIAL, `13${padOctet(color.green)}`);
            await ztermCom(HW_SERIAL, `14${padOctet(color.blue)}`);
            await ztermCom(HW_SERIAL, '11002');
            return;
        }
        case "relax": {
            if (await powerStatus() === "1") {
                await coaxMode();
            } else {
                await ztermCom(USB_SERIAL, "01003");
            }
            try {
                const { stdout } = await execFileAsync(SUDO, [RELAX, arg2], { timeout: PROCESS_TIMEOUT });
                return stdout;
            } catch (e) {
                return `relaxrun failed: ${e.message}`;
            }
        }
        case "stoprelax":
            try {
                await execFileAsync(SUDO, [RELAX, 'stop'], { timeout: PROCESS_TIMEOUT });
            } catch (e) {
                return `Failed to stop relaxrun: ${e.message}`;
            }
            await optAMode();
            return "OK";
        case "opt-a":        return await optAMode();
        case "opt-b":        return await optBMode();
        case "coaxial":      return await coaxMode();
        case "aux":          return await ztermCom(USB_SERIAL, "02004");
        case "volup":        return await ztermCom(USB_SERIAL, "03002");
        case "voldwn":       return await ztermCom(USB_SERIAL, "03003");
        case "volmute":      return await ztermCom(USB_SERIAL, "03001");
        case "ampon-lastin": return await onLastInput();
        case "inputstate":   return await ztermCom(USB_SERIAL, "01006");
        case "volstate":     return await ztermCom(USB_SERIAL, "01007");
        case "ampstate":     return await powerStatus();        
        case "ampstateon":   return await ztermCom(USB_SERIAL, "01004");
        case "ampstateoff":
            await ztermCom(USB_SERIAL, "01002");
            try {
                await execFileAsync(SUDO, [RELAX, 'stop'], { timeout: PROCESS_TIMEOUT });
            } catch (e) {
                return `Failed to stop relaxrun: ${e.message}`;
            }
            return "OK";
        case "amptogglepwr": {
            if (await powerStatus() === "1") await ztermCom(USB_SERIAL, "01002");
            else await onLastInput();
            return "OK";
        }
        case "hwcom":
            return await ztermCom(HW_SERIAL, arg2);
        case "usbcom":
            return await ztermCom(USB_SERIAL, arg2);
        default:
            return "Bedroom Pi - Enter valid command.";
    }
}

module.exports = { exeCustom };