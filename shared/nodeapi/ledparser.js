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
        // Read file into array equivalent
        const data = await fsPromises.readFile(ramDisk + "/mainmenu.txt", 'utf-8');
        const lines = data.split('\n');
        const hosts = [];

        // Loop over the elements to split data by delimiter
        for (const line of lines) {
            if (line.trim() === "") continue;
            const parts = line.split('|'); 
            const host = parts[0]; 
            levelHost = host; // Continuously updates to hold the last host in the list
            hosts.push(host); // Append active hosts to array
        }

        if (varA === "fc_leds") {
            // Show brightness of last in list
            if (levelHost !== "") {
                const url = `${levelHost}/api?var=&arg=fc_leds&action=leds`; //
                try {
                    const response = await fetch(url, {
                        signal: AbortSignal.timeout(5000) // Mimics --max-time 5
                    });
                    if (response.ok) {
                        const result = await response.text();
                        return result.replace(/[\\"]/g, "");
                    }
                } catch (error) {
                  return error.message;
                }
            }
        } else {
            // Dispatch command to active hosts concurrently
            if (hosts.length > 0) {
                const fetchPromises = hosts.map(host => {
                    const url = `${host}/api?var=${varB}&arg=${varA}&action=leds`; //
                    
                    // Return the fetch promise, catching errors to prevent Promise.allSettled from throwing
                    return fetch(url, { 
                        signal: AbortSignal.timeout(5000) 
                    }).catch(() => {}); // Mimics sending errors to /dev/null
                });

                // Execute all requests in parallel, mimicking xargs -P 5 and backgrounding (&)
                // We don't await this if we want true "fire and forget" background execution, 
                // but awaiting allSettled is safer for cloud/serverless environments.
                Promise.allSettled(fetchPromises);
            }
            return "OK";
        }

    } catch (error) {
        if (error.code === 'ENOENT') {
            return("mainmenu.txt not found.");
        } else {
            return("An error occurred while parsing LED configuration:", error.message);
        }
    }
}

module.exports = { OPCLEDparser };