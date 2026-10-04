//// Camera relay ////
//
// Streams RTSP cameras to the website's camera window:
//
//   GET /api/camera/<name>   live fragmented MP4 of that camera, played by
//                            camera.js through Media Source Extensions
//
// Every camera in CAMERAS gets its own route. Each is relayed by one ffmpeg
// process that only repackages the camera's H.264 (-c copy): nothing is
// decoded or encoded, so it costs a few percent of one core and ~20 MB.
// ffmpeg starts with the first viewer, every further viewer shares it, and
// it is stopped STOP_DELAY_MS after the last viewer leaves -- which in turn
// lets the camera release its own sensor and encoder.
//
// The video is sent one group of pictures at a time: a fragment starting at
// each keyframe (one second with rtspcam -i 15 at 15 fps). That adds about
// a second of delay but gives the browser whole seconds of evenly timed
// video to play, which it does far more smoothly than single frames.
//
// The last fragment is kept, so a new viewer sees a picture at once instead
// of waiting for the next keyframe. A viewer
// whose connection cannot keep up is skipped forward to the next keyframe
// rather than buffered for, so a slow phone never grows the server's memory.
//
// The response headers carry what the page needs before it can play:
//   X-Camera-Codec     the MediaSource codec string, e.g. avc1.640028
//   X-Camera-Rotation  clockwise degrees to turn the picture: 0/90/180/270,
//                      read from the H.264 display orientation message the
//                      camera sends (rtspcam -r), unless set in CAMERAS
//
// Needs ffmpeg on the server. lighttpd must pass the response through as it
// arrives instead of buffering it: server.stream-response-body = 2 for
// /api/camera/.

const { spawn } = require('child_process');
const { BoxReader, readVideoConfig, inspectFrame } = require('./fmp4');

// Route name -> camera. rotation: null reads it from the stream itself, or
// a number of clockwise degrees to use whatever the stream says.
const CAMERAS = {
    doorway: { url: 'rtsp://doorway.home:8554/cam', rotation: null }
};

const ROUTE = '/api/camera/:name';
const FFMPEG_PATH = 'ffmpeg';
const RTSP_TIMEOUT_US = 5000000;   // drop a camera that goes silent for 5 s
const ANALYZE_US = 1000000;        // how long ffmpeg may probe the stream
const START_TIMEOUT_MS = 15000;    // no keyframe by then: give up and retry
const STOP_DELAY_MS = 5000;        // keep running this long after the last viewer
const MAX_CACHED_FRAGMENTS = 30;   // cap on the kept group of pictures

// ffmpeg reads the camera over TCP (no lost packets on Wi-Fi), copies the
// video untouched and writes one MP4 fragment per keyframe, flushed at once.
const ffmpegArgs = (url) => [
    '-hide_banner', '-loglevel', 'error', '-nostdin',
    '-rtsp_transport', 'tcp',
    '-timeout', String(RTSP_TIMEOUT_US),
    '-fflags', 'nobuffer',
    '-analyzeduration', String(ANALYZE_US),
    '-i', url,
    '-map', '0:v:0', '-c', 'copy', '-an', '-sn', '-dn',
    '-f', 'mp4',
    '-movflags', '+frag_keyframe+empty_moov+default_base_moof',
    '-flush_packets', '1',
    'pipe:1'
];

// One camera: its ffmpeg process and the browsers watching it.
class CameraRelay {
    constructor(name, settings) {
        this.name = name;
        this.settings = settings;
        this.viewers = new Set();
        this.process = null;
        this.isStopping = false;
        this.stopTimer = null;
        this.startTimer = null;
        this.resetStream();
    }

    // forgets everything learned from the last ffmpeg run
    resetStream() {
        this.initParts = [];
        this.initSegment = null;
        this.videoConfig = null;
        this.pendingMoof = null;
        this.groupOfPictures = [];
        this.rotation = null;
        this.isReady = false;
    }

    log(message) {
        console.log(`camera ${this.name}: ${message}`);
    }

    //// viewers ////

    // A browser asked for the stream. It is answered at once when the relay
    // is already running, or as soon as the first keyframe arrives.
    addViewer(res) {
        const viewer = {
            res,
            isAdmitted: false,     // headers and init segment sent
            needsKeyframe: true,   // waiting to start (or restart) at a keyframe
            isCongested: false     // its connection is full; skip until drained
        };
        this.viewers.add(viewer);
        res.on('close', () => this.removeViewer(viewer));
        clearTimeout(this.stopTimer);
        this.stopTimer = null;
        // a relay being shut down is left to finish; it restarts for this
        // viewer as soon as it has
        if (this.isReady && !this.isStopping) {
            this.admitViewer(viewer);
            return;
        }
        if (!this.process) {
            this.startProcess();
        }
    }

    // The browser went away. The last one out starts the stop timer, so a
    // page reload or a quick reopen does not restart the camera.
    removeViewer(viewer) {
        if (!this.viewers.delete(viewer)) {
            return;
        }
        if (this.viewers.size > 0 || !this.process || this.stopTimer) {
            return;
        }
        this.stopTimer = setTimeout(() => {
            this.stopTimer = null;
            if (this.viewers.size === 0) {
                this.stopProcess();
            }
        }, STOP_DELAY_MS);
    }

    // Sends the headers, the init segment and the kept group of pictures,
    // after which the viewer gets every new frame live.
    admitViewer(viewer) {
        viewer.isAdmitted = true;
        viewer.res.writeHead(200, {
            'Content-Type': 'video/mp4',
            'Cache-Control': 'no-store',
            'X-Accel-Buffering': 'no',
            'X-Camera-Codec': this.videoConfig.codec,
            'X-Camera-Rotation': String(this.rotation)
        });
        if (viewer.res.socket) {
            viewer.res.socket.setNoDelay(true);
        }
        if (!this.send(viewer, this.initSegment)) {
            return;
        }
        if (this.groupOfPictures.length === 0) {
            return; // nothing kept: it starts at the next keyframe
        }
        viewer.needsKeyframe = false;
        for (const fragment of this.groupOfPictures) {
            if (!this.send(viewer, fragment)) {
                return;
            }
        }
    }

    // Writes to one viewer. When its connection is full the data just
    // written still goes out, but nothing more is queued for it: once it
    // drains it picks up again at the next keyframe.
    send(viewer, data) {
        if (viewer.res.write(data)) {
            return true;
        }
        viewer.isCongested = true;
        viewer.res.once('drain', () => {
            viewer.isCongested = false;
            viewer.needsKeyframe = true;
        });
        return false;
    }

    broadcast(fragment, isKeyframe) {
        for (const viewer of this.viewers) {
            if (!viewer.isAdmitted || viewer.isCongested) {
                continue;
            }
            if (viewer.needsKeyframe) {
                if (!isKeyframe) {
                    continue;
                }
                viewer.needsKeyframe = false;
            }
            this.send(viewer, fragment);
        }
    }

    //// ffmpeg ////

    startProcess() {
        this.resetStream();
        this.isStopping = false;
        const child = spawn(FFMPEG_PATH, ffmpegArgs(this.settings.url), {
            stdio: ['ignore', 'pipe', 'pipe']
        });
        this.process = child;
        this.log('relay started');

        let lastError = 'stream ended';
        let hasEnded = false;
        const ended = () => {
            if (hasEnded) return;
            hasEnded = true;
            this.processEnded(child, lastError);
        };

        const boxReader = new BoxReader((type, box) => this.handleBox(type, box));
        child.stdout.on('data', (chunk) => {
            try {
                boxReader.push(chunk);
            } catch (err) {
                lastError = err.message;
                child.kill('SIGKILL');
            }
        });
        // ffmpeg only prints errors at this log level; the last line is the
        // reason it stopped
        child.stderr.on('data', (text) => {
            const lines = text.toString().trim().split('\n');
            lastError = lines[lines.length - 1];
        });
        child.on('error', (err) => {
            lastError = err.message;
            ended();
        });
        child.on('close', ended);

        clearTimeout(this.startTimer);
        this.startTimer = setTimeout(() => {
            if (this.process === child && !this.isReady) {
                lastError = 'no video from camera';
                child.kill('SIGKILL');
            }
        }, START_TIMEOUT_MS);
    }

    stopProcess() {
        if (!this.process) {
            return;
        }
        this.isStopping = true;
        this.process.kill('SIGTERM');
    }

    // ffmpeg has gone. Viewers that were watching are ended (the page
    // reconnects by itself); viewers still waiting get an error, unless the
    // stop was ours and they arrived during it, in which case ffmpeg is
    // simply started again for them.
    processEnded(child, reason) {
        if (this.process !== child) {
            return;
        }
        this.process = null;
        clearTimeout(this.startTimer);
        const wasStopping = this.isStopping;
        this.isStopping = false;
        if (wasStopping) {
            this.log('relay stopped');
        } else {
            this.log(`relay ended: ${reason}`);
        }
        for (const viewer of [...this.viewers]) {
            if (viewer.isAdmitted) {
                this.viewers.delete(viewer);
                viewer.res.end();
            } else if (!wasStopping) {
                this.viewers.delete(viewer);
                viewer.res.status(503).json(`camera ${this.name} unavailable: ${reason}`);
            }
        }
        this.resetStream();
        if (this.viewers.size > 0) {
            this.startProcess();
        }
    }

    //// the stream ////

    handleBox(type, box) {
        switch (type) {
            case 'ftyp':
                this.initParts = [box];
                break;
            case 'moov':
                this.initParts.push(box);
                this.initSegment = Buffer.concat(this.initParts);
                this.initParts = [];
                this.videoConfig = readVideoConfig(box);
                if (!this.videoConfig) {
                    throw new Error('camera stream has no H.264 video');
                }
                break;
            case 'moof':
                this.pendingMoof = box;
                break;
            case 'mdat':
                this.handleFrame(box);
                break;
            default:
                break; // nothing else is needed by the browser
        }
    }

    // One fragment (moof + mdat): a keyframe and the frames after it. Looks
    // at it once for everyone, keeps it if it belongs to the current group
    // of pictures, and passes it on.
    handleFrame(mdat) {
        const moof = this.pendingMoof;
        this.pendingMoof = null;
        if (!moof || !this.videoConfig) {
            return;
        }
        const isRotationUnknown = this.rotation === null;
        const frameInfo = inspectFrame(mdat, this.videoConfig.nalLengthSize, isRotationUnknown);
        if (isRotationUnknown && frameInfo.rotation !== null) {
            this.rotation = frameInfo.rotation;
        }

        const fragment = Buffer.concat([moof, mdat]);
        if (frameInfo.isKeyframe) {
            this.groupOfPictures = [fragment];
        } else if (this.groupOfPictures.length > 0) {
            if (this.groupOfPictures.length < MAX_CACHED_FRAGMENTS) {
                this.groupOfPictures.push(fragment);
            } else {
                this.groupOfPictures = []; // too long to keep; start at keyframes
            }
        }

        if (!this.isReady) {
            if (frameInfo.isKeyframe) {
                this.becomeReady();
            }
            return;
        }
        this.broadcast(fragment, frameInfo.isKeyframe);
    }

    // The first keyframe is in: the codec and the rotation are known, so
    // everyone waiting can be answered.
    becomeReady() {
        clearTimeout(this.startTimer);
        const configuredRotation = this.settings.rotation;
        if (typeof configuredRotation === 'number') {
            this.rotation = configuredRotation;
        } else if (this.rotation === null) {
            this.rotation = 0; // the camera sends no orientation
        }
        this.isReady = true;
        this.log(`streaming ${this.videoConfig.codec}, rotation ${this.rotation}`);
        for (const viewer of this.viewers) {
            if (!viewer.isAdmitted) {
                this.admitViewer(viewer);
            }
        }
    }
}

// Mounts one route for every camera in CAMERAS.
function registerCameraRoutes(app) {
    const relays = new Map();
    for (const [name, settings] of Object.entries(CAMERAS)) {
        relays.set(name, new CameraRelay(name, settings));
    }

    app.get(ROUTE, (req, res) => {
        const relay = relays.get(req.params.name);
        if (!relay) {
            return res.status(404).json(`unknown camera`);
        }
        relay.addViewer(res);
    });

    // ffmpeg would notice the closed pipe on its own; this is quicker
    process.on('exit', () => {
        for (const relay of relays.values()) {
            relay.stopProcess();
        }
    });
}

module.exports = { registerCameraRoutes };
