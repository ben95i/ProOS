/////////////////////////////////////////////////
////// Automate Home Automation Server API //////
///////////// by Ben Provenzano III /////////////
/////////////////////////////////////////////////

const { execFile } = require('child_process');
const fsPromises = require('fs/promises');
const path = require('path');
const util = require('util');
const execFileAsync = util.promisify(execFile);
const PROCESS_TIMEOUT = 3000;  // in (ms)
const MAX_PING_WAIT = "0.5"; // in (sec)
const LOCAL_DOMAIN = "home";
const BRPC_MAC = "90:2e:16:46:86:43";
const NET_RANGE = "192.168.1.0/24";
const LOCAL_IP = {
    lrxmit: "192.168.1.12",
    desk: "192.168.1.14",
    brpi: "192.168.1.15",
    brpc: "192.168.1.17",
    picolamp1: "192.168.1.18"
};

// imports //
const { OPCLEDparser } = require('./ledparser');
const { runStatusUpdate } = require('./statusupd');
const { runSiteLookup } = require('./sitelookup');
const { runWeatherWidget } = require('./weather');
const { runScanNetwork } = require('./netscan');

const fifoQueue = [];
const MAX_QUEUE_SIZE = 20;
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
            return reject(new Error("Server busy: queue limit reached"));
        }
        fifoQueue.push({ executeTask, resolve, reject });
        processQueue();
    });
}

function isNumeric(value) {
    return typeof value === 'string' && value.trim() !== '' && Number.isInteger(Number(value));
}

function splitByDelimiter(str, delimiter = '-') {
    if (!str) return ['', ''];
    const parts = str.split(delimiter);
    const before = parts[0] || '';
    const after = parts.length > 1 ? parts.slice(1).join(delimiter) : '';
    return [before, after];
}

function extractResponse(str, delimiter = '|') {
    if (!str) return '';
    const outstr = str.replace(/[\\"\n]/g, '');
    if (!outstr || delimiter == null) return outstr;
    const first = outstr.indexOf(delimiter);
    if (first === -1) return outstr;
    const last = outstr.lastIndexOf(delimiter);
    return last > first
        ? outstr.substring(first + 1, last) 
        : outstr.substring(first + 1);
}

function padOctet(value) {
  return String(value).padStart(3, "0");
}

function hexToRgb(hexString) {
  const cleanHex = hexString.replace(/^#/, '');
  const redDecimal = padOctet(parseInt(cleanHex.substring(0, 2), 16));
  const greenDecimal = padOctet(parseInt(cleanHex.substring(2, 4), 16));
  const blueDecimal = padOctet(parseInt(cleanHex.substring(4, 6), 16));
  return { red: redDecimal, green: greenDecimal, blue: blueDecimal };
}

async function localPing(ip) {
    try {
        await execFileAsync('/usr/bin/ping', ['-4', '-A', '-c', '1', '-i', MAX_PING_WAIT, '-W', MAX_PING_WAIT, ip], { 
            timeout: PROCESS_TIMEOUT 
        });
        return "1"; 
    } catch (error) {
        return "0";
    }
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
        `Command failed with code ${code} ,signal: ${signal}`,
        baseMessage ? `Message: ${baseMessage}` : null,
        stderr ? `stderr: ${stderr}` : null,
        stdout ? `stdout: ${stdout}` : null
    ].filter(Boolean).join(separator);
}

class RoomManager {
    constructor() {
        this.roomState = null;
        this.devices = [];
    }
    addDevice(name, value) {
        let _status = null;
        if (value === 0 || value === '0') _status = false;
        else if (isNumeric(value)) _status = true; 
        this.devices.push({
            name: name,
            value: value,
            status: _status
        });
    }
    updateRoomState() {
        this.roomState = this.devices.some(device => device.status === true);
    }
    getData() {
        return {
            roomState: this.roomState,
            devices: this.devices
        };
    }
}

async function commonAPIRequest(server, delim) {
    try {
        const response = await fetch(server, { 
            signal: AbortSignal.timeout(PROCESS_TIMEOUT)});
        if (!response.ok) 
            return "commonAPIRequest: invalid response";
        return extractResponse(await response.text(), delim);
    } catch (error) {
        return "commonAPIRequest exception: " + error.message;
    }
}

async function callApi(target, arg1, arg2 = '') {
    if (arg1 === "") return "callApi: argument cannot be empty";
    return await commonAPIRequest(`http://${target}:80/api?var=${arg2}&arg=${arg1}&action=main`, '|');
}

async function callPico(target, arg1, arg2 = '') {
    if (arg1 === "") return "callPico: argument cannot be empty";
    return await commonAPIRequest(`http://${target}:4000/api?${arg1},,${arg2}~`, null);
}

async function callFIFO(host, cmd, out_pos = '') {
    const executeTask = async () => {
        let out = '';
        if (host === "lrxmit") {
            out = await callApi(LOCAL_IP.lrxmit, "extcom", cmd);
        } else if (host === "local") {
            try {
                const { stdout } = await execFileAsync('/usr/bin/ztermcom', [cmd], { 
                    timeout: PROCESS_TIMEOUT 
                });
                out = extractResponse(stdout);
            } catch (error) {
                out = processProgramError(error);
            }
        } else {
            return 'invalid host';
        }
        if (out_pos === '') return out;
        if (out && out.length === 1) return out;
        if (out && out.length === 3) return out[parseInt(out_pos)] === "9" ? "1" : "0";
        return `invalid response: ${out}`;
    };
    return enqueueTask(executeTask);
}

function parseInputData(input, marker = '_set') {
  const idx = input.indexOf(marker);
  if (idx !== -1) {
    const var1 = input.slice(0, idx + marker.length);       // includes marker
    const var2 = input.slice(idx + marker.length + 1);      // skip marker + "_"
    return { var1, var2 };
  }
  return { var1: input, var2: '' };
}

async function brXmit(arg1, arg2 = '') {
    switch (arg1) {
        case "bed_adj_up":       return await callFIFO("local", "11041");
        case "bed_adj_dwn":      return await callFIFO("local", "11042");
        case "bed_adj_stop":     return await callFIFO("local", "11043");
        case "bed_adj_sts":      return await callFIFO("local", "11044");

        case "bed_leds":         return await callFIFO("local", "11009");
        case "bed_leds_sts":     return await callFIFO("local", "11008");
        case "bed_leds_rb":      return await callFIFO("local", "11004");
        case "bed_leds_cs":      return await callFIFO("local", "11005");
        case "bed_leds_off":     return await callFIFO("local", "11000");
        case "bed_leds_pause":   return await callFIFO("local", "11006");
        case "bed_leds_set":     return await callFIFO("local", `16${padOctet(arg2)}`);
        case "bed_leds_hex_set": {
            const rgb = hexToRgb(arg2);
            await callFIFO("local", `12${rgb.red}`);
            await callFIFO("local", `13${rgb.green}`);
            await callFIFO("local", `14${rgb.blue}`);
            return await callFIFO("local", "11002");
        }
    
        case "brlamp1":        return await callFIFO("local", "10007", "0");
        case "brlamp1on":      return await callFIFO("local", "10002");
        case "brlamp1off":     return await callFIFO("local", "10001");

        case "brmacs":         return await callFIFO("local", "10007", "1");
        case "brmacson":       return await callFIFO("local", "10004");
        case "brmacsoff":      return await callFIFO("local", "10003");

        case "retropi":        return await callFIFO("local", "10007", "2");
        case "retropion":      return await callFIFO("local", "10006");
        case "retropioff":     return await callFIFO("local", "10005");

        case "roomth":         return await callFIFO("local", "01005");

        case "brtv":           return await callFIFO("local", "11033", "0");
        case "brtvoff":        return await callFIFO("local", "11032");
        case "brtvon":         return await callFIFO("local", "11031");

        case "brpc":           return await localPing(LOCAL_IP.brpc);
        case "brpcoff": 
            if (await localPing(LOCAL_IP.brpc) === "1") {
              return await callApi(LOCAL_IP.brpc, "sleep");
            }
            return `${LOCAL_IP.brpc} is already offline`;
        case "brpcon": 
            if (await localPing(LOCAL_IP.brpc) === "1") {
              return `${LOCAL_IP.brpc} is already online`;
            }
            return await execFileAsync('/usr/bin/wakeonlan', [BRPC_MAC], { 
                timeout: PROCESS_TIMEOUT 
            }).then(() => "OK").catch(e => processProgramError(e));

        default: return "brXmit: invalid command";
    }
}

async function windowLamp1Status() { 
    return await callPico(LOCAL_IP.picolamp1, "4", "0"); 
}
async function windowLamp1Off() { 
    return await callPico(LOCAL_IP.picolamp1, "1", "2");
}
async function windowLamp1On() { 
    return await callPico(LOCAL_IP.picolamp1, "1", "3");
}

async function lrXmit(arg1, arg2 = '') {
    switch (arg1) {
        case "wl1_dual":      return await windowLamp1Status();
        case "wl1_dual_set":  return await callPico(LOCAL_IP.picolamp1, "3", arg2);     
        case "wl1_dualoff":   return await windowLamp1Off();
        case "wl1_dualon":    return await windowLamp1On();
        case "wl1_led1off":   return await windowLamp1Off();
        case "wl1_led1on":    return await windowLamp1On();
        case "wl1_led1":      return await callPico(LOCAL_IP.picolamp1, "1", "0");
        case "wl1_led2off":   return await callPico(LOCAL_IP.picolamp1, "2", "0");
        case "wl1_led2on":    return await callPico(LOCAL_IP.picolamp1, "2", "1");
        case "wl1_led2":      return await callPico(LOCAL_IP.picolamp1, "2", "2");

        case "wkststate":     return await localPing(LOCAL_IP.desk);
        case "wkststateoff":  return await callApi(LOCAL_IP.lrxmit, "pc_power");
        case "wkststateon":   return await callApi(LOCAL_IP.lrxmit, "pc_power");

        case "hifistate":     return await callFIFO("lrxmit", "15019");
        case "hifistateoff":  return await callFIFO("lrxmit", "15018");
        case "hifistateon":   return await callFIFO("lrxmit", "15017");
        case "hifitoggle":    return await callFIFO("lrxmit", "15016");
        case "aux":           return await callFIFO("lrxmit", "15002");
        case "phono":         return await callFIFO("lrxmit", "15003");
        case "usb":           return await callFIFO("lrxmit", "15001");
        case "coax":          return await callFIFO("lrxmit", "15004");
        case "opt":           return await callFIFO("lrxmit", "15005");
        case "curinput":      return await callFIFO("lrxmit", "15007");
        case "autodac":       return await callFIFO("lrxmit", "15006");
        case "vlimit":        return await callFIFO("lrxmit", "15015");
        case "togglehpf":     return await callFIFO("lrxmit", "15014");
        case "curhpfvollim":  return await callFIFO("lrxmit", "15020");
        case "voldisplay":    return await callFIFO("lrxmit", "15013");
        case "volmute":       return await callFIFO("lrxmit", "15012");
        case "voldwn":        return await callFIFO("lrxmit", "15010");
        case "volup":         return await callFIFO("lrxmit", "15011");
        case "voldwnc":       return await callFIFO("lrxmit", "15008");
        case "volupc":        return await callFIFO("lrxmit", "15009");
        case "submute":       return await callFIFO("lrxmit", "17001");
        case "subup":         return await callFIFO("lrxmit", "17002");
        case "subdwn":        return await callFIFO("lrxmit", "17003");
        case "subup_fast":    return await callFIFO("lrxmit", "17004");
        case "subdwn_fast":   return await callFIFO("lrxmit", "17005");
        default: return "lrXmit: invalid command";
    }
}

async function executeMain(baseWWW, firstArg, secondArg) {
    switch (firstArg) {
        case "lightson":
            Promise.all([
                lrXmit("wl1_led1on"), 
                lrXmit("wl1_led2on"), 
                brXmit("brlamp1on"),
                brXmit("bed_leds_rb"),
            ]).catch(() => {});
            return "OK";
        case "lightsoff":
            Promise.all([
                lrXmit("wl1_led1off"), 
                lrXmit("wl1_led2off"), 
                brXmit("brlamp1off"),
                brXmit("bed_leds_off"),
            ]).catch(() => {});
            OPCLEDparser(baseWWW, "stop");
            return "OK";
        case "lrxmit": 
            const lrcmds = parseInputData(secondArg);
            return await lrXmit(lrcmds.var1, lrcmds.var2);
        case "lrstatus": {
            const lrRoom = new RoomManager();
            const [wlRaw, wkst, hifi] = await Promise.all([
                windowLamp1Status(),
                lrXmit("wkststate"),
                lrXmit("hifistate")
            ]);
            const [wl1, wl2] = splitByDelimiter(wlRaw, '~');
            lrRoom.addDevice('Window Lamp', wl1);
            lrRoom.addDevice('Lamp Backlight', wl2);
            lrRoom.addDevice('Desktop PC', wkst);
            lrRoom.addDevice('Stereo System', hifi);
            lrRoom.updateRoomState();
            return lrRoom.getData();
        }
        case "lrlightson":
            lrXmit("wl1_led1on");
            return "OK";
        case "lrlightsoff":
            lrXmit("wl1_led1off");
            return "OK";
        case "lron":
            Promise.all([
                lrXmit("wl1_led1on"), 
                lrXmit("wl1_led2on"), 
                lrXmit("wkststateon"), 
                lrXmit("hifistateon")
            ]).catch(() => {});
            OPCLEDparser(baseWWW, "abstract");
            return "OK";
        case "lroff":
            Promise.all([
                lrXmit("wl1_led1off"),
                lrXmit("wl1_led2off"), 
                lrXmit("wkststateoff"), 
                lrXmit("hifistateoff")
            ]).catch(() => {});
            OPCLEDparser(baseWWW, "stop");
            return "OK";
        case "brxmit":
            const brcmds = parseInputData(secondArg);
            return await brXmit(brcmds.var1, brcmds.var2);
        case "brstatus": {
            const brRoom = new RoomManager();
            const [lamp, bedleds, retro, macs, tv, pc, amp] = await Promise.all([
                brXmit("brlamp1"),
                brXmit("bed_leds_sts"),
                brXmit("retropi"),
                brXmit("brmacs"),
                brXmit("brtv"),
                brXmit("brpc"),
                callApi(LOCAL_IP.brpi, "ampstate")
            ]);
            brRoom.addDevice('Desk Lamp', lamp);
            brRoom.addDevice('Bed Lights', bedleds);
            brRoom.addDevice('RetroPi TV', retro);
            brRoom.addDevice('Vintage Macs', macs);
            brRoom.addDevice('Bedroom TV', tv);
            brRoom.addDevice('Bedroom PC', pc);
            brRoom.addDevice('Audio System', amp);
            brRoom.updateRoomState();
            return brRoom.getData();
        }
        case "bron":
            Promise.all([
                brXmit("bed_leds_rb"),
                brXmit("brlamp1on"), 
                brXmit("retropion"),
                brXmit("brmacson"), 
                brXmit("brtvon"), 
                brXmit("brpcon"), 
                callApi(LOCAL_IP.brpi, "ampstateon")
            ]).catch(() => {});
            return "OK";
        case "broff":
            Promise.all([
                brXmit("bed_leds_off"),
                brXmit("brlamp1off"), 
                brXmit("retropioff"), 
                brXmit("brmacsoff"), 
                brXmit("brtvoff"), 
                callApi(LOCAL_IP.brpc, "sleep"), 
                callApi(LOCAL_IP.brpi, "ampstateoff")
            ]).catch(() => {});
            return "OK";
        case "brpi":
            return await callApi(LOCAL_IP.brpi, secondArg);
        case "relax":
            Promise.all([
                callApi(LOCAL_IP.brpi, "relax", secondArg), 
                brXmit("brtvoff"), 
                callApi(LOCAL_IP.brpc, "sleep")
            ]).catch(() => {});
            return "OK";
        case "stop-br":
            return await callApi(LOCAL_IP.brpi, "stoprelax");
        case "localping":
            return await localPing(`${secondArg}.${LOCAL_DOMAIN}`);
        case "status": {
            try { return await runStatusUpdate(baseWWW); } 
            catch (exc) { return "status update exception: " + exc.message; }
        }
        case "sitelookup":
            return await runSiteLookup(secondArg);
        case "server": {
            const logFile = path.join(baseWWW, '/ram/sysout.txt');
            if (!secondArg) return "empty server command!";
            if (secondArg === "netscan") {
                await fsPromises.appendFile(logFile, `Scanning network range ${NET_RANGE}...\n`);
                const scanResult = await runScanNetwork(NET_RANGE);
                await fsPromises.appendFile(logFile, `${scanResult}\n`);
                return "OK";
            }
            await fsPromises.appendFile(logFile, `\n${secondArg} sent.\n`);
            await fsPromises.writeFile(path.join(baseWWW, `/ram/${secondArg}.txt`), ''); 
            return "OK";
        }
        case "weather": {
            try {
                return await runWeatherWidget();
            } catch (err) {
                return `weatherWidget error: ${err.message}`;
            }
        }
        default: return "executeMain: invalid command";
    }
}

module.exports = { executeMain };