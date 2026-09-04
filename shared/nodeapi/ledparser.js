const fsPromises = require('fs/promises');

const LED_MENU_FILE = "/menus/mainmenu.txt";
const FETCH_TIMEOUT = 5000; // in (ms)

// Reads the LED hosts out of mainmenu.txt: one host per line, host|... .
async function readLEDHosts(baseWWW) {
    const data = await fsPromises.readFile(baseWWW + LED_MENU_FILE, 'utf-8');
    return data.split('\n')
        .filter(line => line.trim() !== "")
        .map(line => line.split('|')[0]);
}

/**
 * Dispatches an LED command to every host in mainmenu.txt, or reads the
 * level back from the last of them when varA is "fc_leds".
 *
 * OPCLEDparser(baseWWW, 'abstract')      -> "OK"
 * OPCLEDparser(baseWWW, 'fc_leds')       -> the level, e.g. "128"
 *
 * @param {string} baseWWW - web root path
 * @param {string} varA - the command, or 'fc_leds' to read the level
 * @param {string} varB - the secondary argument
 * @returns {Promise<string|void>}
 */
async function OPCLEDparser(baseWWW, varA, varB = "") {
    try {
        const hosts = await readLEDHosts(baseWWW);
        // the level is read from the last host listed, as it always has been
        const levelHost = hosts.length > 0 ? hosts[hosts.length - 1] : "";
        if (varA === "fc_leds") {
            if (levelHost === "") {
                return;
            }
            const url = `${levelHost}/api?var=&arg=fc_leds&action=leds`;
            try {
                const response = await fetch(url, { signal: AbortSignal.timeout(FETCH_TIMEOUT) });
                if (response.ok) {
                    const result = await response.text();
                    return result.replace(/[\\"]/g, "");
                }
            } catch (error) {
                return error.message;
            }
            return;
        }
        // every host is sent the command, and none of them is waited on for
        // an answer -- a host that is off must not hold up the rest
        const fetchPromises = hosts.map(host => {
            const url = `${host}/api?var=${varB}&arg=${varA}&action=leds`;
            return fetch(url, { signal: AbortSignal.timeout(FETCH_TIMEOUT) }).catch(() => {});
        });
        await Promise.allSettled(fetchPromises);
        return "OK";
    } catch (error) {
        if (error.code === 'ENOENT') {
            return "mainmenu.txt not found.";
        }
        return `An error occurred while parsing LED configuration: ${error.message}`;
    }
}

module.exports = { OPCLEDparser };
