const { execFile } = require('child_process');
const { OPCLEDparser } = require('./ledparser');

const util = require('util');
const execFileAsync = util.promisify(execFile);

const PINS = {
    whiteLed: "12",
    frontLed: "26",
    pcPower: "16"
};

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

async function callGPIO(pin, state) {
    try {
        await fetch(`http://127.0.0.1:8770/gpio?pin=${pin}&state=${state}`, { 
            signal: AbortSignal.timeout(500) 
        });
    } catch (error) {
        console.error(`GPIO request failed for pin ${pin}:` + error.message);
    }
}

async function exeCustom(ramDisk, firstArg, secondArg) {
    switch (firstArg) {
        case "front_btn":
            // Front Panel Button Press
            await callGPIO(PINS.frontLed, "HIGH");
            try {
                await OPCLEDparser(ramDisk, 'shuffle', '');
            } catch (e) {
                return('leds shuffle script failed:', e.message);
            }
            await callGPIO(PINS.frontLed, "LOW");
            return('OK');

        case "whtledon":
            // White Front LED On
            await callGPIO(PINS.whiteLed, "HIGH");
            return('OK');

        case "whtledoff":
            // White Front LED Off
            await callGPIO(PINS.whiteLed, "LOW");
            return('OK');

        case "whtled":
            // White Front LED Toggle
            await callGPIO(PINS.whiteLed, "TOGGLE");
            return('OK');

        case "extcom":
            // Pass command to external serial port
            return await ztermCom(secondArg);

        case "pc_power":
            // PC Power Trigger with 300ms delay
            await callGPIO(PINS.pcPower, "HIGH");
            await new Promise(resolve => setTimeout(resolve, 300)); // in ms
            await callGPIO(PINS.pcPower, "LOW");
            return('OK');

        default:
            return(`unrecognized LEDwall command: ${firstArg}`);
    }
}

module.exports = { exeCustom };
