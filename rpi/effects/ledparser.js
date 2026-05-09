/**
 * Parses mainmenu.txt and dispatches commands to LED hosts
 * 
 * @param {string} ramDisk - RAM disk path
 * @param {string} varA - The primary command argument (e.g., 'level', 'preset_name')
 * @param {string} varB - The secondary variable argument
 * @returns {Promise<string|void>} - Returns the level string if varA is 'level', otherwise void.
 */

const BIN_PATH = "/opt/rpi";
const { exec } = require('child_process');
const fsPromises = require('fs/promises');
const util = require('util');
const setleds_cmd = '/usr/bin/sudo ' + BIN_PATH + '/effects/setleds.sh';

const execAsync = util.promisify(exec);

// Replicates the constants from the shell script
const LED_PROXY = "http://127.0.0.1:7892";

/**
 * Handles LED effect dispatching.
 */
async function OPCLEDparser(ramDisk, effect = "", subArg = "") {

    if (effect === "") {
        return("no effect command, exiting."); 
    }

    if (effect === "pause") {
        await execAsync(`${setleds_cmd}`);
        return("paused"); ;
    }

    if (effect === "fc") { // set brightness level 
        await fetch(`${LED_PROXY}/set?b=${subArg}`, { signal: AbortSignal.timeout(500) }).catch(() => {}); 
        return("fc ok");
    }

    if (effect === "fc_leds") { // show brightness level
        try {
            const resp = await fetch(`${LED_PROXY}/brightness`, { signal: AbortSignal.timeout(500) }); 
            const data = await resp.text();
            return data;
        } catch (e) {
            return "error setting brightness level";
        }
    }

    if (effect === "shuffle") {
        try {
            const shuffleData = await fsPromises.readFile(`${BIN_PATH}/effects/shuffle.txt`, 'utf-8'); 
            const lines = [...new Set(shuffleData.split('\n').filter(l => l.trim() !== ''))];
            effect = lines[Math.floor(Math.random() * lines.length)].replace(/\r/g, ""); 
        } catch (e) {
            return("shuffle file not found");
        }
    }

    if (effect === "stop") {
        effect = "black"; 
    }

    // Check for mp4 video effect
    try {
        await fsPromises.access(`${BIN_PATH}/effects/videos/${effect}.mp4`); 
        await execAsync(`${setleds_cmd} mp4-${effect}`);
        return(`Set LEDs to video loop: ${effect}`); 
    } catch (e) {
        // continue to other categories
    }

    // Python effects
    const pythEffects = ["rave", "chase", "miami", "spark", "stripes", "lava"]; 
    if (pythEffects.includes(effect)) {
        await execAsync(`${setleds_cmd} pyth-${effect}`); 
        return(`Set LEDs to python effect: ${effect}`); 
    }

    // C / C++ effects
    const cEffects = ["spectro"]; 
    if (cEffects.includes(effect)) {
        await execAsync(`${setleds_cmd} c-${effect}`); 
        return(`Set LEDs to C / C++ effect: ${effect}`); 
    }

    // Hex and solid colors
    if (effect === "hex") {
        await execAsync(`${setleds_cmd} hex-${subArg}`); 
        return(`Set LEDs to RGB-HEX color: ${subArg}`); 
    }

    await execAsync(`${setleds_cmd} color-${effect}`); 
    return(`Set LEDs to solid color: ${effect}`); 
}

module.exports = { OPCLEDparser };