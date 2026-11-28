//// Automate REST API - Server Commands ////
//
// The Server window's commands (shared/html/server.js), streamed to the page as
// they run:
//   GET /api/svrcmd?cmd=<command>&out=<output file>&arg=<argument>
// starts the command and follows its output file, ram/<out>.txt, sending one
// JSON event per line (application/x-ndjson) as the output grows:
//   {"type":"waiting","command":"mnt_ram_region"}   another command holds the
//                                                    file; this one starts after it
//   {"type":"start","command":"mnt_arch_region"}    the run began
//   {"type":"text","text":"attaching archive...\n"} new output, whole lines
//   {"type":"done","code":0}                         the run ended, its exit code
//   {"type":"error","message":"..."}                 it did not start, or its
//                                                    output was lost
// The command is started as every server command is, through executeMain():
// on the Proxmox host 'server' drops a trigger file that file-monitor.sh runs,
// and on a Pi it runs servercmds.sh ('confwpa' with its argument, the Pi's
// Wi-Fi setup). Both write the run's output between two marker lines:
//   @START|<command>|<epoch ms>
//   @END|<command>|<exit code>|<epoch ms>
// A run is the one asked for when its @START names the command and is no older
// than the request, so an earlier run left in the file is never shown as new.
//
// Every page (every user) goes through this API, so it keeps one run per output
// file and hands it to everyone who asks:
//   - a request for the command already running on its file joins that run: it
//     is sent every event so far, then the live ones, and no second run starts;
//   - a request for another command on a file in use (the region commands share
//     regions.out.save.txt) is told it is waiting, and starts once that run ends,
//     so no run's output is emptied away while someone is reading it;
//   - a run is followed to its end even when every page has gone, so a page
//     opened later can join it, and the file's next run waits for it.
//
// Mounted with one line in server.js:
//   const { registerSvrCmdRoutes } = require('./svrcmd');
//   registerSvrCmdRoutes(app);

const fsPromises = require('fs/promises');
const path = require('path');
const { query, validationResult } = require('express-validator');
const { executeMain } = require('./controller');

//// settings ////

const ROUTE = '/api/svrcmd';
const BASE_WWW = '/var/www/html';
// the RAM disk: on the server the host's, bind-mounted into the automate LXC
const RAM_DIR = path.join(BASE_WWW, 'ram');
// a command, as its trigger file is named: mnt_arch_region
const COMMAND_PATTERN = /^[A-Za-z0-9_]+$/;
// an output file and nothing else in ram/: regions.out.save
const OUTPUT_PATTERN = /^[A-Za-z0-9_-]+\.out\.save$/;
// the API's character set for a query value (server.js); free text comes as
// URL-safe base64
const ARG_PATTERN = /^[A-Za-z0-9_.#%@&-]*$/;
// the Pi's Wi-Fi setup, the one command that takes an argument
const WIFI_SETUP_COMMAND = 'confwpa';
// how often the output file is read while following a run
const POLL_MS = 250;
// how long the run may take to begin once started (the host's own lock on the
// file, or a busy file-monitor)
const START_TIMEOUT = 120000; // in (ms)
// a run going this long is given up on
const RUN_TIMEOUT = 1800000; // in (ms)
const START_MARK = '@START';
const END_MARK = '@END';

// the run on each output file, running or about to: { 'regions.out.save': CommandRun }
const activeRuns = new Map();

//// reading a run ////

// Waits a while: await sleep(250)
function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

// Starts a command as the page's Server window would, and answers what the
// controller does: "OK" from the server's trigger, the Pi's own answer, or a
// failure's text. startServerCommand('syslog', '') -> "OK"
function startServerCommand(command, commandArg) {
    if (command === WIFI_SETUP_COMMAND) {
        return executeMain(BASE_WWW, WIFI_SETUP_COMMAND, commandArg);
    }
    return executeMain(BASE_WWW, 'server', command);
}

// The output file's text, or '' while it does not exist yet.
async function readOutput(outputPath) {
    try {
        return await fsPromises.readFile(outputPath, 'utf8');
    } catch (error) {
        if (error.code === 'ENOENT') {
            return '';
        }
        throw error;
    }
}

// Finds a run in its output file's text: the first @START naming the command at
// or after the given time, its whole lines of output so far, and its exit code
// once its @END is written; null while it has not begun.
// findRun("@START|syslog|1700000000500\nok\n@END|syslog|0|1700000001000\n", 'syslog', 1700000000000)
//   -> { startTime: 1700000000500, output: 'ok\n', ended: true, code: 0 }
function findRun(fileText, command, requested) {
    const lines = fileText.split('\n');
    // the last piece is a line still being written, or '' after the last newline
    lines.pop();
    let startIndex = -1;
    let startTime = 0;
    for (let index = 0; index < lines.length; index += 1) {
        const fields = lines[index].split('|');
        const isStart = fields[0] === START_MARK && fields[1] === command;
        if (!isStart) {
            continue;
        }
        const markTime = Number(fields[2]);
        if (markTime >= requested) {
            startIndex = index;
            startTime = markTime;
            break;
        }
    }
    if (startIndex === -1) {
        return null;
    }
    const outputLines = [];
    for (let index = startIndex + 1; index < lines.length; index += 1) {
        const fields = lines[index].split('|');
        const isEnd = fields[0] === END_MARK && fields[1] === command;
        if (isEnd) {
            const exitCode = Number(fields[2]);
            return { startTime: startTime, output: joinLines(outputLines), ended: true, code: exitCode };
        }
        outputLines.push(lines[index]);
    }
    return { startTime: startTime, output: joinLines(outputLines), ended: false, code: null };
}

// whole lines back into text, each ending in a newline: ['a', 'b'] -> "a\nb\n"
function joinLines(lines) {
    let text = '';
    for (const line of lines) {
        text += line + '\n';
    }
    return text;
}

//// one run, shared ////

/**
 * One run of a command on its output file, and the pages following it. Every
 * event sent is kept, so a page joining late is played them all before the
 * live ones. `ended` settles once the run is over, which a request waiting for
 * the file awaits.
 */
class CommandRun {
    constructor(command, outputFile, commandArg) {
        this.command = command;
        this.outputFile = outputFile;
        this.commandArg = commandArg;
        this.lines = [];
        this.viewers = new Set();
        this.finished = false;
        this.ended = new Promise(resolve => {
            this.markEnded = resolve;
        });
    }

    // Sends an event to every page following and keeps it for later ones.
    // run.send({ type: 'text', text: 'detached all region(s).\n' })
    send(event) {
        if (this.finished) {
            return;
        }
        const line = JSON.stringify(event) + '\n';
        this.lines.push(line);
        for (const viewer of this.viewers) {
            writeLine(viewer, line);
        }
    }

    // Plays a page every event so far, then follows it with the live ones.
    addViewer(res) {
        for (const line of this.lines) {
            writeLine(res, line);
        }
        if (this.finished) {
            res.end();
            return;
        }
        this.viewers.add(res);
        res.on('close', () => {
            this.viewers.delete(res);
        });
    }

    // Sends the last event, ends every page's stream and frees the output file
    // for its next run.
    finish(event) {
        this.send(event);
        this.finished = true;
        for (const viewer of this.viewers) {
            if (!viewer.writableEnded) {
                viewer.end();
            }
        }
        this.viewers.clear();
        if (activeRuns.get(this.outputFile) === this) {
            activeRuns.delete(this.outputFile);
        }
        this.markEnded();
    }

    // Starts the command and follows its output file until the run ends or fails
    // (see the top), whether or not any page is still following.
    async follow() {
        const outputPath = path.join(RAM_DIR, `${this.outputFile}.txt`);
        const requested = Date.now();
        // the controller's answer, when it says the command could not be run
        let startFailure = null;
        startServerCommand(this.command, this.commandArg)
            .then(answer => {
                const answerText = String(answer);
                if (answerText.includes('exception') || answerText.startsWith('empty')) {
                    startFailure = answerText;
                }
            })
            .catch(error => {
                startFailure = error.message;
            });

        let startTime = null;
        let sentLength = 0;
        while (true) {
            await sleep(POLL_MS);
            const fileText = await readOutput(outputPath);
            const run = findRun(fileText, this.command, requested);
            const elapsed = Date.now() - requested;
            if (startTime !== null && (run === null || run.startTime !== startTime)) {
                return { type: 'error', message: `${this.command}'s output was replaced by a newer run` };
            }
            if (run === null) {
                if (startFailure !== null) {
                    return { type: 'error', message: `${this.command} could not be run: ${startFailure}` };
                }
                if (elapsed > START_TIMEOUT) {
                    return { type: 'error', message: `${this.command} did not start` };
                }
                continue;
            }
            if (startTime === null) {
                startTime = run.startTime;
                this.send({ type: 'start', command: this.command });
            }
            if (run.output.length > sentLength) {
                this.send({ type: 'text', text: run.output.slice(sentLength) });
                sentLength = run.output.length;
            }
            if (run.ended) {
                return { type: 'done', code: run.code };
            }
            if (elapsed > RUN_TIMEOUT) {
                return { type: 'error', message: `${this.command} is still running; stopped following it` };
            }
        }
    }
}

// writes one event line to a page, unless its stream has ended
function writeLine(res, line) {
    if (!res.destroyed && !res.writableEnded) {
        res.write(line);
    }
}

// Starts a run on a free output file and marks the file as in use.
function startRun(command, outputFile, commandArg) {
    const run = new CommandRun(command, outputFile, commandArg);
    activeRuns.set(outputFile, run);
    run.follow()
        .then(lastEvent => run.finish(lastEvent))
        .catch(error => run.finish({
            type: 'error',
            message: `reading ${command}'s output failed: ${error.message}`
        }));
    return run;
}

// Hands a request its run (see the top): joins the command's run on the file,
// or starts one once the file is free, telling the page while it waits. A
// command asked for always runs, even when its page goes while it waits.
async function followCommand(req, res) {
    const command = req.query.cmd;
    const outputFile = req.query.out;
    const commandArg = req.query.arg || '';
    let viewerGone = false;
    res.writeHead(200, {
        'Content-Type': 'application/x-ndjson',
        'Cache-Control': 'no-store',
        'X-Accel-Buffering': 'no'
    });
    if (res.socket) {
        res.socket.setNoDelay(true);
    }
    res.on('close', () => {
        viewerGone = true;
    });
    // the command last named in a 'waiting' event, so each is sent once
    let waitingFor = null;
    while (true) {
        const current = activeRuns.get(outputFile);
        if (current === undefined) {
            const run = startRun(command, outputFile, commandArg);
            if (!viewerGone) {
                run.addViewer(res);
            }
            return;
        }
        if (current.command === command) {
            if (!viewerGone) {
                current.addViewer(res);
            }
            return;
        }
        if (waitingFor !== current.command) {
            waitingFor = current.command;
            writeLine(res, JSON.stringify({ type: 'waiting', command: current.command }) + '\n');
        }
        await current.ended;
    }
}

//// routes ////

// answers the validation errors, or null when the request is good
function badRequest(req, res) {
    const errs = validationResult(req);
    if (errs.isEmpty()) {
        return null;
    }
    return res.status(400).json({ errors: errs.array() });
}

function registerSvrCmdRoutes(app) {
    app.get(ROUTE, [
        query('cmd').matches(COMMAND_PATTERN),
        query('out').matches(OUTPUT_PATTERN),
        query('arg').optional().matches(ARG_PATTERN)
    ], (req, res) => {
        if (badRequest(req, res)) return;
        followCommand(req, res);
    });
}

module.exports = { registerSvrCmdRoutes };
