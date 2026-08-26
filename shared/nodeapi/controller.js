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
const NET_RANGE = "192.168.1.0/24";
const IP_ADRS = {
    lrxmit: "192.168.1.12",
    desk: "192.168.1.14",
    brpi: "192.168.1.15",
    brpc: "192.168.1.17",
    brpc_mac: "90:2e:16:46:86:43",
    picolamp1: "192.168.1.18"
};

// imports //
const { OPCLEDparser } = require('./ledparser');
const { runStatusUpdate } = require('./statusupd');
const { runSiteLookup } = require('./sitelookup');
const { runWeatherWidget } = require('./weather');
const { runScanNetwork } = require('./netscan');

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

function padOctet(value) {
  return String(value).padStart(3, "0");
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

function extractResponse(str, delim) {
    if (!str) return str;
    const outstr = str.replace(/\\n|[\\"]/g, m => m === '\\n' ? '\n' : '');
    if (delim === '') return outstr;
    const first = outstr.indexOf(delim);
    if (first === -1) return outstr;
    const last = outstr.lastIndexOf(delim);
    return last > first
        ? outstr.substring(first + 1, last) 
        : outstr.substring(first + 1);
}

async function commonAPIRequest(server, delim = '') {
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

async function callPico(target, arg1, arg2 = '') {
    if (arg1 === "") return "callPico: argument cannot be empty";
    const response = await commonAPIRequest(`http://${target}:4000/api?${arg1},,${arg2}~`);
    return response.replaceAll('\n', '');
}

async function callApi(target, arg1, arg2 = '', delim = '|') {
    if (arg1 === "") return "callApi: argument cannot be empty";
    return await commonAPIRequest(`http://${target}:80/api?var=${arg2}&arg=${arg1}&action=main`, delim);
}

async function brComCall(cmd, pos = '') {
    return decodeXmitCmd(await callApi(IP_ADRS.brpi, "hwcom", cmd), pos);
}

async function lrComCall(cmd, pos = '') {
    return decodeXmitCmd(await callApi(IP_ADRS.lrxmit, "extcom", cmd), pos);
}

function decodeXmitCmd(str, pos){
    if (pos === '') return str;
    if (str && str.length === 1) return str;
    if (str && str.length === 3) return str[parseInt(pos)] === "9" ? "1" : "0";
    return `invalid response: ${str}`;
}

function parseInputData(input, marker) {
  const idx = input.indexOf(marker);
  if (idx !== -1) {
    const var1 = input.slice(0, idx + marker.length);       // includes marker
    const var2 = input.slice(idx + marker.length + 1);      // skip marker + "_"
    return { var1, var2 };
  }
  return { var1: input, var2: '' };
}

function bedroomOn() {
    Promise.all([
        brXmit("bed_leds_rb"),
        brXmit("brlamp1on"), 
        brXmit("retropion"),
        brXmit("brmacson"), 
        brXmit("brtvon"), 
        brXmit("brpcon"), 
        callApi(IP_ADRS.brpi, "ampstateon")
    ]).catch(() => {});
}

function bedroomOff() {
    Promise.all([
        brXmit("bed_leds_off"),
        brXmit("brlamp1off"), 
        brXmit("retropioff"), 
        brXmit("brmacsoff"), 
        brXmit("brtvoff"), 
        callApi(IP_ADRS.brpc, "sleep"), 
        callApi(IP_ADRS.brpi, "ampstateoff")
    ]).catch(() => {});
}

function livingRoomOn(baseWWW) {
    Promise.all([
        lrXmit("wl1_led1on"), 
        lrXmit("wl1_led2on"), 
        lrXmit("wkststateon"), 
        lrXmit("hifistateon"),
        OPCLEDparser(baseWWW, "abstract")
    ]).catch(() => {});
}

function livingRoomOff(baseWWW) {
    Promise.all([
        lrXmit("wl1_led1off"),
        lrXmit("wl1_led2off"), 
        lrXmit("wkststateoff"), 
        lrXmit("hifistateoff"),
        OPCLEDparser(baseWWW, "stop")
    ]).catch(() => {});
}

async function windowLamp1Status() { 
    return await callPico(IP_ADRS.picolamp1, "4", "0"); 
}
async function windowLamp1Off() { 
    return await callPico(IP_ADRS.picolamp1, "1", "2");
}
async function windowLamp1On() { 
    return await callPico(IP_ADRS.picolamp1, "1", "3");
}

async function brXmit(arg1, arg2 = '') {
    switch (arg1) {
        case "brtv":             return await brComCall("11033", "0");
        case "brtvoff":          return await brComCall("11032");
        case "brtvon":           return await brComCall("11031");

        case "bed_adj_up":       return await brComCall("11041");
        case "bed_adj_dwn":      return await brComCall("11042");
        case "bed_adj_stop":     return await brComCall("11043");
        case "bed_adj_sts":      return await brComCall("11044");
        case "bed_leds":         return await brComCall("11004");
        case "bed_leds_sts":     return await brComCall("11003");
        case "bed_leds_rb":      return await brComCall("11005");
        case "bed_leds_cy":      return await brComCall("11006");
        case "bed_leds_sw":      return await brComCall("11007");
        case "bed_leds_off":     return await brComCall("11001");
        case "bed_leds_pause":   return await brComCall("11002");
        case "bed_leds_set":     return await brComCall(`16${padOctet(arg2)}`);
        case "bed_leds_hex_set": return await callApi(IP_ADRS.brpi, arg1, arg2);
    
        case "brlamp1":          return await brComCall("10007", "0");
        case "brlamp1on":        return await brComCall("10002");
        case "brlamp1off":       return await brComCall("10001");

        case "brmacs":           return await brComCall("10007", "1");
        case "brmacson":         return await brComCall("10004");
        case "brmacsoff":        return await brComCall("10003");

        case "retropi":          return await brComCall("10007", "2");
        case "retropion":        return await brComCall("10006");
        case "retropioff":       return await brComCall("10005");

        case "roomth":           return await brComCall("01005");

        case "brpc":             return await localPing(IP_ADRS.brpc);
        case "brpcoff":  
            if (await localPing(IP_ADRS.brpc) === "1") {
              return await callApi(IP_ADRS.brpc, "sleep");
            }
            return `${IP_ADRS.brpc} is already offline`;
        case "brpcon": 
            if (await localPing(IP_ADRS.brpc) === "1") {
              return `${IP_ADRS.brpc} is already online`;
            }
            return await execFileAsync('/usr/bin/wakeonlan', [IP_ADRS.brpc_mac], { 
                timeout: PROCESS_TIMEOUT 
            }).then(() => "OK").catch(e => processProgramError(e));

        default: return "brXmit: invalid command";
    }
}

async function lrXmit(arg1, arg2 = '') {
    switch (arg1) {
        case "wl1_dual":      return await windowLamp1Status();
        case "wl1_dual_set":  return await callPico(IP_ADRS.picolamp1, "3", arg2);     
        case "wl1_dualoff":   return await windowLamp1Off();
        case "wl1_dualon":    return await windowLamp1On();
        case "wl1_led1off":   return await windowLamp1Off();
        case "wl1_led1on":    return await windowLamp1On();
        case "wl1_led1":      return await callPico(IP_ADRS.picolamp1, "1", "0");
        case "wl1_led2off":   return await callPico(IP_ADRS.picolamp1, "2", "0");
        case "wl1_led2on":    return await callPico(IP_ADRS.picolamp1, "2", "1");
        case "wl1_led2":      return await callPico(IP_ADRS.picolamp1, "2", "2");

        case "wkststate":     return await localPing(IP_ADRS.desk);
        case "wkststateoff": 
            if (await localPing(IP_ADRS.desk) === "1") {
              return await callApi(IP_ADRS.lrxmit, "pc_power");
            }
            return `${IP_ADRS.desk} is already offline`;
        case "wkststateon": 
            if (await localPing(IP_ADRS.desk) === "1") {
              return `${IP_ADRS.desk} is already online`;
            }
            return await callApi(IP_ADRS.lrxmit, "pc_power");

        case "hifistate":     return await lrComCall("15019");
        case "hifistateoff":  return await lrComCall("15018");
        case "hifistateon":   return await lrComCall("15017");
        case "hifitoggle":    return await lrComCall("15016");
        case "aux":           return await lrComCall("15002");
        case "phono":         return await lrComCall("15003");
        case "usb":           return await lrComCall("15001");
        case "coax":          return await lrComCall("15004");
        case "opt":           return await lrComCall("15005");
        case "curinput":      return await lrComCall("15007");
        case "autodac":       return await lrComCall("15006");
        case "vlimit":        return await lrComCall("15015");
        case "togglehpf":     return await lrComCall("15014");
        case "curhpfvollim":  return await lrComCall("15020");
        case "voldisplay":    return await lrComCall("15013");
        case "volmute":       return await lrComCall("15012");
        case "voldwn":        return await lrComCall("15010");
        case "volup":         return await lrComCall("15011");
        case "voldwnc":       return await lrComCall("15008");
        case "volupc":        return await lrComCall("15009");
        case "submute":       return await lrComCall("17001");
        case "subup":         return await lrComCall("17002");
        case "subdwn":        return await lrComCall("17003");
        case "subup_fast":    return await lrComCall("17004");
        case "subdwn_fast":   return await lrComCall("17005");
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
                OPCLEDparser(baseWWW, "stop")
            ]).catch(() => {});
            return "OK";
        case "allon":
            bedroomOn();
            livingRoomOn(baseWWW);
            return "OK";
        case "alloff":
            bedroomOff();
            livingRoomOff(baseWWW);
            return "OK";
        case "lrxmit": 
            const lrcmds = parseInputData(secondArg, '_set');
            return await lrXmit(lrcmds.var1, lrcmds.var2);
        case "lrstatus":
            const lrRoom = new RoomManager();
            const [l1, l2, l3] = await Promise.all([
                windowLamp1Status(),
                lrXmit("wkststate"),
                lrXmit("hifistate")
            ]);
            const [l1a, l1b] = 
                splitByDelimiter(l1, '~');
            lrRoom.addDevice('Window Lamp', l1a);
            lrRoom.addDevice('Lamp Backlight', l1b);
            lrRoom.addDevice('Desktop PC', l2);
            lrRoom.addDevice('Stereo System', l3);
            lrRoom.updateRoomState();
            return lrRoom.getData();
        case "lrlightson":
            lrXmit("wl1_led1on");
            return "OK";
        case "lrlightsoff":
            lrXmit("wl1_led1off");
            return "OK";
        case "lron":
            livingRoomOn(baseWWW);
            return "OK";
        case "lroff":
            livingRoomOff(baseWWW);
            return "OK";
        case "brxmit":
            const brcmds = parseInputData(secondArg, '_set');
            return await brXmit(brcmds.var1, brcmds.var2);
        case "brstatus":
            const brRoom = new RoomManager();
            const [b1, b2, b3, b4, b5, b6, b7] = await Promise.all([
                brXmit("brlamp1"),
                brXmit("bed_leds_sts"),
                brXmit("retropi"),
                brXmit("brmacs"),
                brXmit("brtv"),
                brXmit("brpc"),
                callApi(IP_ADRS.brpi, "ampstate")
            ]);
            brRoom.addDevice('Desk Lamp',    b1);
            brRoom.addDevice('Bed Lights',   b2);
            brRoom.addDevice('RetroPi TV',   b3);
            brRoom.addDevice('Vintage Macs', b4);
            brRoom.addDevice('Bedroom TV',   b5);
            brRoom.addDevice('Bedroom PC',   b6);
            brRoom.addDevice('Audio System', b7);
            brRoom.updateRoomState();
            return brRoom.getData();
        case "bron":
            bedroomOn();
            return "OK";
        case "broff":
            bedroomOff();
            return "OK";
        case "brpi":
            return await callApi(IP_ADRS.brpi, secondArg);
        case "relax":
            Promise.all([
                callApi(IP_ADRS.brpi, "relax", secondArg), 
                brXmit("brtvoff"), 
                callApi(IP_ADRS.brpc, "sleep")
            ]).catch(() => {});
            return "OK";
        case "relaxmenu":
        case "relaxstop":
            return await callApi(IP_ADRS.brpi, firstArg, '', '');
        case "status":
            try { return await runStatusUpdate(baseWWW); } 
            catch (exc) { return "status update exception: " + exc.message; }
        case "sitelookup":
            return await runSiteLookup(secondArg);
        case "server":
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
        case "weather":
            try {
                return await runWeatherWidget();
            } catch (err) {
                return `weatherWidget error: ${err.message}`;
            }
        default: return "executeMain: invalid command";
    }
}

module.exports = { executeMain };