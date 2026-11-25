// The LED hosts, and the effects Shuffle picks from, as opts.json lists them.
// The Pis pick from the same list in their own copy (rpi/ledparser.js).
const { LED_HOSTS, LED_SHUFFLE } = require('./options');

// Every other device call in the API gives up after 3 s as well.
const SEND_TIMEOUT = 3000; // in (ms)
// How long the Control menu waits for the first host to report its level.
const LEVEL_READ_TIMEOUT = 1500; // in (ms)
// How long a command waits on the hosts before the page is answered. A host
// still busy after it is reported as not answering, and its command carries
// on in the background.
const ANSWER_WAIT = 1500; // in (ms)

// The page reads this prefix as "some hosts did not answer" and shows the
// message (LED_NO_ANSWER in dropdowns.js). The Pis' own replies never start
// with it.
const NO_ANSWER_PREFIX = "No answer from";

// The Pixels window's host list, and its commands for one host, which is
// named in the second argument as its name in opts.json: its status, and
// setting its level.
const HOST_LIST_COMMAND = "fc_host_list";
const HOST_STATUS_COMMAND = "fc_host_status";
const HOST_SET_PREFIX = "fc_host_set_"; // followed by the level, 0-255

// what a host's fc_status (rpi/ledparser.js) may answer
const HOST_STATES = ["off", "active", "paused", "color"];
const HOST_EFFECT_PATTERN = /^[A-Za-z0-9_-]+$/; // an effect name, as in LED_EFFECTS in html/opts.js
const HOST_COLOR_PATTERN = /^[0-9A-F]{6}$/;
const LEVEL_MAX = 255;

// One send queue per host, kept for the life of the API, by host URL.
const hostLanes = new Map();

/**
 * The commands for one host. At most one request is in flight to the host,
 * and at most one command of each kind waits behind it: 'level' for the
 * brightness, 'scene' for everything else. A newer command replaces the one
 * of its kind that is waiting, so a dragged slider or colour map never builds
 * a queue, the host gets its commands in order, and the last value always
 * lands. A host that is slow or off only holds up its own queue.
 */
class HostLane {
    constructor(host) {
        this.host = host;
        this.busy = false;
        // kind -> { command, value, resolvers }
        this.waiting = new Map();
    }

    // Resolves true once the host has answered the command, or the newer
    // command that replaced it, and false when it did not answer.
    send(kind, command, value) {
        return new Promise(resolve => {
            let resolvers = [resolve];
            const replaced = this.waiting.get(kind);
            if (replaced) {
                // the replaced command is never sent, so whoever was waiting
                // on it hears how its replacement went
                resolvers = replaced.resolvers.concat(resolvers);
            }
            this.waiting.set(kind, { command, value, resolvers });
            this.drain();
        });
    }

    async drain() {
        if (this.busy) {
            return;
        }
        this.busy = true;
        while (this.waiting.size > 0) {
            const [kind, next] = this.waiting.entries().next().value;
            this.waiting.delete(kind);
            const answered = await this.request(next.command, next.value);
            for (const resolve of next.resolvers) {
                resolve(answered);
            }
        }
        this.busy = false;
    }

    async request(command, value) {
        const query = new URLSearchParams({ var: value, arg: command, action: 'leds' });
        const url = `${this.host.url}/api?${query}`;
        for (let attempt = 1; attempt <= 2; attempt++) {
            try {
                const response = await fetch(url, { signal: AbortSignal.timeout(SEND_TIMEOUT) });
                // the body is read through so the connection can be reused
                await response.text();
                return response.ok;
            } catch (error) {
                // A failure that comes at once, such as a kept-alive
                // connection the Pi has since closed, is worth one more try.
                // A host that timed out would only time out again.
                if (error.name === 'TimeoutError') {
                    return false;
                }
            }
        }
        return false;
    }
}

function laneFor(host) {
    let lane = hostLanes.get(host.url);
    if (!lane) {
        lane = new HostLane(host);
        hostLanes.set(host.url, lane);
    }
    lane.host = host;
    return lane;
}

// Sends one command to every host at once. The brightness and colours are
// sent many times a second while they are dragged, so the page is answered
// at once for those; anything else waits up to ANSWER_WAIT and names the
// hosts that did not answer.
async function sendToAllHosts(hosts, command, value) {
    let kind = "scene";
    if (command.startsWith("fc_set")) {
        kind = "level";
    }
    const sends = hosts.map(host => laneFor(host).send(kind, command, value));
    if (kind === "level" || command === "hex") {
        return "OK";
    }
    let deadline;
    const timedOut = new Promise(resolve => {
        deadline = setTimeout(() => resolve(false), ANSWER_WAIT);
    });
    const answers = await Promise.all(sends.map(sent => Promise.race([sent, timedOut])));
    clearTimeout(deadline);
    const silentHosts = hosts.filter((host, index) => answers[index] === false);
    if (silentHosts.length === 0) {
        return "OK";
    }
    return `${NO_ANSWER_PREFIX} ${silentHosts.map(host => host.name).join(", ")}`;
}

// Asks one host for its brightness and answers it as text, "0" to "255".
// Throws when the host does not answer with a level within LEVEL_READ_TIMEOUT.
async function readHostLevel(host) {
    const url = `${host.url}/api?var=&arg=fc_leds&action=leds`;
    const response = await fetch(url, { signal: AbortSignal.timeout(LEVEL_READ_TIMEOUT) });
    const level = (await response.text()).replace(/[\\"]/g, "").trim();
    if (!response.ok || !/^\d+$/.test(level)) {
        throw new Error(`${host.name} answered ${level}`);
    }
    return level;
}

// Asks one host what it is showing and how bright, and answers it checked,
// so only these values ever reach the page:
//   { effect: "maze", state: "active", color: null, brightness: 128 }
// effect and color are null where they do not apply. Throws when the host
// does not answer with a status and a level within LEVEL_READ_TIMEOUT.
async function readHostStatus(host) {
    const url = `${host.url}/api?var=&arg=fc_status&action=leds`;
    const response = await fetch(url, { signal: AbortSignal.timeout(LEVEL_READ_TIMEOUT) });
    const status = await response.json();
    if (!response.ok || status === null || typeof status !== 'object') {
        throw new Error(`${host.name} answered no status`);
    }
    const brightness = status.brightness;
    const levelValid = Number.isInteger(brightness) && brightness >= 0 && brightness <= LEVEL_MAX;
    if (!levelValid || !HOST_STATES.includes(status.state)) {
        throw new Error(`${host.name} answered an unreadable status`);
    }
    let effect = null;
    if (typeof status.effect === 'string' && HOST_EFFECT_PATTERN.test(status.effect)) {
        effect = status.effect;
    }
    let color = null;
    if (typeof status.color === 'string' && HOST_COLOR_PATTERN.test(status.color)) {
        color = status.color;
    }
    return { effect, state: status.state, color, brightness };
}

// Asks every host for the brightness at once and answers with the first
// level that comes back, so one host that is off does not hold up the menu.
async function readLevel(hosts) {
    const reads = hosts.map(host => readHostLevel(host));
    try {
        return await Promise.any(reads);
    } catch (error) {
        return "no LED host answered";
    }
}

// Reads the status of, or sets the brightness of, the one host named
// hostName, for the Pixels window, which asks each host on its own so a host
// that is off only leaves its own box empty.
//
// hostCommand(hosts, 'fc_host_status', 'LEDwall')  -> readHostStatus()'s object,
//                                                     or "No answer from LEDwall"
// hostCommand(hosts, 'fc_host_set_120', 'LEDwall') -> "OK" once LEDwall has taken it
async function hostCommand(hosts, command, hostName) {
    const host = hosts.find(candidate => candidate.name === hostName);
    if (!host) {
        return `no LED host named ${hostName}`;
    }
    const noAnswer = `${NO_ANSWER_PREFIX} ${host.name}`;
    if (command === HOST_STATUS_COMMAND) {
        try {
            return await readHostStatus(host);
        } catch (error) {
            return noAnswer;
        }
    }
    const level = command.slice(HOST_SET_PREFIX.length);
    if (!/^\d+$/.test(level)) {
        return `invalid level ${level}`;
    }
    // through the host's own queue as a brightness, the same way the Control
    // slider's level goes, so the two never race on one host
    const answered = await laneFor(host).send("level", `fc_set_${level}`, "");
    if (answered) {
        return "OK";
    }
    return noAnswer;
}

// Picks the Shuffle effect here, so every host plays the same one. Without
// the list, each host is sent "shuffle" and picks its own.
function pickShuffleEffect() {
    if (LED_SHUFFLE.length === 0) {
        return "shuffle";
    }
    const randomIndex = Math.floor(Math.random() * LED_SHUFFLE.length);
    return LED_SHUFFLE[randomIndex];
}

/**
 * Sends an LED command to every host in opts.json, or reads the level back
 * when varA is "fc_leds". fc_host_list answers the hosts themselves; the
 * other fc_host_ commands reach one host only, named in varB.
 *
 * OPCLEDparser(baseWWW, 'abstract')      -> "OK", or "No answer from LEDwall"
 * OPCLEDparser(baseWWW, 'fc_set_120')    -> "OK" at once
 * OPCLEDparser(baseWWW, 'fc_leds')       -> the level, e.g. "128"
 * OPCLEDparser(baseWWW, 'fc_host_list')  -> [{ url: 'http://192.168.1.12:80', name: 'LEDwall' }]
 * OPCLEDparser(baseWWW, 'fc_host_status', 'LEDwall')  -> LEDwall's status object
 * OPCLEDparser(baseWWW, 'fc_host_set_120', 'LEDwall') -> "OK"
 *
 * @param {string} baseWWW - the web root; not used here, and kept so the call
 *   matches the Pis' rpi/ledparser.js
 * @param {string} varA - the command, or 'fc_leds' to read the level
 * @param {string} varB - the secondary argument, or the host's name
 * @returns {Promise<string|object>}
 */
async function OPCLEDparser(baseWWW, varA, varB = "") {
    try {
        const hosts = LED_HOSTS;
        if (varA === HOST_LIST_COMMAND) {
            return hosts;
        }
        if (hosts.length === 0) {
            return "no LED hosts in opts.json";
        }
        if (varA === "fc_leds") {
            return await readLevel(hosts);
        }
        if (varA === HOST_STATUS_COMMAND || varA.startsWith(HOST_SET_PREFIX)) {
            return await hostCommand(hosts, varA, varB);
        }
        let command = varA;
        if (command === "shuffle") {
            command = pickShuffleEffect();
        }
        return await sendToAllHosts(hosts, command, varB);
    } catch (error) {
        return `An error occurred while parsing LED configuration: ${error.message}`;
    }
}

module.exports = { OPCLEDparser };
