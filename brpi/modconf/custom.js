const fs = require('fs');
const { execFile } = require('child_process');
const util = require('util');
const execFileAsync = util.promisify(execFile);

const PROCESS_TIMEOUT = 3000;
const MAX_QUEUE_SIZE = 5;

const USB_SERIAL = '/dev/ttyUSB0';
const HW_SERIAL = '/dev/ttyAMA0';

const LED_PIN = 12; // GPIO LED
const LED_FLASH_TIME = 80; // in (ms)
let isLEDflashing = false;

const SUDO = '/usr/bin/sudo';
const ZTERM = '/usr/bin/ztermcom';

const RELAX_CMD = '/opt/rpi/relaxrun.sh';
const RELAX_MENU = '/var/www/html/menus/relaxmenu.txt';
const RELAX_MENU_TAG = 'relaxmenu';
const RELAX_ACTIVE_TAG = 'relaxactive';
const RELAX_STOP_ROW = 'stop|relaxstop|Stop';

// Currently running relaxrun.sh command (arg2), or null when nothing is running.
let relaxState = null;

// Cached relaxmenu.txt contents, reloaded when the file's mtime changes.
let relaxMenuCache = { mtimeMs: null, entries: [] };

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

// --- relax menu / state -----------------------------------------------------

// Parse relaxmenu.txt into [{ command, display }]. Blank lines and lines
// starting with # are ignored. Result is cached until the file's mtime changes.
function loadRelaxMenu() {
    try {
        const { mtimeMs } = fs.statSync(RELAX_MENU);
        if (mtimeMs === relaxMenuCache.mtimeMs) return relaxMenuCache.entries;
        const entries = fs.readFileSync(RELAX_MENU, 'utf8')
            .split(/\r?\n/)
            .map(line => line.trim())
            .filter(line => line.length > 0 && !line.startsWith('#'))
            .map(line => {
                const parts = line.split('|');
                return {
                    command: (parts[0] || '').trim(),
                    display: parts.slice(1).join('|').trim()
                };
            })
            .filter(entry => entry.command.length > 0);
        relaxMenuCache = { mtimeMs, entries };
    } catch (error) {
        console.error(`Failed to read ${RELAX_MENU}: ${error.message}`);
    }
    return relaxMenuCache.entries;
}

function isKnownRelaxCommand(cmd) {
    const menu = loadRelaxMenu();
    // If the menu file can't be read at all, don't block playback on validation.
    if (menu.length === 0) return true;
    return menu.some(entry => entry.command === cmd);
}

// Stops relaxrun.sh and clears the tracked state. State is cleared on failure
// too, so a bad/absent process can never leave the UI stuck showing "active".
async function stopRelax() {
    try {
        await execFileAsync(SUDO, [RELAX_CMD, 'stop'], { timeout: PROCESS_TIMEOUT });
        return { ok: true };
    } catch (error) {
        return { ok: false, error: error.message };
    } finally {
        relaxState = null;
    }
}

// Enforces a single running instance: any existing sound is stopped first.
async function startRelax(cmd) {
    if (relaxState !== null) {
        const stopped = await stopRelax();
        if (!stopped.ok) {
            return `Failed to stop running relaxrun: ${stopped.error}`;
        }
    }
    try {
        const { stdout } = await execFileAsync(SUDO, [RELAX_CMD, cmd], { timeout: PROCESS_TIMEOUT });
        relaxState = cmd;
        return stdout;
    } catch (error) {
        relaxState = null; // not running
        return `relaxrun failed: ${error.message}`;
    }
}

// COMMAND|relaxmenu|DISPLAY_NAME  (or relaxactive for the running command).
// When one of the menu commands is active, "stop|relaxstop|Stop" is prepended.
function relaxDisplayMenu() {
    const menu = loadRelaxMenu();
    if (menu.length === 0) return `unable to read ${RELAX_MENU}`;
    const lines = menu.map(({ command, display }) =>
        `${command}|${command === relaxState ? RELAX_ACTIVE_TAG : RELAX_MENU_TAG}|${display}`);
    if (menu.some(({ command }) => command === relaxState)) {
        lines.unshift(RELAX_STOP_ROW);
    }
    return lines.join('\n');
}

// ---------------------------------------------------------------------------

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
            return;
        }
        case "relax": {
            const requested = String(arg2 || "").trim();
            if (!requested)  return "relax: no sound specified";
            if (!isKnownRelaxCommand(requested)) {
                return `relax: unknown sound "${requested}"`;
            }
            if (await powerStatus() === "1") {
                await coaxMode();
            } else {
                await ztermCom(USB_SERIAL, "01003");
            }
            return await startRelax(requested);
        }
        case "relaxmenu":    return relaxDisplayMenu();
        case "relaxstop": {
            const stopped = await stopRelax();
            if (!stopped.ok) return `Failed to stop relaxrun: ${stopped.error}`;
            await optAMode();
            return "OK";
        }
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
        case "ampstateoff": {
            await ztermCom(USB_SERIAL, "01002");
            const stopped = await stopRelax();
            if (!stopped.ok) return `Failed to stop relaxrun: ${stopped.error}`;
            return "OK";
        }
        case "amptogglepwr": {
            if (await powerStatus() === "1") {
                await ztermCom(USB_SERIAL, "01002");
                await stopRelax();
            } else {
                await onLastInput();
            }
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