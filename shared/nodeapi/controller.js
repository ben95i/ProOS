const { execFile, exec } = require('child_process');
const fsPromises = require('fs/promises');
const util = require('util');
const { OPCLEDparser } = require('./ledparser');
const { runStatusUpdate } = require('./statusupd');

const execFileAsync = util.promisify(execFile);
const execAsync = util.promisify(exec);

const API_CALL_TIMEOUT = 5000; // in (ms)
const MAX_PING_WAIT = "0.5"; // in (sec)
const LOCAL_DOMAIN = "home";
const NET_RANGE = "192.168.1.0";
const BRPC_MAC = "90:2e:16:46:86:43";
const IPs = {
    lrxmit: "192.168.1.12",
    desk: "192.168.1.14",
    brpi: "192.168.1.15",
    brpc: "192.168.1.17",
    picolamp1: "192.168.1.18"
};

function extractResponse(str) {
    const delimiter = "|";
    if (str === "") {
        return "";
    }
    let tmpStr = str;
    const firstPipe = tmpStr.indexOf(delimiter);
    if (firstPipe !== -1) {
        tmpStr = tmpStr.substring(firstPipe + 1);
    }
    const lastPipe = tmpStr.lastIndexOf(delimiter);
    if (lastPipe !== -1) {
        tmpStr = tmpStr.substring(0, lastPipe);
    }
    return tmpStr;
}

async function localPing(ip) {
    try {
        await execFileAsync('/usr/bin/ping', ['-4', '-A', '-c', '1', '-i', MAX_PING_WAIT, '-W', MAX_PING_WAIT, ip]);
        return "1"; 
    } catch (error) {
        return "0";
    }
}

async function localCom(cmd) {
    try {
        const { stdout } = await execFileAsync('/usr/bin/ztermcom', [cmd]);
        return stdout;
    } catch (error) {
        return error.stderr || error.message;
    }
}

async function wakeBrpc() {
    const ping = await localPing(IPs.brpc);
    if (ping === "1") {
        return "bedroom PC already online.";
    } else {
        await execFileAsync('/usr/bin/wakeonlan', [BRPC_MAC]);
        return "";
    }
}

async function callApi(target, arg1, arg2 = "") {
    if (arg1 === "") {
        return "";
    }
    const server = `http://${target}:80/api?var=${arg2}&arg=${arg1}&action=main`;
    try {
        const response = await fetch(server, { signal: AbortSignal.timeout(API_CALL_TIMEOUT) });
        if (!response.ok) {
            return "invalid response";
        }
        const apiResp = await response.text();
        const respClean = apiResp.replace(/[\\"]/g, '');
        return(extractResponse(respClean));
    } catch (error) {
        return "callApi exception: " + error.message;
    }
}

async function callPico(target, arg1, arg2 = "") {
    if (arg1 === "") {
        return "argument cannot be empty";
    }
    const server = `http://${target}:4000/api?${arg1},,${arg2}~`;
    try {
        const response = await fetch(server, { signal: AbortSignal.timeout(API_CALL_TIMEOUT) });
        if (!response.ok) {
            return "invalid response";
        }
        const piResp = await response.text();
        return piResp.replace(/[\\"]/g, '')
    } catch (error) {
        return "callPico exception: " + error.message;
    }
}

async function readComResp(xmit, respCmd, respPos = "") {
    let ttyRaw = "";
    if (xmit === "lrxmit") {
        ttyRaw = await callApi(IPs.lrxmit, "extcom", respCmd);
    }
    if (xmit === "local") {
        ttyRaw = await localCom(respCmd);
    }
    const ttyOut = extractResponse(ttyRaw);
    if (respPos === "") {
        return ttyOut;
    }
    if (ttyOut.length === 3) {
        return ttyOut[parseInt(respPos)] === "9" ? "1" : "0";
    }
    if (ttyOut.length === 1) {
        return ttyOut[0] === "1" ? "1" : "0";
    }
    return "X";
}

async function brXmit(cmd) {
    switch (cmd) {
        case "brlamp1":    return await readComResp("local", "10007", "0");
        case "brlamp1on":  return await localCom("10002");
        case "brlamp1off": return await localCom("10001");
        
        case "brmacs":     return await readComResp("local", "10007", "1");
        case "brmacson":   return await localCom("10004");
        case "brmacsoff":  return await localCom("10003");
        
        case "retropi":    return await readComResp("local", "10007", "2");
        case "retropion":  return await localCom("10006");
        case "retropioff": return await localCom("10005");
        
        case "roomth":     return await readComResp("local", "01005", "");
        
        case "brtv":       return await readComResp("local", "01003", "0");
        case "brtvoff":    return await localCom("01002");
        case "brtvon":
            await localCom("01001");
            await wakeBrpc();
            return "OK";

        case "brpc": {
            const ping = await localPing(IPs.brpc);
            if (ping === "1") { return "pc_awake"; }
            return "0";
        }
        case "brpcoff": {
            const ping = await localPing(IPs.brpc);
            if (ping === "1") {
                return await callApi(IPs.brpc, "sleep", "");
            } else {
                return `${IPs.brpc} is already offline`;
            }
        }
        case "brpcon": {
            const ping = await localPing(IPs.brpc);
            if (ping === "1") {
                return `${IPs.brpc} is already online`;
            } else {
                await wakeBrpc();
                return "OK";
            }
        }

        default:
            return "invalid local command!";
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
        case "wl_led2":       return await callPico(IPs.picolamp1, "2", "2");

        case "wkststate": {
            const resp = await localPing(IPs.desk);
            if (resp === "1") { return "pc_awake"; }
            if (resp === "0") { return "0"; }
            return "X";
        }
        case "wkststateoff":
        case "wkststateon":   return await callApi(IPs.lrxmit, "pc_power", "");
        
        case "hifistate": {
            const resp = await readComResp("lrxmit", "15019", "");
            if (resp === "1") { return "1"; }
            if (resp === "0") { return "0"; }
            return "X";
        }
        case "hifistateoff":  return await callApi(IPs.lrxmit, "extcom", "15018");
        case "hifistateon":   return await callApi(IPs.lrxmit, "extcom", "15017");
        case "hifitoggle":    return await callApi(IPs.lrxmit, "extcom", "15016");

        case "aux":           return await callApi(IPs.lrxmit, "extcom", "15002");
        case "phono":         return await callApi(IPs.lrxmit, "extcom", "15003");
        case "usb":           return await callApi(IPs.lrxmit, "extcom", "15001");
        case "coax":          return await callApi(IPs.lrxmit, "extcom", "15004");
        case "opt":           return await callApi(IPs.lrxmit, "extcom", "15005");
        case "curinput":      return await callApi(IPs.lrxmit, "extcom", "15007");
        case "autodac":       return await callApi(IPs.lrxmit, "extcom", "15006");
        
        case "vlimit":        return await callApi(IPs.lrxmit, "extcom", "15015");
        case "togglehpf":     return await callApi(IPs.lrxmit, "extcom", "15014");
        case "curhpfvollim":  return await callApi(IPs.lrxmit, "extcom", "15020");
        
        case "voldisplay":    return await readComResp("lrxmit", "15013", "");
        case "volmute":       return await readComResp("lrxmit", "15012", "");
        case "voldwn":        return await readComResp("lrxmit", "15010", "");
        case "volup":         return await readComResp("lrxmit", "15011", "");
        case "voldwnc":       return await readComResp("lrxmit", "15008", "");
        case "volupc":        return await readComResp("lrxmit", "15009", "");
        
        case "submute":       return await callApi(IPs.lrxmit, "extcom", "17001");
        case "subup":         return await callApi(IPs.lrxmit, "extcom", "17002");
        case "subdwn":        return await callApi(IPs.lrxmit, "extcom", "17003");
        case "subup_fast":    return await callApi(IPs.lrxmit, "extcom", "17004");
        case "subdwn_fast":   return await callApi(IPs.lrxmit, "extcom", "17005");

        default:              return "invalid lrXmit command!";
    }
}

async function executeMain(ramDisk, firstArg, secondArg) {
    const cmd = firstArg.replace(/\n/g, "");
    const subCmd = secondArg.replace(/\n/g, "");
    switch (cmd) {
        case "bron":
            await brXmit("brlamp1on");
            await brXmit("retropion");
            await brXmit("brmacson");
            await callApi(IPs.brpi, "ampstateon", "");
            await brXmit("brtvon");
            return "";
        case "broff":
            await brXmit("brlamp1off");
            await brXmit("retropioff");
            await brXmit("brmacsoff");
            await callApi(IPs.brpi, "ampstateoff", "");
            await brXmit("brtvoff");
            await callApi(IPs.brpc, "sleep", "");
            return "";
        case "lightson":
            await lrXmit("wl_led1on");
            await lrXmit("wl_led2on");
            await brXmit("brlamp1on");
            return "";
        case "lightsoff":
            await lrXmit("wl_led1off");
            await lrXmit("wl_led2off");
            await brXmit("brlamp1off");
            await OPCLEDparser(ramDisk, "stop", "");
            return "";
        case "lrlightson":
            await lrXmit("wl_led1on");
            return "";
        case "lrlightsoff":
            await lrXmit("wl_led1off");
            return "";
        case "lron":
            await lrXmit("wl_led1on");
            await lrXmit("wl_led2on");
            await OPCLEDparser(ramDisk, "abstract", "");
            await lrXmit("wkststateon");
            await lrXmit("hifistateon");
            return "";
        case "lroff":
            await lrXmit("wl_led1off");
            await lrXmit("wl_led2off");
            await OPCLEDparser(ramDisk, "stop", "");
            await lrXmit("wkststateoff");
            await lrXmit("hifistateoff");
            return "";
        case "brxmit":
            return await brXmit(subCmd);
        case "lrxmit":
            return await lrXmit(subCmd);
        case "brpi":
            return await callApi(IPs.brpi, subCmd, "");
        case "relax":
            await callApi(IPs.brpi, "relax", subCmd);
            await callApi(IPs.brpc, "sleep", "");
            await brXmit("brtvoff");
            return "";
        case "stop-br":
            return await callApi(IPs.brpi, "stoprelax", "");
        case "localping":
            return await localPing(`${subCmd}.${LOCAL_DOMAIN}`);
        case "status": {
            try {
                const result = await runStatusUpdate(ramDisk);
                return result; 
            } catch (exc) {
                return "status update exception: " + exc.message;
            }
        }
        case "sitelookup": {
            try {
                let base64In = subCmd.replace(/-/g, "+").replace(/_/g, "/").replace(/@/g, "=");
                let decodedUrl = Buffer.from(base64In, 'base64').toString('utf8');
                let urlObj = new URL(decodedUrl);
                if (urlObj.hostname === "www.ups.com" || urlObj.hostname === "ups.com") {
                    return "UPS Tracking";
                }
                if (urlObj.hostname === "tools.usps.com") {
                    return "USPS Tracking";
                }
                const { stdout } = await execAsync(`curl -s -X GET "${decodedUrl}" | xmllint -html -xpath "//head/title/text()" - 2>/dev/null`);
                if (stdout.trim() === "") {
                    return "Site Lookup Error!";
                }
                return stdout.trim();
            } catch (e) {
                return "Site Lookup Error!";
            }
        }
        case "server": {
            const logFile = `${ramDisk}/sysout.txt`;
            if (subCmd === "") {
                return "server argument cannot be empty!";
            }
            if (subCmd === "netscan") {
                await execAsync(`nmap -v --open -sn ${NET_RANGE}/24 >> ${logFile}`);
                return "** network scan **";
            }
            await fsPromises.appendFile(logFile, `\n${subCmd} sent.\n`);
            await execAsync(`touch ${ramDisk}/${subCmd}.txt`);
            return " ";
        }
        default:
            return "invalid command !";
    }
}

module.exports = { executeMain };
