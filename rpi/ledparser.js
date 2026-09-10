const { spawn, execFile, exec } = require('child_process');
const fsPromises = require('fs/promises');
const util = require('util');

const LOCALHOST_URL = "http://127.0.0.1";
const BINARY_ROOT_PATH = "/opt/rpi";
const LED_PROXY_URL = `${LOCALHOST_URL}:7892`;
const MP4_LEDS_API_URL = `${LOCALHOST_URL}:7860`;
const EFFECTS_DIRECTORY_PATH = `${BINARY_ROOT_PATH}/effects`;
const LED_TYPE_CONFIG_FILE = `${BINARY_ROOT_PATH}/ledtype.txt`;
const DEFAULT_LED_HARDWARE_TYPE = "8x8grid";
const TERM_SIGNAL = "SIGKILL";

const execFileAsync = util.promisify(execFile);
const execAsync = util.promisify(exec);

let inMemoryState = {
    processId: null,
    effectName: null
};

// State Mutex to prevent zombie processes during rapid requests
let isSwitchingEffect = false; 

function logInternalError(functionName, errorMessage, identifier) {
    console.error(`${functionName}: ${errorMessage}, ${identifier}`);
}

function recordEffectState(processId, effectName) {
    inMemoryState.processId = processId;
    inMemoryState.effectName = effectName;
}

async function forceKillOrphanedProcesses() {
    try {
        const searchResult = await execAsync(`pgrep -f ${EFFECTS_DIRECTORY_PATH}`, { encoding: 'utf-8' });
        const pidsList = searchResult.stdout.split('\n');

        for (let i = 0; i < pidsList.length; i++) {
            const trimmedPid = pidsList[i].trim();
            if (trimmedPid !== "") {
                const pidToKill = parseInt(trimmedPid, 10);
                if (!Number.isNaN(pidToKill)) {
                    try {
                        const nameResult = await execAsync(`ps -p ${pidToKill} -o comm=`, { encoding: 'utf-8' });
                        const processName = nameResult.stdout.trim();
                        const protectedProcesses = ["mp4toleds", "opcserver", "fcproxy", "fcserver"];
                        
                        if (!protectedProcesses.some(name => processName.includes(name))) {
                            process.kill(pidToKill, TERM_SIGNAL);
                        }
                    } catch (nameError) {} // Already exited
                }
            }
        }
    } catch (searchError) {}
}

async function stopActiveEffect() {
    try {
        await fetch(`${MP4_LEDS_API_URL}/api?video=stop`, { signal: AbortSignal.timeout(1000) });
    } catch (networkError) {}
    
    if (inMemoryState.processId !== null) {
        try { process.kill(inMemoryState.processId, TERM_SIGNAL); } 
        catch (pidKillError) {}
    }
    recordEffectState(null, null);
}

async function executeEffectProcess(binaryPath, binaryArguments, effectName) {
    // Spinlock: wait for any current transition to finish
    while (isSwitchingEffect) await new Promise(resolve => setTimeout(resolve, 50));
    isSwitchingEffect = true;

    try {
        await stopActiveEffect();
        console.log(`executeEffectProcess: ${effectName}`);

        const newProcess = spawn(binaryPath, binaryArguments, { stdio: 'ignore', detached: false, cwd: EFFECTS_DIRECTORY_PATH });
        
        if (newProcess.pid !== undefined) {
            recordEffectState(newProcess.pid, effectName);
        }

        newProcess.on('close', () => {
            if (inMemoryState.processId === newProcess.pid) recordEffectState(null, null);
        });

        newProcess.on('error', (spawnError) => {
            logInternalError("executeEffectProcess", "failed to spawn binary", spawnError.message);
            recordEffectState(null, null);
        });
    } catch (spawnException) {
        logInternalError("executeEffectProcess", "exception", spawnException.message);
    } finally {
        isSwitchingEffect = false;
    }
}

async function fetchLedHardwareType() {
    try {
        return (await fsPromises.readFile(LED_TYPE_CONFIG_FILE, 'utf-8')).trim();
    } catch (readError) {
        return DEFAULT_LED_HARDWARE_TYPE;
    }
}

let isSystemInitialized = false;
let systemStartupPromise = null;

async function initializeSystemOnStartup() {
    try {
        recordEffectState(null, null);
        await forceKillOrphanedProcesses();
        const hardwareType = await fetchLedHardwareType();
        await executeEffectProcess(`${EFFECTS_DIRECTORY_PATH}/writecolor`, [hardwareType, "000000"], "startup_clear");
        isSystemInitialized = true;
    } catch (e) {
        logInternalError("initializeSystemOnStartup", "critical failure", e.message);
    }
}

function parseInputData(input, marker = 'fc_set') {
  const idx = input.indexOf(marker);
  if (idx !== -1) {
    const var1 = input.slice(0, idx + marker.length);       // includes marker
    const var2 = input.slice(idx + marker.length + 1);      // skip marker + "_"
    return { var1, var2 };
  }
  return { var1: input, var2: '' };
}

async function OPCLEDparser(baseWWW, CommandIn = "", secondaryArgument = "") {
    const parsedOut = parseInputData(CommandIn);
    const effectCommand = parsedOut.var1;

    if (!isSystemInitialized) {
        if (!systemStartupPromise) systemStartupPromise = initializeSystemOnStartup();
        await systemStartupPromise;
    }

    if (effectCommand === "") return "no effect command";
    const currentLedHardware = await fetchLedHardwareType();

    if (effectCommand === "pause") {
        await stopActiveEffect();
        await forceKillOrphanedProcesses();
        return "paused";
    }
    if (effectCommand === "stop") {
        await executeEffectProcess(`${EFFECTS_DIRECTORY_PATH}/writecolor`, [currentLedHardware, "000000"], "stop_clear");
        await forceKillOrphanedProcesses();
        return "stopped";
    }

    if (effectCommand === "fc_set" || effectCommand === "fc_leds") {
        const brightnessLevel = parsedOut.var2;
        let proxyRequestUrl = (effectCommand === "fc_set") 
            ? `${LED_PROXY_URL}/set?b=${brightnessLevel}`
            : `${LED_PROXY_URL}/brightness`;
        try {
            const proxyResponse = await fetch(proxyRequestUrl, { signal: AbortSignal.timeout(1000) });
            return (effectCommand === "fc_leds") ? await proxyResponse.text() : "fc ok";
        } catch (proxyError) {
            return "fc error";
        }
    }

    if (effectCommand === "shuffle") {
        try {
            const shuffleContent = await fsPromises.readFile(`${EFFECTS_DIRECTORY_PATH}/shuffle.txt`, 'utf-8');
            const lines = shuffleContent.split('\n').filter(l => l.trim() !== "");
            const selected = lines[Math.floor(Math.random() * lines.length)].trim();
            return await OPCLEDparser(baseWWW, selected, secondaryArgument);
        } catch (e) {
            return "shuffle error";
        }
    }

    try {
        const videoFileName = `${effectCommand}.mp4`;
        await fsPromises.access(`${EFFECTS_DIRECTORY_PATH}/videos/${videoFileName}`);
        await stopActiveEffect();
        const apiResponse = await fetch(`${MP4_LEDS_API_URL}/api?video=${videoFileName}`, { signal: AbortSignal.timeout(1000) });
        return await apiResponse.text();
    } catch (e) {}

    const registeredCppEffects = ["orbits", "noise", "chase", "miami", "stripes", "spectro", "oscope", "textscroll", "maze"];
    if (registeredCppEffects.includes(effectCommand)) {
        let binaryArgs = [currentLedHardware];

        // Spectrograph
        const VERT_ZOOM   = "2"; 
        const OCTAVE_BANDS = "8";
        const MIN_FREQ    = "800";
        const MAX_FREQ    = "3500";
        const AGC_FLOOR   = "0.03";
        const SCROLL      = "1.0";
        const DB_RANGE    = "30";

        if (effectCommand === "spectro")
            binaryArgs.push(VERT_ZOOM, OCTAVE_BANDS, MIN_FREQ, MAX_FREQ, AGC_FLOOR, SCROLL, DB_RANGE);

        // Oscilloscope
        const AMP_DIV = "2";
        const FREQ_DIV = "3";
        const LINE_SMOOTH = "0.6";
        const NOISE_GATE = "10";
        if (effectCommand === "oscope") 
            binaryArgs.push(AMP_DIV, FREQ_DIV, LINE_SMOOTH, NOISE_GATE);

        // Scrolling Text        
        if (effectCommand === "textscroll") {
          const tsBase64 = secondaryArgument.replace(/-/g, "+").replace(/_/g, "/").replace(/@/g, "=");
          const tsDecodedCommands = Buffer.from(tsBase64, 'base64').toString('utf8');
          const tsCommandArray = tsDecodedCommands.split("|"); 
          const tsMessage = tsCommandArray[0];
          const tsColor = tsCommandArray[1];
          const tsFont = tsCommandArray[2];
          const tsSpeed = tsCommandArray[3]; 
          binaryArgs.push(tsMessage, tsColor, tsFont, tsSpeed);
        }

        await executeEffectProcess(`${EFFECTS_DIRECTORY_PATH}/${effectCommand}`, binaryArgs, `cpp_${effectCommand}`);
        return `Binary Effect: ${effectCommand}`;
    }

    if (effectCommand === "hex") {
        await executeEffectProcess(`${EFFECTS_DIRECTORY_PATH}/writecolor`, [currentLedHardware, secondaryArgument], "hex_color");
        return `Set Color to #: ${secondaryArgument}`;
    }

    try {
        return `invalid effect name!`;
        
    } catch (e) {
        logInternalError("OPCLEDparser", "color failed", e.message);
        return "error";
    }
}

module.exports = { OPCLEDparser };