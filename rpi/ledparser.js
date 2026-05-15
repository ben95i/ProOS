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
const TERM_SIGNAL = "SIGTERM";

const execFileAsync = util.promisify(execFile);
const execAsync = util.promisify(exec);

/**
 * Helper to dynamically generate the state file path using the provided ram disk path.
 * @param {string} ramDiskPath - The dynamic path passed from the API.
 * @returns {string} The full absolute path to the JSON state file.
 */
function getStateFilePath(ramDiskPath) {
    const fullPath = `${ramDiskPath}/led_effect_state.json`;
    return fullPath;
}

/**
 * Centralized error logging utility.
 * Format: FUNCTION_NAME: error message in lower case, IDENTIFIER or err.message
 * @param {string} functionName - Name of the function where the error occurred.
 * @param {string} errorMessage - Description of the error in lower case.
 * @param {string} identifier - Error message or specific identifier.
 */
function logInternalError(functionName, errorMessage, identifier) {
    const formattedMessage = `${functionName}: ${errorMessage}, ${identifier}`;
    console.error(formattedMessage);
}

/**
 * Writes the current PID and effect name to a JSON file to track state.
 * Uses atomic writes (write temp -> rename) to prevent file corruption.
 * @param {string} ramDiskPath - Directory to store the state file.
 * @param {number|null} processId - The PID of the child process.
 * @param {string|null} effectName - The name of the effect being run.
 */
async function recordEffectState(ramDiskPath, processId, effectName) {
    const stateFilePath = getStateFilePath(ramDiskPath);
    const tempFilePath = `${stateFilePath}.tmp`;
    
    const stateData = {
        processId: processId,
        effectName: effectName
    };

    const serializedState = JSON.stringify(stateData);

    try {
        await fsPromises.writeFile(tempFilePath, serializedState, 'utf-8');
        await fsPromises.rename(tempFilePath, stateFilePath);
    } catch (fileWriteError) {
        logInternalError("recordEffectState", "failed to write atomic state file", fileWriteError.message);
    }
}

/**
 * Performs a broad system sweep to kill any process running from the effects directory.
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
                        
                        const protectedProcesses = ["mp4toleds", "opcserver", "fcproxy"];
                        
                        const isProtected = protectedProcesses.some((protectedName) => {
                            const matchFound = processName.includes(protectedName);
                            return matchFound;
                        });

                        if (isProtected === false) {
                            process.kill(pidToKill, TERM_SIGNAL);
                        }
                    } catch (nameError) {
                        // Process may have already exited organically
                    }
                }
            }
        }
    } catch (searchError) {
        // pgrep returns an error if no processes are found; this is expected behavior.
    }
}

/**
 * Stops any active effect by checking the dynamic state file AND sweeping the system.
 * @param {string} ramDiskPath - Directory storing the state file.
 */
async function stopActiveEffect(ramDiskPath) {
    const stateFilePath = getStateFilePath(ramDiskPath);

    // 1. Signal MP4 API to stop
    const stopApiUrl = `${MP4_LEDS_API_URL}/api?video=stop`;
    const requestTimeout = AbortSignal.timeout(1000);

    try {
        await fetch(stopApiUrl, { signal: requestTimeout });
    } catch (networkError) {
        // API might be down, continue process cleanup gracefully
    }

    // 2. Attempt to kill the specific PID from state file
    try {
        const stateFileExists = fs.existsSync(stateFilePath); 
        
        if (stateFileExists === true) {
            const fileBuffer = await fsPromises.readFile(stateFilePath, 'utf-8');
            const trimmedBuffer = fileBuffer.trim();

            if (trimmedBuffer !== "") {
                const parsedState = JSON.parse(trimmedBuffer);
                const activePid = parsedState.processId;

                if (activePid !== null) {
                    try {
                        process.kill(activePid, TERM_SIGNAL);
                    } catch (pidKillError) {
                        // PID might be stale or already dead
                    }
                }
            }
        }
    } catch (stateReadError) {
        logInternalError("stopActiveEffect", "could not process state file json", stateReadError.message);
    }

    // 3. Sweep for orphans asynchronously
    await forceKillOrphanedProcesses();

    // 4. Clear the tracking file dynamically
    await recordEffectState(ramDiskPath, null, null);
}

/**
 * Spawns a new LED effect process and manages its lifecycle dynamically.
 * @param {string} ramDiskPath - Directory storing the state file.
 * @param {string} binaryPath - Path to the executable.
 * @param {string[]} binaryArguments - Executable arguments.
 * @param {string} effectName - Name of the effect to record.
 */
async function executeEffectProcess(ramDiskPath, binaryPath, binaryArguments, effectName) {
    const stateFilePath = getStateFilePath(ramDiskPath);
    
    await stopActiveEffect(ramDiskPath);

    try {
        const processConfiguration = {
            stdio: 'ignore', 
            detached: false,
            cwd: EFFECTS_DIRECTORY_PATH
        };

        const newProcess = spawn(binaryPath, binaryArguments, processConfiguration);
        const spawnedPid = newProcess.pid;

        if (spawnedPid !== undefined) {
            await recordEffectState(ramDiskPath, spawnedPid, effectName);
        }

        newProcess.on('close', async (exitCode) => {
            try {
                const checkStateExists = fs.existsSync(stateFilePath);
                if (checkStateExists === true) {
                    const currentStateRaw = await fsPromises.readFile(stateFilePath, 'utf-8');
                    const currentState = JSON.parse(currentStateRaw);
                    const trackedPid = currentState.processId;

                    if (trackedPid === spawnedPid) {
                        await recordEffectState(ramDiskPath, null, null);
                    }
                }
            } catch (cleanupError) {
                // Ignore errors reading state during a close event
            }
        });

        newProcess.on('error', async (spawnError) => {
            logInternalError("executeEffectProcess", "failed to spawn binary", spawnError.message);
            await recordEffectState(ramDiskPath, null, null);
        });

    } catch (spawnException) {
        logInternalError("executeEffectProcess", "exception during process creation", spawnException.message);
    }
}

/**
 * Reads the hardware LED configuration from the filesystem.
 */
async function fetchLedHardwareType() {
    try {
        const fileContent = await fsPromises.readFile(LED_TYPE_CONFIG_FILE, 'utf-8');
        const hardwareType = fileContent.trim();
        return hardwareType;
    } catch (readError) {
        logInternalError("fetchLedHardwareType", "read failure for config file", readError.code);
        return DEFAULT_LED_HARDWARE_TYPE;
    }
}

/**
 * Startup Locks and initialization logic
 */
let isSystemInitialized = false;
let systemStartupPromise = null;

async function initializeSystemOnStartup(ramDiskPath) {
    try {
        await recordEffectState(ramDiskPath, null, null);
        await forceKillOrphanedProcesses();

        const hardwareType = await fetchLedHardwareType();
        const hexClearColor = "000000";
        const clearArgs = [hardwareType, hexClearColor];
        const clearBinary = `${EFFECTS_DIRECTORY_PATH}/writecolor`;
        
        await executeEffectProcess(ramDiskPath, clearBinary, clearArgs, "startup_hardware_clear");
        
        isSystemInitialized = true;
    } catch (initializationError) {
        logInternalError("initializeSystemOnStartup", "failed to initialize system", initializationError.message);
    }
}

/**
 * Main parser function to handle LED control commands dynamically.
 */
async function OPCLEDparser(ramDiskPath, effectCommand = "", secondaryArgument = "") {
    // 1. Lazy Initialization: Start the lock on the very first API hit
    if (isSystemInitialized === false) {
        const isPromiseNull = systemStartupPromise === null;
        if (isPromiseNull === true) {
            systemStartupPromise = initializeSystemOnStartup(ramDiskPath);
        }
        await systemStartupPromise;
    }

    if (effectCommand === "") {
        return "no effect command";
    }

    const currentLedHardware = await fetchLedHardwareType();

    // 2. Core State Controls
    if (effectCommand === "pause") {
        await stopActiveEffect(ramDiskPath);
        return "paused";
    }

    if (effectCommand === "stop") {
        await stopActiveEffect(ramDiskPath);
        const stopHex = "000000";
        const stopArguments = [currentLedHardware, stopHex];
        const writeColorBinary = `${EFFECTS_DIRECTORY_PATH}/writecolor`;
        await executeEffectProcess(ramDiskPath, writeColorBinary, stopArguments, "stop_clear");
        return "stopped and cleared";
    }

    // 3. Fadecandy Proxy Logic
    const isBrightnessSet = effectCommand === "fc";
    const isBrightnessQuery = effectCommand === "fc_leds";

    if (isBrightnessSet === true || isBrightnessQuery === true) {
        let proxyRequestUrl;
        if (isBrightnessSet === true) {
            const encodedBrightness = encodeURIComponent(secondaryArgument);
            proxyRequestUrl = `${LED_PROXY_URL}/set?b=${encodedBrightness}`;
        } else {
            proxyRequestUrl = `${LED_PROXY_URL}/brightness`;
        }

        try {
            const networkTimeout = AbortSignal.timeout(1000);
            const proxyResponse = await fetch(proxyRequestUrl, { signal: networkTimeout });
            
            if (isBrightnessQuery === true) {
                const brightnessText = await proxyResponse.text();
                return brightnessText;
            } else {
                return "fc ok";
            }
        } catch (proxyError) {
            logInternalError("OPCLEDparser", "proxy fetch failure", proxyError.message);
            return "fc error";
        }
    }

    // 4. Shuffle Selection Logic
    if (effectCommand === "shuffle") {
        try {
            const shuffleListPath = `${EFFECTS_DIRECTORY_PATH}/shuffle.txt`;
            const shuffleContent = await fsPromises.readFile(shuffleListPath, 'utf-8');
            const contentLines = shuffleContent.split('\n');
            
            const filteredLines = contentLines.filter((line) => {
                const trimmedLine = line.trim();
                const isLineValid = trimmedLine !== "";
                return isLineValid;
            });

            const randomLineIndex = Math.floor(Math.random() * filteredLines.length);
            const selectedEffect = filteredLines[randomLineIndex];
            const cleanEffectCommand = selectedEffect.trim();
            
            return await OPCLEDparser(ramDiskPath, cleanEffectCommand, secondaryArgument);
            
        } catch (shuffleFileError) {
            logInternalError("OPCLEDparser", "failed to read shuffle list", shuffleFileError.message);
            return "shuffle error";
        }
    }

    // 5. MP4 Video Stream Handling
    try {
        const videoFileName = `${effectCommand}.mp4`;
        const videoFileSystemPath = `${EFFECTS_DIRECTORY_PATH}/videos/${videoFileName}`;
        
        await fsPromises.access(videoFileSystemPath);
        
        await stopActiveEffect(ramDiskPath);
        
        const videoApiEndpoint = `${MP4_LEDS_API_URL}/api?video=${videoFileName}`;
        const videoAbortSignal = AbortSignal.timeout(1000);

        try {
            const apiResponse = await fetch(videoApiEndpoint, { signal: videoAbortSignal });
            const apiStatus = await apiResponse.text();
            return apiStatus;
        } catch (apiNetworkError) {
            logInternalError("OPCLEDparser", "mp4 api communication failure", apiNetworkError.message);
            return `network error for ${effectCommand}`;
        }
    } catch (videoMissingError) {
        // File does not exist, continue to check for binaries
    }

    // 6. C++ Executable Effects
    const registeredCppEffects = ["spectro", "orbits", "noise", "chase", "miami", "spark", "stripes", "lava"];
    const isRegisteredCppEffect = registeredCppEffects.includes(effectCommand);

    if (isRegisteredCppEffect === true) {
        let binaryArgs = [currentLedHardware];
        
        if (effectCommand === "spectro") {
            binaryArgs.push("40");
            binaryArgs.push("350");
            binaryArgs.push("4500");
        }

        const effectBinaryPath = `${EFFECTS_DIRECTORY_PATH}/${effectCommand}`;
        await executeEffectProcess(ramDiskPath, effectBinaryPath, binaryArgs, `cpp_${effectCommand}`);
        return `Binary Effect: ${effectCommand}`;
    }

    // 7. Manual Hex Color Set
    if (effectCommand === "hex") {
        const hexColorArgs = [currentLedHardware, secondaryArgument];
        const hexBinaryPath = `${EFFECTS_DIRECTORY_PATH}/writecolor`;
        await executeEffectProcess(ramDiskPath, hexBinaryPath, hexColorArgs, "hex_color");
        return `Set Color To Hex: ${secondaryArgument}`;
    }

    // 8. Named Color Lookup (Colorscan)
    try {
        const colorScanBinary = `${EFFECTS_DIRECTORY_PATH}/colorscan`;
        const colorScanArgs = [effectCommand];
        const scanExecutionOptions = { cwd: EFFECTS_DIRECTORY_PATH };

        const scanResult = await execFileAsync(colorScanBinary, colorScanArgs, scanExecutionOptions);
        const resolvedHexColor = scanResult.stdout.trim();

        const isHexValid = /^[0-9A-Fa-f]{6}$/.test(resolvedHexColor);
        
        if (isHexValid === false) {
            throw new Error(`Invalid hex color returned: ${resolvedHexColor}`);
        }

        const applyColorArgs = [currentLedHardware, resolvedHexColor];
        const applyColorBinary = `${EFFECTS_DIRECTORY_PATH}/writecolor`;

        await executeEffectProcess(ramDiskPath, applyColorBinary, applyColorArgs, `color_${effectCommand}`);
        return `Set Color To: ${effectCommand}`;
    } catch (colorLookupError) {
        logInternalError("OPCLEDparser", "named color resolution failed", colorLookupError.message);
        return "error";
    }
}

module.exports = {
    OPCLEDparser
};