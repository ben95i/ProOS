const fsPromises = require('fs/promises');

/**
 * Parses mainmenu.txt and dispatches commands to LED hosts
 * 
 * @param {string} ramDisk - RAM disk path
 * @param {string} varA - The primary command argument (e.g., 'level', 'preset_name')
 * @param {string} varB - The secondary variable argument
 * @returns {Promise<string|void>} - Returns the level string if varA is 'level', otherwise void.
 */
async function OPCLEDparser(ramDisk, varA, varB = "") {
    let levelHost = "";
    try {
        const data = await fsPromises.readFile(ramDisk + "/mainmenu.txt", 'utf-8');
        const lines = data.split('\n');
        const hosts = [];
        for (const line of lines) {
            if (line.trim() === "") continue;
            const parts = line.split('|'); 
            levelHost = parts[0]; 
            hosts.push(levelHost); 
        }

        if (varA === "fc_leds") {
            if (levelHost !== "") {
                const url = `${levelHost}/api?var=&arg=fc_leds&action=leds`; 
                try {
                    const response = await fetch(url, { signal: AbortSignal.timeout(5000) });
                    if (response.ok) {
                        const result = await response.text();
                        return result.replace(/[\\"]/g, "");
                    }
                } catch (error) {
                  return error.message;
                }
            }
        } else {
            if (hosts.length > 0) {
                const fetchPromises = hosts.map(host => {
                    const url = `${host}/api?var=${varB}&arg=${varA}&action=leds`;
                    return fetch(url, { signal: AbortSignal.timeout(5000) }).catch(() => {});
                });
                await Promise.allSettled(fetchPromises);
            }
            return "OK";
        }
    } catch (error) {
        if (error.code === 'ENOENT') {
            return "mainmenu.txt not found.";
        } else {
            return `An error occurred while parsing LED configuration: ${error.message}`;
        }
    }
}

module.exports = { OPCLEDparser };