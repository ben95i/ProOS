const { execFile } = require('child_process');
const { OPCLEDparser } = require('./ledparser');
const util = require('util');
const execFileAsync = util.promisify(execFile);

const PINS = {
    whiteLed: "12",
    frontLed: "26",
    pcPower: "16"
};

async function curlGpio(pin, state) {
    try {
        await fetch(`http://127.0.0.1:8770/gpio?pin=${pin}&state=${state}`, { 
            signal: AbortSignal.timeout(500) 
        });
    } catch (error) {
        console.error(`GPIO request failed for pin ${pin}:` + error.message);
    }
}

async function ztermCom(cmd) {
    try {
        const { stdout } = await execFileAsync('/usr/bin/ztermcom', [cmd]);
        return stdout;
    } catch (error) {
        const out = error.stdout || '';
        const err = error.stderr || '';
        return `${out} ${err} ${error.message}`.replace(/\s+/g, ' ').trim();
    }
}

async function exeCustom(ramDisk, arg1, arg2) {
    const firstArg = arg1.replace(/\n/g, "");
    const secondArg = arg2.replace(/\n/g, "");
    switch (firstArg) {
        case "front_btn":
            // Front Panel Button Press
            await curlGpio(PINS.frontLed, "HIGH");
            try {
                await OPCLEDparser(ramDisk, 'shuffle', '');
            } catch (e) {
                return('leds shuffle script failed:', e.message);
            }
            await curlGpio(PINS.frontLed, "LOW");
            return('OK');

        case "whtledon":
            // White Front LED On
            await curlGpio(PINS.whiteLed, "HIGH");
            return('OK');

        case "whtledoff":
            // White Front LED Off
            await curlGpio(PINS.whiteLed, "LOW");
            return('OK');

        case "whtled":
            // White Front LED Toggle
            await curlGpio(PINS.whiteLed, "TOGGLE");
            return('OK');

        case "extcom":
            // Pass command to external serial port
            return await ztermCom(secondArg);

        case "pc_power":
            // PC Power Trigger with 300ms delay
            await curlGpio(PINS.pcPower, "HIGH");
            await new Promise(resolve => setTimeout(resolve, 300)); // in ms
            await curlGpio(PINS.pcPower, "LOW");
            return('OK');

        default:
            return(`unrecognized LEDwall command: ${firstArg}`);
    }
}

module.exports = { exeCustom };
