const { execFile } = require('child_process');
const { OPCLEDparser } = require('./ledparser');
const util = require('util');

const execFileAsync = util.promisify(execFile);

const PINS = {
    whiteLed: "12",
    frontLed: "26",
    pcPower: "16"
};

const SERIAL_PORT = '/dev/ttyAMA0';

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

/**
 * Formats errors thrown by Node.js child_process operations
 * @param {Error|any} error - The caught error object.
 * @param {string} [separator=' : '] - The string used to join the details.
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

// FIFO queue
async function ztermCom(cmd) {
    const executeTask = async () => {
        try { // serial transmission
            const { stdout } = await execFileAsync('/usr/bin/ztermcom', [SERIAL_PORT, cmd], { timeout: PROCESS_TIMEOUT });
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

let isPcPowerToggling = false;

async function exeCustom(ramDisk, firstArg, secondArg) {
    switch (firstArg) {
        case "front_btn":
            await callGPIO(PINS.frontLed, "HIGH");
            try {
                await OPCLEDparser(ramDisk, 'shuffle', '');
            } catch (e) {
                return `leds shuffle script failed: ${e.message}`;
            }
            await callGPIO(PINS.frontLed, "LOW");
            return 'OK';

        case "whtledon":
            await callGPIO(PINS.whiteLed, "HIGH");
            return 'OK';

        case "whtledoff":
            await callGPIO(PINS.whiteLed, "LOW");
            return 'OK';

        case "whtled":
            await callGPIO(PINS.whiteLed, "TOGGLE");
            return 'OK';

        case "extcom":
            return await ztermCom(secondArg);

        case "pc_power":
            if (isPcPowerToggling) return 'Server busy: PC power sequence currently active';
            isPcPowerToggling = true;
            try {
                await callGPIO(PINS.pcPower, "HIGH");
                await new Promise(resolve => setTimeout(resolve, 300));
                await callGPIO(PINS.pcPower, "LOW");
            } finally {
                // Guaranteed to release the lock even if the GPIO fetch fails
                isPcPowerToggling = false; 
            }
            return 'OK';

        default:
            return `unrecognized LEDwall command: ${firstArg}`;
    }
}

module.exports = { exeCustom };