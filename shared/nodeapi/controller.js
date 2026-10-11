/////////////////////////////////////////////////
////// Automate Home Automation Server API //////
///////////// by Ben Provenzano III /////////////
/////////////////////////////////////////////////
//
// executeMain(baseWWW, arg1, arg2) is the one entry point: it dispatches a
// website command to the room it belongs to, and answers either a string or,
// for a room status, { roomState, devices }.

const { execFile } = require('child_process');
const fsPromises = require('fs/promises');
const path = require('path');
const util = require('util');
const execFileAsync = util.promisify(execFile);

const PROCESS_TIMEOUT = 3000; // in (ms)
const MAX_PING_WAIT = "0.5";  // in (sec)
const NET_RANGE = "192.168.1.0/24";
const IP_ADRS = {
    lrxmit: "192.168.1.12",
    desk: "192.168.1.14",
    brpi: "192.168.1.15",
    brpc: "192.168.1.17",
    brpc_mac: "90:2e:16:46:86:43",
    lrmlamp1: "192.168.1.18",
    bthrlight1: "192.168.1.21"
};

// imports //
const { OPCLEDparser } = require('./ledparser');
const { runStatusUpdate } = require('./statusupd');
const { runSiteLookup } = require('./sitelookup');
const { runWeatherWidget } = require('./weather');
const { runScanNetwork } = require('./netscan');

//// helpers ////

function isNumeric(value) {
    return typeof value === 'string' && value.trim() !== '' &&
        Number.isInteger(Number(value));
}

// 'a-b-c' -> ['a', 'b-c']; only the first delimiter splits
function splitByDelimiter(str, delimiter = '-') {
    if (!str) return ['', ''];
    const parts = str.split(delimiter);
    const before = parts[0] || '';
    let after = '';
    if (parts.length > 1) {
        after = parts.slice(1).join(delimiter);
    }
    return [before, after];
}

// a level as the three digits the transmitter expects: 7 -> "007"
function padOctet(value) {
  return String(value).padStart(3, "0");
}

// "1" when the host answers a ping, "0" when it does not
async function localPing(ip) {
    try {
        await execFileAsync('/usr/bin/ping',
            ['-4', '-A', '-c', '1', '-i', MAX_PING_WAIT, '-W', MAX_PING_WAIT, ip], {
            timeout: PROCESS_TIMEOUT
        });
        return "1";
    } catch (error) {
        return "0";
    }
}

// everything a failed child process can tell us, on one line
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

// Collects the devices of one room and works out whether the room counts as
// on: it does when any device in it is.
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

// Unescapes a device's answer and, when a delimiter is given, returns what
// sits between its first and last occurrence.
function extractResponse(str, delim) {
    if (!str) return str;
    const outstr = str.replace(/\\n|[\\"]/g, m => m === '\\n' ? '\n' : '');
    if (delim === '') return outstr;
    const first = outstr.indexOf(delim);
    if (first === -1) return outstr;
    const last = outstr.lastIndexOf(delim);
    if (last > first) return outstr.substring(first + 1, last);
    return outstr.substring(first + 1);
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

// a Pico lamp controller, which answers on port 4000
async function callPico(target, arg1, arg2 = '') {
    if (arg1 === "") return "callPico: argument cannot be empty";
    const response = await commonAPIRequest(`http://${target}:4000/api?${arg1},,${arg2}~`);
    return response.replaceAll('\n', '');
}

// as callPico(), but keeps the lines of a multi-line answer (a log or status)
async function callPicoText(target, arg1, arg2 = '') {
    if (arg1 === "") return "callPicoText: argument cannot be empty";
    const response = await commonAPIRequest(`http://${target}:4000/api?${arg1},,${arg2}~`);
    return response.trimEnd();
}

// another Automate device, which answers on port 80
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

// A three character answer holds one digit per device, and "9" means on;
// pos says which of them was asked about. Anything else passes through.
function decodeXmitCmd(str, pos) {
    if (pos === '') return str;
    if (str && str.length === 1) return str;
    if (str && str.length === 3) {
        if (str[parseInt(pos)] === "9") {
            return "1";
        }
        return "0";
    }
    return `invalid response: ${str}`;
}

// Splits a command at a marker it keeps: parseInputData('bed_leds_set_120',
// '_set') -> { var1: 'bed_leds_set', var2: '120' }
function parseInputData(input, marker) {
  const idx = input.indexOf(marker);
  if (idx !== -1) {
    const var1 = input.slice(0, idx + marker.length);       // includes marker
    const var2 = input.slice(idx + marker.length + 1);      // skip marker + "_"
    return { var1, var2 };
  }
  return { var1: input, var2: '' };
}

//// room scenes ////

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

function bathroomOn() {
    Promise.all([
        bthrXmit("bthr-light1on")
    ]).catch(() => {});
}

function bathroomOff() {
    Promise.all([
        bthrXmit("bthr-light1off")
    ]).catch(() => {});
}

//// living room lamp #1 ////

async function lrmLamp1Status() {
    return await callPico(IP_ADRS.lrmlamp1, "4", "0");
}
async function lrmLamp1Off() {
    return await callPico(IP_ADRS.lrmlamp1, "1", "2");
}
async function lrmLamp1On() {
    return await callPico(IP_ADRS.lrmlamp1, "1", "3");
}

//// bedroom ////

// Bedroom transmitter commands: command -> [code, position]. The position is
// which digit of a three digit status answer belongs to this device, and is
// left out for the commands that do not read a status back.
const BR_COM_CODES = {
    brtv:            ["11033", "0"],
    brtvoff:         ["11032"],
    brtvon:          ["11031"],

    bed_adj_up:      ["11041"],
    bed_adj_dwn:     ["11042"],
    bed_adj_stop:    ["11043"],
    bed_adj_sts:     ["11044"],
    bed_leds:        ["11004"],
    bed_leds_sts:    ["11003"],
    bed_leds_rb:     ["11005"],
    bed_leds_cy:     ["11006"],
    bed_leds_sw:     ["11007"],
    bed_leds_off:    ["11001"],
    bed_leds_pause:  ["11002"],

    brlamp1:         ["10007", "0"],
    brlamp1on:       ["10002"],
    brlamp1off:      ["10001"],

    brmacs:          ["10007", "1"],
    brmacson:        ["10004"],
    brmacsoff:       ["10003"],

    retropi:         ["10007", "2"],
    retropion:       ["10006"],
    retropioff:      ["10005"],

    roomth:          ["01005"]
};

// brXmit('brtvon') -> sends the TV on code; brXmit('bed_leds_set', '120')
async function brXmit(arg1, arg2 = '') {
    const coded = BR_COM_CODES[arg1];
    if (coded) {
        return await brComCall(coded[0], coded[1] || '');
    }
    switch (arg1) {
        case "bed_leds_set":     return await brComCall(`16${padOctet(arg2)}`);
        case "bed_leds_hex_set": return await callApi(IP_ADRS.brpi, arg1, arg2);

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

//// living room ////

// Living room transmitter commands: command -> code. None of these reads a
// multi-device status answer, so none carries a position.
const LR_COM_CODES = {
    hifistate:     "15019",
    hifistateoff:  "15018",
    hifistateon:   "15017",
    hifitoggle:    "15016",
    aux:           "15002",
    phono:         "15003",
    usb:           "15001",
    coax:          "15004",
    opt:           "15005",
    curinput:      "15007",
    autodac:       "15006",
    vlimit:        "15015",
    togglehpf:     "15014",
    curhpfvollim:  "15020",
    voldisplay:    "15013",
    volmute:       "15012",
    voldwn:        "15010",
    volup:         "15011",
    voldwnc:       "15008",
    volupc:        "15009",
    submute:       "17001",
    subup:         "17002",
    subdwn:        "17003",
    subup_fast:    "17004",
    subdwn_fast:   "17005"
};

async function lrXmit(arg1, arg2 = '') {
    const code = LR_COM_CODES[arg1];
    if (code) {
        return await lrComCall(code);
    }
    switch (arg1) {
        case "wl1_dual":      return await lrmLamp1Status();
        case "wl1_dual_set":  return await callPico(IP_ADRS.lrmlamp1, "3", arg2);
        case "wl1_dualoff":   return await lrmLamp1Off();
        case "wl1_dualon":    return await lrmLamp1On();
        case "wl1_led1off":   return await lrmLamp1Off();
        case "wl1_led1on":    return await lrmLamp1On();
        case "wl1_led1":      return await callPico(IP_ADRS.lrmlamp1, "1", "0");
        case "wl1_led2off":   return await callPico(IP_ADRS.lrmlamp1, "2", "0");
        case "wl1_led2on":    return await callPico(IP_ADRS.lrmlamp1, "2", "1");
        case "wl1_led2":      return await callPico(IP_ADRS.lrmlamp1, "2", "2");

        case "wkststate":     return await localPing(IP_ADRS.desk);
        case "wkststateoff":
            if (await localPing(IP_ADRS.desk) === "1") return await callApi(IP_ADRS.lrxmit, "pc_power");
            return `${IP_ADRS.desk} is already offline`;
        case "wkststateon":
            if (await localPing(IP_ADRS.desk) === "1") return `${IP_ADRS.desk} is already online`;
            return await callApi(IP_ADRS.lrxmit, "pc_power");

        default: return "lrXmit: invalid command";
    }
}

//// bathroom ////

// Bathroom light (Pico W) commands: command -> [Pico command, Pico value,
// reader]. A null value is taken from the website (the "_set" commands), and
// the log and status answer several lines, so callPicoText() reads them.
//
// "bthr-light1" is the brightness, 1-255: the level the light is at, or is
// fading to, while it is on, and the level it will come on at while it is
// off. Whether it is on is "bthr-light1_state", 1 or 0, which reports where a
// fade is heading from the moment it starts.
const BTHR_PICO_CODES = {
    "bthr-light1":               ["2",  "2",  callPico],
    "bthr-light1on":             ["2",  "1",  callPico],
    "bthr-light1off":            ["2",  "0",  callPico],
    "bthr-light1_state":         ["2",  "4",  callPico],
    "bthr-light1_set":           ["3",  null, callPico],
    "bthr-light1_motion":        ["2",  "3",  callPico],
    "bthr-light1_config":        ["8",  "0",  callPico],
    "bthr-light1_rangemin":      ["8",  "4",  callPico],
    "bthr-light1_rangemin_set":  ["4",  null, callPico],
    "bthr-light1_rangemax":      ["8",  "5",  callPico],
    "bthr-light1_rangemax_set":  ["5",  null, callPico],
    "bthr-light1_presence":      ["8",  "6",  callPico],
    "bthr-light1_presence_set":  ["6",  null, callPico],
    "bthr-light1_timeout":       ["8",  "7",  callPico],
    "bthr-light1_timeout_set":   ["7",  null, callPico],
    "bthr-light1_mode":          ["8",  "13", callPico],
    "bthr-light1_mode_set":      ["13", null, callPico],
    "bthr-light1_reset":         ["12", "1",  callPico],
    "bthr-light1_log":           ["9",  "0",  callPicoText],
    "bthr-light1_log_set":       ["9",  null, callPicoText],
    "bthr-light1_status":        ["10", "0",  callPicoText],
    "bthr-light1_sensor":        ["10", "1",  callPicoText]
};

// bthrXmit('bthr-light1on') -> fades the light on; bthrXmit('bthr-light1_set', '128')
//
// A "_set" command takes its value from the website. It is sent as the Pico
// reads it, a whole number of up to 4 digits ("0128" -> "128"), and refused
// here when it is not one, so an empty value never reaches the Pico as 0.
// Ranges within that are checked by the Pico itself.
async function bthrXmit(arg1, arg2 = '') {
    const codes = BTHR_PICO_CODES[arg1];
    if (!codes) return "bthrXmit: invalid command";
    const [command, fixedValue, reader] = codes;
    let value = fixedValue;
    if (value === null) {
        if (!isNumeric(arg2)) return "bthrXmit: non-numeric value";
        const number = Number(arg2);
        if (number < 0 || number > 9999) return "bthrXmit: out-of-range value";
        value = String(number);
    }
    return await reader(IP_ADRS.bthrlight1, command, value);
}

//// room status ////

// Reads every living room device at once and answers { roomState, devices }.
async function livingRoomStatus() {
    const lrRoom = new RoomManager();
    const [lamp, desktop, stereo] = await Promise.all([
        lrmLamp1Status(),
        lrXmit("wkststate"),
        lrXmit("hifistate")
    ]);
    // the lamp answers both of its channels as "lamp~backlight"
    const [lampMain, lampBacklight] = splitByDelimiter(lamp, '~');
    lrRoom.addDevice('Main Lamp', lampMain);
    lrRoom.addDevice('Backlight', lampBacklight);
    lrRoom.addDevice('Desktop PC', desktop);
    lrRoom.addDevice('Stereo System', stereo);
    lrRoom.updateRoomState();
    return lrRoom.getData();
}

async function bedroomStatus() {
    const brRoom = new RoomManager();
    const readings = await Promise.all([
        brXmit("brlamp1"),
        brXmit("bed_leds_sts"),
        brXmit("retropi"),
        brXmit("brmacs"),
        brXmit("brtv"),
        brXmit("brpc"),
        callApi(IP_ADRS.brpi, "ampstate")
    ]);
    const names = [
        'Desk Lamp', 
        'Bed Lights', 
        'RetroPi TV', 
        'Vintage Macs',
        'Bedroom TV', 
        'Bedroom PC', 
        'Audio System'
    ];
    names.forEach((name, idx) => brRoom.addDevice(name, readings[idx]));
    brRoom.updateRoomState();
    return brRoom.getData();
}

// The bathroom is on while its light is on, whether motion or the website
// switched it on. Read from the light's on / off state rather than its
// brightness, which stays above 0 while the light is off (it holds the level
// the light will come on at).
async function bathroomStatus() {
    const bthrRoom = new RoomManager();
    const light = await bthrXmit("bthr-light1_state");
    bthrRoom.addDevice('Bathroom Light', light);
    bthrRoom.updateRoomState();
    return bthrRoom.getData();
}

//// server actions ////

// Writes the request into the log and drops a trigger file for the shell
// side to pick up. The network scan answers into the log directly instead.
async function runServerCommand(baseWWW, secondArg) {
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

//// dispatch ////

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
        case "lrxmit": {
            const lrcmds = parseInputData(secondArg, '_set');
            return await lrXmit(lrcmds.var1, lrcmds.var2);
        }
        case "lrstatus":
            return await livingRoomStatus();
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
        case "brxmit": {
            const brcmds = parseInputData(secondArg, '_set');
            return await brXmit(brcmds.var1, brcmds.var2);
        }
        case "brstatus":
            return await bedroomStatus();
        case "bron":
            bedroomOn();
            return "OK";
        case "broff":
            bedroomOff();
            return "OK";
        case "bthrxmit": {
            const bthrcmds = parseInputData(secondArg, '_set');
            return await bthrXmit(bthrcmds.var1, bthrcmds.var2);
        }
        case "bthrstatus":
            return await bathroomStatus();
        case "bthron":
            bathroomOn();
            return "OK";
        case "bthroff":
            bathroomOff();
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
            return await runServerCommand(baseWWW, secondArg);
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
