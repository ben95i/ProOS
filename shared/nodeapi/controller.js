const { execFile, exec } = require('child_process');
const fsPromises = require('fs/promises');
const util = require('util');
const { OPCLEDparser } = require('./ledparser');
const { runStatusUpdate } = require('./statusupd');
const { runScanNetwork } = require('./netscan');
const { runSiteLookup } = require('./sitelookup');

const execFileAsync = util.promisify(execFile);
const execAsync = util.promisify(exec);

const API_CALL_TIMEOUT = 1000; // in (ms)
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

function containsNonZero(arr) {
  for (let idx = 0; idx < arr.length; idx++) {
    if (arr[idx] != '0' && arr[idx] != 0) {
      return true;
    }
  }
  return false;
}

async function localPing(ip) {
    try {
        await execFileAsync('/usr/bin/ping', ['-4', '-A', '-c', '1', '-i', MAX_PING_WAIT, '-W', MAX_PING_WAIT, ip]);
        return "1"; 
    } catch (error) {
        return "0";
    }
}

async function wakeBrpc() {
    const ping = await localPing(IPs.brpc);
    if (ping === "1") {
        return "bedroom PC already online";
    } else {
        
        return "OK";
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
        const resp = extractResponse(raw_resp.replace(/[\\"\n]/g, ''), '|');
        return resp;
    } catch (error) {
        return "callApi exception: " + error.message;
    }
}

function extractResponse(str, delimiter) {
    if (str === "") return "";
    let outstr = str;
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
    const executeTask = async () => {
        let out;
        if (host === "lrxmit") {
            out = await callApi(IPs.lrxmit, "extcom", cmd);
        } 
        else if (host === "local") {
            const { stdout } = await execFileAsync('/usr/bin/ztermcom', [cmd]);
            out = extractResponse(stdout, '|');
        } else return "host empty!";
        if (out_pos === "") return out;
        if (out && out.length === 3) {
            return out[parseInt(out_pos)] === "9" ? "1" : "0";
        } 
        else if (out && out.length === 1) {
            return out[0] === "1" ? "1" : "0";
        } else return "X";
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

        case "brpc": {
            if (await localPing(IPs.brpc) === "1") return "pc_awake";
            return "0";
        }
        case "brpcoff": {
            if (await localPing(IPs.brpc) === "1") {
                return await callApi(IPs.brpc, "sleep");
            } else {
                return `${IPs.brpc} is already offline`;
            }
        }
        case "brpcon": {
            if (await localPing(IPs.brpc) === "1") {
                return `${IPs.brpc} is already online`;
            } else {
                await execFileAsync('/usr/bin/wakeonlan', [BRPC_MAC]);
                return "OK";
            }
        }

        default:  return "brXmit: invalid command";
    }
}

async function lrXmit(cmdIn) {
    let cmd1 = cmdIn;
    let cmd2 = "";
    const sepIndex = cmdIn.indexOf("-");
    if (sepIndex !== -1) {
        cmd1 = cmdIn.substring(0, sepIndex);
        cmd2 = cmdIn.substring(sepIndex + 1);
    }
    switch (cmd1) {
        case "wl_leds":       return await callPico(IPs.picolamp1, "4", "0");
        case "wl_ledsoff":
        case "wl_led1off":    return await callPico(IPs.picolamp1, "1", "2");
        case "wl_ledson":
        case "wl_led1on":     return await callPico(IPs.picolamp1, "1", "3");
        case "wl_leds_set":   return await callPico(IPs.picolamp1, "3", cmd2);
        case "wl_led2off":    return await callPico(IPs.picolamp1, "2", "0");
        case "wl_led2on":     return await callPico(IPs.picolamp1, "2", "1");
        case "wl_led1":       return await callPico(IPs.picolamp1, "1", "0");
        case "wl_led2":       return await callPico(IPs.picolamp1, "2", "2");

        case "wkststate": {
            const resp = await localPing(IPs.desk);
            if (resp === "1") { return "pc_awake"; }
            if (resp === "0") { return "0"; }
            return "X";
        }
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

        default:  return "lrXmit: invalid command";
    }
}

async function executeMain(ramDisk, firstArgIn, secondArgIn) {
    let roomStateData = {
      roomState: false,
      devices: []
    };
    function isRoomActive() {
        return roomStateData.devices.some(device => device[1] !== '0' && device[1] !== 0);
    }
    const firstArg = firstArgIn.replace(/\n/g, "");
    const secondArg = secondArgIn.replace(/\n/g, "");
    switch (firstArg) {

        // all lights
        case "lightson":
            await lrXmit("wl_led1on");
            await lrXmit("wl_led2on");
            await brXmit("brlamp1on");
            return "OK";
        case "lightsoff":
            await lrXmit("wl_led1off");
            await lrXmit("wl_led2off");
            await brXmit("brlamp1off");
            await OPCLEDparser(ramDisk, "stop", "");
            return "OK";

        // living room
        case "lrxmit":
            return await lrXmit(secondArg);
        case "lrstatus":
            try {
                roomStateData.devices = [];
                roomStateData.roomState = false;
                roomStateData.devices.push(['Window Lamp',   await lrXmit("wl_led1")]);
                roomStateData.devices.push(['Backlight',     await lrXmit("wl_led2")]);
                roomStateData.devices.push(['Desktop PC',    await lrXmit("wkststate")]);
                roomStateData.devices.push(['Stereo System', await lrXmit("hifistate")]);
                if (isRoomActive()) roomStateData.roomState = true;
                return roomStateData;
            } catch (error) {
                return "lrstatus error: " + error.message;
            }
        case "lrlightson":
            await lrXmit("wl_led1on");
            return "OK";
        case "lrlightsoff":
            await lrXmit("wl_led1off");
            return "OK";
        case "lron":
            await lrXmit("wl_led1on");
            await lrXmit("wl_led2on");
            await OPCLEDparser(ramDisk, "abstract", "");
            await lrXmit("wkststateon");
            await lrXmit("hifistateon");
            return "OK";
        case "lroff":
            await lrXmit("wl_led1off");
            await lrXmit("wl_led2off");
            await OPCLEDparser(ramDisk, "stop", "");
            await lrXmit("wkststateoff");
            await lrXmit("hifistateoff");
            return "OK";

        // bedroom
        case "brxmit":
            return await brXmit(secondArg);
        case "brstatus":
            try {
                roomStateData.devices = [];
                roomStateData.roomState = false;
                roomStateData.devices.push(['Desk Lamp',    await brXmit("brlamp1")]);
                roomStateData.devices.push(['RetroPi CRT',  await brXmit("retropi")]);
                roomStateData.devices.push(['Vintage Macs', await brXmit("brmacs")]);
                roomStateData.devices.push(['Bedroom TV',   await brXmit("brtv")]);
                roomStateData.devices.push(['Bedroom PC',   await brXmit("brpc")]);
                roomStateData.devices.push(['Audio System', await callApi(IPs.brpi, "ampstate")]);
                if (isRoomActive()) roomStateData.roomState = true;
                return roomStateData;
            } catch (error) {
                return "brstatus error: " + error.message;
            }

        case "bron":
            await brXmit("brlamp1on");
            await brXmit("retropion");
            await brXmit("brmacson");
            await brXmit("brtvon");
            await callApi(IPs.brpi, "ampstateon");
            return "OK";
        case "broff":
            await brXmit("brlamp1off");
            await brXmit("retropioff");
            await brXmit("brmacsoff");
            await brXmit("brtvoff");
            await callApi(IPs.brpi, "ampstateoff");
            await callApi(IPs.brpc, "sleep");
            return "OK";
        case "brpi":
            return await callApi(IPs.brpi, secondArg);
        case "relax":
            await brXmit("brtvoff");
            await callApi(IPs.brpi, "relax", secondArg);
            await callApi(IPs.brpc, "sleep");
            return "OK";
        case "stop-br":
            return await callApi(IPs.brpi, "stoprelax");

        // server functions
        case "localping":
            return await localPing(`${secondArg}.${LOCAL_DOMAIN}`);
        case "status": {
            try {
                const status_list = await runStatusUpdate(ramDisk);
                return status_list; 
            } catch (exc) {
                return "status update exception: " + exc.message;
            }
        }
        case "sitelookup": {
            const site_title = await runSiteLookup(secondArg);
            return site_title;
        }
        case "server": {
            const logFile = `${ramDisk}/sysout.txt`;
            if (secondArg === "") {
                return "empty server command!";
            }
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
        default:  return "executeMain: invalid command";
    }
}

module.exports = { executeMain };
