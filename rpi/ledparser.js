const { spawn, execFile, exec } = require('child_process');
const fs = require('fs');
const fsPromises = require('fs/promises');
const util = require('util');

/**
 * CONSTANTS
 */
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

/**
 * PERSISTENT MEMORY STATE
 */
let inMemoryState = {
    processId: null,
    effectName: null
};

/**
 * Centralized error logging utility.
 */
function logInternalError(functionName, errorMessage, identifier) {
    const formattedMessage = `${functionName}: ${errorMessage}, ${identifier}`;
    console.error(formattedMessage);
}

/**
 * Updates the API's persistent memory with the current running effect.
 * @param {number|null} processId - The PID of the child process.
 * @param {string|null} effectName - The name of the effect being run.
 */
function recordEffectState(processId, effectName) {
    inMemoryState.processId = processId;
    inMemoryState.effectName = effectName;
}

/**
 * Performs a broad system sweep to stop any process running in the effects directory.
 */
async function forceKillOrphanedProcesses() {
    try {
        const searchCommand = `pgrep -f ${EFFECTS_DIRECTORY_PATH}`;
        const searchResult = await execAsync(searchCommand, { encoding: 'utf-8' });
        const pidsOutput = searchResult.stdout;
        const pidsList = pidsOutput.split('\n');

        for (let i = 0; i < pidsList.length; i = i + 1) {
            const currentPidString = pidsList[i];
            const trimmedPid = currentPidString.trim();

            if (trimmedPid !== "") {
                const pidToKill = parseInt(trimmedPid, 10);
                const isValidNumber = Number.isNaN(pidToKill) === false;

                if (isValidNumber === true) {
                    try {
                        const checkNameCommand = `ps -p ${pidToKill} -o comm=`;
                        const nameResult = await execAsync(checkNameCommand, { encoding: 'utf-8' });
                        const processName = nameResult.stdout.trim();
                        
                        const protectedProcesses = ["mp4toleds", "opcserver", "fcproxy", "fcserver"];
                        
                        const isProtected = protectedProcesses.some((protectedName) => {
                            return processName.includes(protectedName);
                        });

                        if (isProtected === false) {
                            process.kill(pidToKill, TERM_SIGNAL);
                        }
                    } catch (nameError) {
                        // Process already exited
                    }
                }
            }
        }
    } catch (searchError) {
        // No processes found
    }
}

/**
 * Stops any active effect by checking persistent memory AND sweeping the system.
 */
async function stopActiveEffect() {
    // 1. Signal MP4 API to stop
    const stopApiUrl = `${MP4_LEDS_API_URL}/api?video=stop`;
    try {
        await fetch(stopApiUrl, { signal: AbortSignal.timeout(1000) });
    } catch (networkError) {}
    // 2. Terminate PID stored in persistent memory
    const activePid = inMemoryState.processId;
    if (activePid !== null) {
        try {
            process.kill(activePid, TERM_SIGNAL);
        } catch (pidKillError) {}
    }
    // 3. Clear Memory
    recordEffectState(null, null);
}

/**
 * Spawns a new LED effect process and manages its lifecycle in memory.
 */
async function executeEffectProcess(binaryPath, binaryArguments, effectName) {
    await stopActiveEffect();
    try {
        console.log(`executeEffectProcess: ${effectName}`);

        const processConfiguration = {
            stdio: 'ignore',
            detached: false,
            cwd: EFFECTS_DIRECTORY_PATH
        };

        const newProcess = spawn(binaryPath, binaryArguments, processConfiguration);
        const spawnedPid = newProcess.pid;

        if (spawnedPid !== undefined) {
            recordEffectState(spawnedPid, effectName);
        }

        newProcess.on('close', () => {
            // Only clear memory if the process that just closed is the one we are tracking
            if (inMemoryState.processId === spawnedPid) {
                recordEffectState(null, null);
            }
        });

        newProcess.on('error', (spawnError) => {
            logInternalError("executeEffectProcess", "failed to spawn binary", spawnError.message);
            recordEffectState(null, null);
        });
        
    } catch (spawnException) {
        logInternalError("executeEffectProcess", "exception during process creation", spawnException.message);
    }
}

/**
 * Reads the hardware LED configuration.
 */
async function fetchLedHardwareType() {
    try {
        const fileContent = await fsPromises.readFile(LED_TYPE_CONFIG_FILE, 'utf-8');
        return fileContent.trim();
    } catch (readError) {
        return DEFAULT_LED_HARDWARE_TYPE;
    }
}

/**
 * Initialization logic to ensure a clean start on API boot.
 */
let isSystemInitialized = false;
let systemStartupPromise = null;

async function initializeSystemOnStartup() {
    try {
        recordEffectState(null, null);
        await forceKillOrphanedProcesses();

        const hardwareType = await fetchLedHardwareType();
        const clearBinary = `${EFFECTS_DIRECTORY_PATH}/writecolor`;
        await executeEffectProcess(clearBinary, [hardwareType, "000000"], "startup_clear");

        isSystemInitialized = true;
    } catch (e) {
        logInternalError("initializeSystemOnStartup", "critical failure", e.message);
    }
}

/**
 * Main parser function.
 */
async function OPCLEDparser(ramDiskPath, effectCommand = "", secondaryArgument = "") {
    if (isSystemInitialized === false) {
        if (systemStartupPromise === null) {
            systemStartupPromise = initializeSystemOnStartup();
        }
        await systemStartupPromise;
    }

    if (effectCommand === "") {
        return "no effect command";
    }

    const currentLedHardware = await fetchLedHardwareType();

    // 1. State Controls
    if (effectCommand === "pause") {
        await stopActiveEffect();
        await forceKillOrphanedProcesses();
        console.log('OPCLEDparser: pausedEffect');
        return "paused";
    }
    if (effectCommand === "stop") {
        const writeColorBinary = `${EFFECTS_DIRECTORY_PATH}/writecolor`;
        await executeEffectProcess(writeColorBinary, [currentLedHardware, "000000"], "stop_clear");
        await forceKillOrphanedProcesses();
        return "stopped";
    }

    // 2. Brightness Logic
    if (effectCommand === "fc" || effectCommand === "fc_leds") {
        let proxyRequestUrl = (effectCommand === "fc") 
            ? `${LED_PROXY_URL}/set?b=${encodeURIComponent(secondaryArgument)}`
            : `${LED_PROXY_URL}/brightness`;
        try {
            const proxyResponse = await fetch(proxyRequestUrl, { signal: AbortSignal.timeout(1000) });
            return (effectCommand === "fc_leds") ? await proxyResponse.text() : "fc ok";
        } catch (proxyError) {
            return "fc error";
        }
    }

    // 3. Shuffle Selection
    if (effectCommand === "shuffle") {
        try {
            const shuffleContent = await fsPromises.readFile(`${EFFECTS_DIRECTORY_PATH}/shuffle.txt`, 'utf-8');
            const lines = shuffleContent.split('\n').filter(l => l.trim() !== "");
            const selected = lines[Math.floor(Math.random() * lines.length)].trim();
            return await OPCLEDparser(ramDiskPath, selected, secondaryArgument);
        } catch (e) {
            return "shuffle error";
        }
    }

    // 4. MP4 Video Stream Handling
    try {
        const videoFileName = `${effectCommand}.mp4`;
        await fsPromises.access(`${EFFECTS_DIRECTORY_PATH}/videos/${videoFileName}`);

        await stopActiveEffect();

        const apiResponse = await fetch(`${MP4_LEDS_API_URL}/api?video=${videoFileName}`, { signal: AbortSignal.timeout(1000) });
        const apiText = await apiResponse.text();
        console.log(`OPCLEDparser: ${apiText}`);
        
        return apiText;
    } catch (e) {
        // File not found, proceed to binary check
    }

    // 5. C++ Executable Effects
    const registeredCppEffects = ["orbits", "noise", "chase", "miami", "stripes", "spectro", "oscope", "textscroll"];
    if (registeredCppEffects.includes(effectCommand)) {
        let binaryArgs = [currentLedHardware];

        // Spectrograph
        const AGC_FLOOR = "0.015";
        const VERT_ZOOM = "3";
        const OCTAVE_BANDS = "8";
        const MIN_FREQ = "1500";
        const MAX_FREQ = "4500";
        if (effectCommand === "spectro") binaryArgs.push(VERT_ZOOM, OCTAVE_BANDS, MIN_FREQ, MAX_FREQ, AGC_FLOOR);

        // Oscilloscope
        const AMP_DIV = "2";
        const FREQ_DIV = "3";
        const LINE_SMOOTH = "0.6";
        const NOISE_GATE = "10";
        if (effectCommand === "oscope") binaryArgs.push(AMP_DIV, FREQ_DIV, LINE_SMOOTH, NOISE_GATE);

        // Scrolling Text        
        if (effectCommand === "textscroll") {
          const tsWebSafeBase64 = secondaryArgument;
          if (tsWebSafeBase64 == "" || tsWebSafeBase64 == null) {
            return "TextScroll: argument cannot be empty";
          }
          const tsWebUnsafeBase64 = tsWebSafeBase64.replace(/-/g, "+").replace(/_/g, "/").replace(/@/g, "=");
          const tsDecodedCommands = Buffer.from(tsWebUnsafeBase64, 'base64').toString('utf8');
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

    // 6. Manual Hex Color
    if (effectCommand === "hex") {
        await executeEffectProcess(`${EFFECTS_DIRECTORY_PATH}/writecolor`, [currentLedHardware, secondaryArgument], "hex_color");
        return `Set Color To Hex: ${secondaryArgument}`;
    }

    // 7. Named Color Lookup (Colorscan)
    try {
        const scanResult = await execFileAsync(`${EFFECTS_DIRECTORY_PATH}/colorscan`, [effectCommand], { cwd: EFFECTS_DIRECTORY_PATH });
        const hex = scanResult.stdout.trim();
        if (!/^[0-9A-Fa-f]{6}$/.test(hex)) throw new Error("Invalid Hex");

        await executeEffectProcess(`${EFFECTS_DIRECTORY_PATH}/writecolor`, [currentLedHardware, hex], `color_${effectCommand}`);
        return `Set Color To: ${effectCommand}`;
    } catch (e) {
        logInternalError("OPCLEDparser", "color failed", e.message);
        return "error";
    }
}

module.exports = {
    OPCLEDparser
};