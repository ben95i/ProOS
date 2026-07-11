/////////////////////////////////////////////////
////// Automate Home Automation Server API //////
///////////// by Ben Provenzano III /////////////
/////////////////////////////////////////////////

const { execFile } = require('child_process');
const fsPromises = require('fs/promises');
const path = require('path');
const util = require('util');
const execFileAsync = util.promisify(execFile);
const API_CALL_TIMEOUT = 2500; // in (ms)
const PROCESS_TIMEOUT = 3000;  // in (ms)
const MAX_PING_WAIT = "0.5"; // in (sec)
const LOCAL_DOMAIN = "home";
const NET_RANGE = "192.168.1.0/24";
const BRPC_MAC = "90:2e:16:46:86:43";
const IPs = {
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

async function localPing(ip) {
    try {
        await execFileAsync('/usr/bin/ping', ['-4', '-A', '-c', '1', '-i', MAX_PING_WAIT, '-W', MAX_PING_WAIT, ip], { timeout: PROCESS_TIMEOUT });
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
        `Command failed with code ${code} (Signal: ${signal})`,
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
            signal: AbortSignal.timeout(API_CALL_TIMEOUT) });
        if (!response.ok) 
            return "commonAPIRequest: invalid response";
        return extractResponse(await response.text(), delim);
    } catch (error) {
        return "commonAPIRequest exception: " + error.message;
    }
}

async function callApi(target, arg1, arg2 = '') {
    if (arg1 === "") return "callApi: argument cannot be empty";
    const server = `http://${target}:80/api?var=${arg2}&arg=${arg1}&action=main`;
    return await commonAPIRequest(server, '|');
}

async function callPico(target, arg1, arg2 = '') {
    if (arg1 === "") return "callPico: argument cannot be empty";
    const server = `http://${target}:4000/api?${arg1},,${arg2}~`;
    return await commonAPIRequest(server, null);
}

async function callFIFO(host, cmd, out_pos = '') {
    const executeTask = async () => {
        let out = '';
        if (host === "lrxmit") {
            out = await callApi(IPs.lrxmit, "extcom", cmd);
        } else if (host === "local") {
            try {
                const { stdout } = await execFileAsync('/usr/bin/ztermcom', [cmd], { timeout: PROCESS_TIMEOUT });
                out = extractResponse(stdout);
            } catch (error) {
                out = processProgramError(error);
            }
        } else {
            return 'invalid host';
        }

        if (out_pos === '') return out;
        if (out && out.length === 3) return out[parseInt(out_pos)] === "9" ? "1" : "0";
        if (out && out.length === 1) return out;
        return `invalid response: ${out}`;
    };
    return enqueueTask(executeTask);
}

async function brXmit(cmd) {
    switch (cmd) {
        case "brlamp1":    return await callFIFO("local", "10007", "0");
        case "brlamp1on":  return await callFIFO("local", "10002");
        case "brlamp1off": return await callFIFO("local", "10001");
        case "brmacs":     return await callFIFO("local", "10007", "1");
        case "brmacson":   return await callFIFO("local", "10004");
        case "brmacsoff":  return await callFIFO("local", "10003");
        case "retropi":    return await callFIFO("local", "10007", "2");
        case "retropion":  return await callFIFO("local", "10006");
        case "retropioff": return await callFIFO("local", "10005");
        case "roomth":     return await callFIFO("local", "01005");
        case "brtv":       return await callFIFO("local", "01003", "0");
        case "brtvoff":    return await callFIFO("local", "01002");
        case "brtvon":     return await callFIFO("local", "01001");
        case "brpc":       return await localPing(IPs.brpc);
        case "brpcoff": 
            if (await localPing(IPs.brpc) === "1") return await callApi(IPs.brpc, "sleep");
            return `${IPs.brpc} is already offline`;
        case "brpcon": 
            if (await localPing(IPs.brpc) === "1") return `${IPs.brpc} is already online`;
            return await execFileAsync('/usr/bin/wakeonlan', [BRPC_MAC], { timeout: PROCESS_TIMEOUT }).then(() => "OK").catch(e => processProgramError(e));
        default: return "brXmit: invalid command";
    }
}

async function windowLamp1Status() { 
    return await callPico(IPs.picolamp1, "4", "0"); 
}

async function lrXmit(arg1, arg2 = '') {
    switch (arg1) {
        case "wl1_dual":      return await windowLamp1Status();
        case "wl1_dual_set":
        case "wl1_led2set":   return await callPico(IPs.picolamp1, "3", arg2);     
        case "wl1_dualoff":
        case "wl1_led1off":   return await callPico(IPs.picolamp1, "1", "2");
        case "wl1_dualon":
        case "wl1_led1on":    return await callPico(IPs.picolamp1, "1", "3");
        case "wl1_led1":      return await callPico(IPs.picolamp1, "1", "0");
        case "wl1_led2off":   return await callPico(IPs.picolamp1, "2", "0");
        case "wl1_led2on":    return await callPico(IPs.picolamp1, "2", "1");
        case "wl1_led2":      return await callPico(IPs.picolamp1, "2", "2");
        case "wkststate":     return await localPing(IPs.desk);
        case "wkststateoff":
        case "wkststateon":   return await callApi(IPs.lrxmit, "pc_power");
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

async function executeMain(ramDisk, firstArg, secondArg) {
    switch (firstArg) {
        case "lightson":
            Promise.all([
                lrXmit("wl1_led1on"), 
                lrXmit("wl1_led2on"), 
                brXmit("brlamp1on")
            ]).catch(() => {});
            return "OK";
        case "lightsoff":
            Promise.all([
                lrXmit("wl1_led1off"), 
                lrXmit("wl1_led2off"), 
                brXmit("brlamp1off")
            ]).catch(() => {});
            OPCLEDparser(ramDisk, "stop");
            return "OK";
        case "lrxmit": 
            return await lrXmit(secondArg);
        case "lrstatus": {
            const lrRoom = new RoomManager();
            const [wlRaw, wkst, hifi] = await Promise.all([
                windowLamp1Status(),
                lrXmit("wkststate"),
                lrXmit("hifistate")
            ]);
            const [wl1, wl2] = splitByDelimiter(wlRaw, '~');
            lrRoom.addDevice('Window Lamp', wl1);
            lrRoom.addDevice('Backlight', wl2);
            lrRoom.addDevice('Desktop PC', wkst);
            lrRoom.addDevice('Stereo System', hifi);
            lrRoom.updateRoomState();
            return lrRoom.getData();
        }
        case "wl1_dual":
            lrXmit(firstArg + '_set', secondArg);
            return "OK";
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
            OPCLEDparser(ramDisk, "abstract");
            return "OK";
        case "lroff":
            Promise.all([
                lrXmit("wl1_led1off"),
                lrXmit("wl1_led2off"), 
                lrXmit("wkststateoff"), 
                lrXmit("hifistateoff")
            ]).catch(() => {});
            OPCLEDparser(ramDisk, "stop");
            return "OK";
        case "brxmit":
            return await brXmit(secondArg);
        case "brstatus": {
            const brRoom = new RoomManager();
            const [lamp, retro, macs, tv, pc, amp] = await Promise.all([
                brXmit("brlamp1"),
                brXmit("retropi"),
                brXmit("brmacs"),
                brXmit("brtv"),
                brXmit("brpc"),
                callApi(IPs.brpi, "ampstate")
            ]);
            brRoom.addDevice('Desk Lamp', lamp);
            brRoom.addDevice('RetroPi CRT', retro);
            brRoom.addDevice('Vintage Macs', macs);
            brRoom.addDevice('Bedroom TV', tv);
            brRoom.addDevice('Bedroom PC', pc);
            brRoom.addDevice('Audio System', amp);
            brRoom.updateRoomState();
            return brRoom.getData();
        }
        case "bron":
            Promise.all([
                brXmit("brlamp1on"), 
                brXmit("retropion"),
                brXmit("brmacson"), 
                brXmit("brtvon"), 
                brXmit("brpcon"), 
                callApi(IPs.brpi, "ampstateon")
            ]).catch(() => {});
            return "OK";
        case "broff":
            Promise.all([
                brXmit("brlamp1off"), 
                brXmit("retropioff"), 
                brXmit("brmacsoff"), 
                brXmit("brtvoff"), 
                callApi(IPs.brpc, "sleep"), 
                callApi(IPs.brpi, "ampstateoff")
            ]).catch(() => {});
            return "OK";
        case "brpi":
            return await callApi(IPs.brpi, secondArg);
        case "relax":
            Promise.all([
                callApi(IPs.brpi, "relax", secondArg), 
                brXmit("brtvoff"), 
                callApi(IPs.brpc, "sleep")
            ]).catch(() => {});
            return "OK";
        case "stop-br":
            return await callApi(IPs.brpi, "stoprelax");
        case "localping":
            return await localPing(`${secondArg}.${LOCAL_DOMAIN}`);
        case "status": {
            try { return await runStatusUpdate(ramDisk); } 
            catch (exc) { return "status update exception: " + exc.message; }
        }
        case "sitelookup":
            return await runSiteLookup(secondArg);
        case "server": {
            const logFile = path.join(ramDisk, 'sysout.txt');
            if (!secondArg) return "empty server command!";
            if (secondArg === "netscan") {
                await fsPromises.appendFile(logFile, `\nScanning network range ${NET_RANGE}...\n`);
                const scanResult = await runScanNetwork(NET_RANGE);
                await fsPromises.appendFile(logFile, `${scanResult}\n`);
                return "OK";
            }
            await fsPromises.appendFile(logFile, `\n${secondArg} sent.\n`);
            await fsPromises.writeFile(path.join(ramDisk, `${secondArg}.txt`), ''); 
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