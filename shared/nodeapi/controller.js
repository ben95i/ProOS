const { execFile, exec } = require('child_process');
const fsPromises = require('fs/promises');
const util = require('util');
const { OPCLEDparser } = require('./ledparser');
const { runStatusUpdate } = require('./statusupd');
const { runScanNetwork } = require('./netscan');
const { runSiteLookup } = require('./sitelookup');

const execFileAsync = util.promisify(execFile);
const execAsync = util.promisify(exec);

const API_CALL_TIMEOUT = 2500; // in (ms)
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
let serialQueue = Promise.resolve();

function isNumeric(value) {
  return typeof value === 'string' && value.trim() !== '' && Number.isInteger(Number(value));
}

function extractResponse(str, delimiter) {
    let outstr = str.replace(/[\\"\n]/g, '');
    if (outstr === '') return '';
    const firstPipe = outstr.indexOf(delimiter);
    if (firstPipe !== -1) {
        outstr = outstr.substring(firstPipe + 1);
    }
    const lastPipe = outstr.lastIndexOf(delimiter);
    if (lastPipe !== -1) {
        outstr = outstr.substring(0, lastPipe);
    }
    return outstr;
}

async function localPing(ip) {
    try {
        await execFileAsync('/usr/bin/ping', ['-4', '-A', '-c', '1', '-i', MAX_PING_WAIT, '-W', MAX_PING_WAIT, ip]);
        return "1"; 
    } catch (error) {
        return "0";
    }
}

class RoomManager {
    constructor() {
        this.roomState = null;
        this.devices = [];
    }
    addDevice(name, value) {
        let _status = null;
        if (value === 0 || value === '0') _status = false;
        else if (isNumeric(value) === true) _status = true; 
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

async function callApi(target, arg1, arg2 = "") {
    if (arg1 === "") {
        return "callApi: argument cannot be empty";
    }
    const server = `http://${target}:80/api?var=${arg2}&arg=${arg1}&action=main`;
    try {
        const response = await fetch(server, { signal: AbortSignal.timeout(API_CALL_TIMEOUT) });
        if (!response.ok) {
            return "callApi: invalid response";
        }
        const raw_resp = await response.text();
        const resp = extractResponse(raw_resp, '|');
        return resp;
    } catch (error) {
        return "callApi exception: " + error.message;
    }
}

async function callPico(target, arg1, arg2 = "") {
    if (arg1 === "") {
        return "callPico: argument cannot be empty";
    }
    const server = `http://${target}:4000/api?${arg1},,${arg2}~`;
    try {
        const response = await fetch(server, { signal: AbortSignal.timeout(API_CALL_TIMEOUT) });
        if (!response.ok) {
            return "callPico: invalid response";
        }
        const raw_resp = await response.text();
        return raw_resp.replace(/[\\"\n]/g, '');
    } catch (error) {
        return "callPico exception: " + error.message;
    }
}

async function callSerial(host, cmd, out_pos = "") {
    // FIFO execution
    const executeTask = async () => {
        let out;
        if (host === "lrxmit") {
            out = await callApi(IPs.lrxmit, "extcom", cmd);
        } 
        else if (host === "local") {
            try {
                const { stdout } = await execFileAsync('/usr/bin/ztermcom', [cmd]);
                out = extractResponse(stdout, '|');
            } catch (error) {
                out = error.message;
            }
        } else return null;
        if (out_pos === "") return out;
        if (out && out.length === 3) return out[parseInt(out_pos)] === "9" ? "1" : "0";
        else if (out && out.length === 1) return out[0] === "1" ? "1" : "0";
        else return "X";
    };
    const task = serialQueue.then(() => executeTask());
    serialQueue = task.catch(() => {});
    return task;
}

async function brXmit(cmd) {
    switch (cmd) {
        case "brlamp1":    return await callSerial("local", "10007", "0");
        case "brlamp1on":  return await callSerial("local", "10002");
        case "brlamp1off": return await callSerial("local", "10001");
        
        case "brmacs":     return await callSerial("local", "10007", "1");
        case "brmacson":   return await callSerial("local", "10004");
        case "brmacsoff":  return await callSerial("local", "10003");
        
        case "retropi":    return await callSerial("local", "10007", "2");
        case "retropion":  return await callSerial("local", "10006");
        case "retropioff": return await callSerial("local", "10005");
        
        case "roomth":     return await callSerial("local", "01005");
        
        case "brtv":       return await callSerial("local", "01003", "0");
        case "brtvoff":    return await callSerial("local", "01002");
        case "brtvon":     return await callSerial("local", "01001");

        case "brpc":       return await localPing(IPs.brpc);
        case "brpcoff": {
            if (await localPing(IPs.brpc) === "1") return await callApi(IPs.brpc, "sleep");
            return `${IPs.brpc} is already offline`; }
        case "brpcon": {
            if (await localPing(IPs.brpc) === "1") return `${IPs.brpc} is already online`;
            return execFileAsync('/usr/bin/wakeonlan', [BRPC_MAC]); }

        default:  return "brXmit: invalid command";
    }
}

async function lrXmit(arg1, arg2 = '') {
    switch (arg1) {
        case "wl1_dualoff":
        case "wl_led1off":    return await callPico(IPs.picolamp1, "1", "2");
        case "wl1_dualon":
        case "wl_led1on":     return await callPico(IPs.picolamp1, "1", "3");
        case "wl_led1":       return await callPico(IPs.picolamp1, "1", "0");
        case "wl_led2off":    return await callPico(IPs.picolamp1, "2", "0");
        case "wl_led2on":     return await callPico(IPs.picolamp1, "2", "1");
        case "wl_led2":       return await callPico(IPs.picolamp1, "2", "2");
        case "wl1_dual_set":  return await callPico(IPs.picolamp1, "3", arg2);     
        case "wl1_dual":      return await callPico(IPs.picolamp1, "4", "0");   

        case "wkststate":     return await localPing(IPs.desk);
        case "wkststateoff":
        case "wkststateon":   return await callApi(IPs.lrxmit, "pc_power");

        case "hifistate":     return await callSerial("lrxmit", "15019");
        case "hifistateoff":  return await callSerial("lrxmit", "15018");
        case "hifistateon":   return await callSerial("lrxmit", "15017");
        case "hifitoggle":    return await callSerial("lrxmit", "15016");
        case "aux":           return await callSerial("lrxmit", "15002");
        case "phono":         return await callSerial("lrxmit", "15003");
        case "usb":           return await callSerial("lrxmit", "15001");
        case "coax":          return await callSerial("lrxmit", "15004");
        case "opt":           return await callSerial("lrxmit", "15005");
        case "curinput":      return await callSerial("lrxmit", "15007");
        case "autodac":       return await callSerial("lrxmit", "15006");
        case "vlimit":        return await callSerial("lrxmit", "15015");
        case "togglehpf":     return await callSerial("lrxmit", "15014");
        case "curhpfvollim":  return await callSerial("lrxmit", "15020");
        case "voldisplay":    return await callSerial("lrxmit", "15013");
        case "volmute":       return await callSerial("lrxmit", "15012");
        case "voldwn":        return await callSerial("lrxmit", "15010");
        case "volup":         return await callSerial("lrxmit", "15011");
        case "voldwnc":       return await callSerial("lrxmit", "15008");
        case "volupc":        return await callSerial("lrxmit", "15009");
        case "submute":       return await callSerial("lrxmit", "17001");
        case "subup":         return await callSerial("lrxmit", "17002");
        case "subdwn":        return await callSerial("lrxmit", "17003");
        case "subup_fast":    return await callSerial("lrxmit", "17004");
        case "subdwn_fast":   return await callSerial("lrxmit", "17005");

        default: return "lrXmit: invalid command";
    }
}

async function executeMain(ramDisk, firstArg, secondArg) {
    switch (firstArg) {
        //// all lights ////
        case "lightson":
            lrXmit("wl_led1on");
            lrXmit("wl_led2on");
            brXmit("brlamp1on");
            return "OK";
        case "lightsoff":
            lrXmit("wl_led1off");
            lrXmit("wl_led2off");
            brXmit("brlamp1off");
            OPCLEDparser(ramDisk, "stop");
            return "OK";
        //// living room ////
        case "lrxmit": 
            return await lrXmit(secondArg);
        case "lrstatus": {
            const lrRoom = new RoomManager();
            lrRoom.addDevice('Window Lamp',   await lrXmit("wl_led1"));
            lrRoom.addDevice('Backlight',     await lrXmit("wl_led2"));
            lrRoom.addDevice('Desktop PC',    await lrXmit("wkststate"));
            lrRoom.addDevice('Stereo System', await lrXmit("hifistate"));
            lrRoom.updateRoomState();
            return lrRoom.getData();
        }
        case "wl1_dual":
            lrXmit(firstArg + '_set', secondArg);
            return "OK";
        case "lrlightson":
            lrXmit("wl_led1on");
            return "OK";
        case "lrlightsoff":
            lrXmit("wl_led1off");
            return "OK";
        case "lron":
            lrXmit("wl_led1on");
            lrXmit("wl_led2on");
            OPCLEDparser(ramDisk, "abstract");
            lrXmit("wkststateon");
            lrXmit("hifistateon");
            return "OK";
        case "lroff":
            lrXmit("wl_led1off");
            lrXmit("wl_led2off");
            OPCLEDparser(ramDisk, "stop");
            lrXmit("wkststateoff");
            lrXmit("hifistateoff");
            return "OK";
        //// bedroom ////
        case "brxmit":
            return await brXmit(secondArg);
        case "brstatus": {
            const brRoom = new RoomManager();
            brRoom.addDevice('Desk Lamp',    await brXmit("brlamp1"));
            brRoom.addDevice('RetroPi CRT',  await brXmit("retropi"));
            brRoom.addDevice('Vintage Macs', await brXmit("brmacs"));
            brRoom.addDevice('Bedroom TV',   await brXmit("brtv"));
            brRoom.addDevice('Bedroom PC',   await brXmit("brpc"));
            brRoom.addDevice('Audio System', await callApi(IPs.brpi, "ampstate"));
            brRoom.updateRoomState();
            return brRoom.getData();
        }
        case "bron":
            brXmit("brlamp1on");
            brXmit("retropion");
            brXmit("brmacson");
            brXmit("brtvon");
            brXmit("brpcon");
            await callApi(IPs.brpi, "ampstateon");
            return "OK";
        case "broff":
            brXmit("brlamp1off");
            brXmit("retropioff");
            brXmit("brmacsoff");
            brXmit("brtvoff");
            await callApi(IPs.brpc, "sleep");
            await callApi(IPs.brpi, "ampstateoff");
            return "OK";
        case "brpi":
            return await callApi(IPs.brpi, secondArg);
        case "relax":
            await callApi(IPs.brpi, "relax", secondArg);
            brXmit("brtvoff");
            await callApi(IPs.brpc, "sleep");
            return "OK";
        case "stop-br":
            return await callApi(IPs.brpi, "stoprelax");
        //// server functions ////
        case "localping":
            return await localPing(`${secondArg}.${LOCAL_DOMAIN}`);
        case "status": {
            try {
                return await runStatusUpdate(ramDisk); 
            } catch (exc) {
                return "status update exception: " + exc.message;
            }
        }
        case "sitelookup": {
            return await runSiteLookup(secondArg);
        }
        case "server": {
            const logFile = `${ramDisk}/sysout.txt`;
            if (secondArg === "") return "empty server command!";
            if (secondArg === "netscan") {
                await fsPromises.appendFile(logFile, `\nScanning network range ${NET_RANGE}...\n`);
                const scanResult = await runScanNetwork(NET_RANGE);
                await fsPromises.appendFile(logFile, `${scanResult}\n`);
                return "OK";
            }
            await fsPromises.appendFile(logFile, `\n${secondArg} sent.\n`);
            await execAsync(`touch ${ramDisk}/${secondArg}.txt`);
            return "OK";
        }
        default: return "executeMain: invalid command";
    }
}

module.exports = { executeMain };
