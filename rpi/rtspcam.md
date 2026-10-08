# rtspcam — IMX462 RTSP Camera Bridge

Oct 5, 2026 · @Ben

## Overview

rtspcam is a single C++ program on the doorway Pi that captures the IMX462 camera, encodes H.264 in hardware and serves it as RTSP at `rtsp://doorway.home:8554/cam`. Everything runs inside one GStreamer pipeline hosted by gst-rtsp-server: no pipes, no extra processes, no software encoding.

- **Source file:** `rtspcam.cpp` (about 1720 lines), built to `/opt/rpi/rtspcam`.
- **Service:** `rpi-rtspcam.service`, started before `rpi-motion.service`.
- **Data path:** libcamerasrc → v4l2h264enc → (optional orientation tag) → rtph264pay → RTSP clients.
- **On demand:** the camera, ISP and encoder only run while at least one RTSP client is connected; all clients share one capture and one encode.
- **Network:** listens on all IPv4 interfaces, so any device on the LAN can connect.
- **Night mode:** at night the ISP outputs greyscale to remove the IR magenta cast; in daytime no correction is applied. Switched by `ldrcmd.sh` with curl through a small loopback-only HTTP control server (see Night mode).

## Hardware and OS

The camera is a native Arducam IMX462 module (board LW-021) on a Raspberry Pi Zero 2 W (512 MB RAM) running Raspberry Pi OS Trixie, 32-bit (armhf). Hostname: `doorway` (`doorway.home`).

| Item | Value |
| --- | --- |
| Sensor driver | imx290 (libcamera reports `/base/soc/i2c0mux/i2c@1/imx290@1a`) |
| libcamera tuning file | `/usr/share/libcamera/ipa/rpi/vc4/imx462.json` |
| Pipeline handler | rpi/vc4 (Unicam + bcm2835-isp) |
| Sensor mode used | 1920x1080, 10-bit (SRGGB10) |
| H.264 encoder | bcm2835-codec via V4L2 (`v4l2h264enc`), max 1920 px per side, level 4 |
| Root filesystem | read-only (overlayfs); persistent changes need the overlay disabled |

The contiguous memory pool (CMA) must be enlarged for the camera, ISP and encoder buffers. `/boot/firmware/config.txt` carries `dtoverlay=cma,cma-128` (128 MB); the default pool runs out and causes lockups. Check it with `grep -i cma /proc/meminfo` (expect `CmaTotal` ≈ 131072 kB).

The IR LEDs and IR-cut filter are handled by the camera board and a separate day/night service (`rpi-ldrdetect.service`); rtspcam does not control them, but follows the same day/night decision to switch its picture mode (see Night mode).

## Pipeline

`buildPipelineDescription()` produces this launch string (shown with the default options), which the RTSP media factory instantiates per media:

```
( libcamerasrc name=camera sensor-config="sensor/config,width=1920,height=1080,depth=10"
  ! video/x-raw,format=I420,width=1280,height=720,framerate=15/1
  ! v4l2h264enc extra-controls="controls,video_bitrate=2000000,h264_i_frame_period=15,repeat_sequence_header=1"
  ! video/x-h264,level=(string)4,profile=(string)high
  ! rtph264pay name=pay0 pt=96 config-interval=0 )
```

1. **libcamerasrc** (named `camera`, so night mode can find it) opens the sensor in the full 1920x1080 10-bit mode (`sensor-config`), and the ISP scales to the output size. This property needs gstreamer1.0-libcamera 0.7.1+rpt20260429 or newer.
2. **Caps filter** fixes the raw format (I420), output size and frame rate.
3. **v4l2h264enc** encodes on the hardware encoder. Frames are copied into the encoder's own buffers (the default `output-io-mode`); the zero-copy dmabuf import mode fails on this hardware and must not be used. `repeat_sequence_header=1` makes the encoder emit SPS/PPS before every keyframe, so late joiners can decode.
4. **H.264 caps** pin High profile, level 4.
5. **rtph264pay** (`pay0`, payload type 96) packetises for RTP. `config-interval=0` because SPS/PPS are already in-band; there is no `h264parse` and no queue, so no extra threads or buffered frames.

When rotation is enabled, a pad probe on `pay0`'s sink pad inserts an orientation SEI before each frame (see Rotation).

## Options and constants

All options are validated strictly at startup; any invalid value prints the problem and the usage line, then exits with status 1.

| Option | Meaning | Default | Accepted |
| --- | --- | --- | --- |
| `-w <width>` | Output width (px) | 1280 | 16–1920, even |
| `-h <height>` | Output height (px) | 720 | 16–1920, even |
| `-f <fps>` | Frame rate | 15 | 1–120 |
| `-b <bitrate>` | H.264 bitrate (bit/s) | 2000000 | 10000–25000000 |
| `-i <frames>` | Keyframe interval (frames) | 15 | 1–1000 |
| `-s <WxH:bits>` | Sensor mode | 1920x1080:10 | W, H 1–8192; bits 8–16 |
| `-p <port>` | RTSP port | 8554 | 1–65535 |
| `-m <mount>` | RTSP path | /cam | starts with `/`, not just `/`, no spaces, `?` or `#` |
| `-r <degrees>` | Display rotation, clockwise | 0 | 0, 90 or 270 |
| `-c <port>` | Day/night control HTTP port (127.0.0.1 only) | 8556 | 1–65535, not the RTSP port |

Numbers are parsed with `strtol` and must be whole decimal strings (no trailing characters, no overflow). `-h` is height, not help. Keep `-i` equal to `-f` (one keyframe per second): downstream consumers assume roughly 1-second groups of pictures.

Fixed settings live as constants at the top of the file:

| Constant | Value | Purpose |
| --- | --- | --- |
| `LISTEN_ADDRESS` | `"0.0.0.0"` | Bind address; serves the whole LAN. Set `"127.0.0.1"` for local-only. |
| `SESSION_CLEANUP_SECONDS` | 10 | How often expired RTSP sessions are reaped. |
| `MAX_FRAME_DIMENSION` | 1920 | Encoder size limit used by `-w`/`-h` validation. |
| `MAX_BITRATE` | 25000000 | Encoder bitrate limit used by `-b` validation. |
| `MIN_…` / `MAX_…` option limits | see table above | Every accepted range is a named constant. |
| `SHUTDOWN_SIGNALS` | SIGTERM, SIGINT, SIGHUP | Signals that stop the server cleanly. |
| `LOG_PREFIX` | `"rtspcam: "` | Prefix on every log line. |
| `LIGHT_MODE_FILE` | `"/run/rtspcam.mode"` | rtspcam saves `night` or `day` here so the mode survives a restart; missing means day. |
| `NIGHT_MODE_TEXT` / `DAY_MODE_TEXT` | `"night"` / `"day"` | Mode words used in the file and the HTTP API. |
| `CONTROL_LISTEN_ADDRESS` | `"127.0.0.1"` | Control server bind address: this Pi only. |
| `CONTROL_PATH` / `CONTROL_STATE_PARAMETER` | `"/mode"` / `"state"` | The control URL and its query parameter. |
| `CONTROL_WORKER_THREADS` | 1 | Worker threads for control requests (one request at a time). |
| `CONTROL_TIMEOUT_SECONDS` | 2 | Read/write timeout per control connection. |
| `CONTROL_MAX_REQUEST_BYTES` | 1024 | Largest accepted request head. |
| `CAMERA_ELEMENT_NAME` | `"camera"` | Name of libcamerasrc in the pipeline. |
| `LIGHT_MODE_CONTROLS` | table | ISP controls changed at night, with their day (neutral) and night values. |

## Rotation

`-r 90` (or `-r 270`) marks the stream as portrait without touching any pixels: every frame carries an H.264 display orientation SEI message telling the player how to turn it. The camera is mounted for portrait viewing, so the service runs with a quarter-turn rotation.

The pixels stay landscape because nothing on this hardware can turn an image 90° for free: the sensor only flips, and neither the ISP nor the encoder can transpose. Rotating on the CPU would cost a copy of every frame.

How the tag is built and inserted:

1. At startup, `buildOrientationSeiNal()` (via `createOrientationSeiMemory()`) builds one Annex B NAL unit (about 8 bytes): start code `00 00 00 01`, NAL header `0x06`, payload type 47 (display orientation, H.264 Annex D.1.27), payload size, payload, stop bits `0x80`. Emulation prevention bytes are inserted by `appendWithEmulationPrevention()`.
2. Payload fields: cancel flag 0, hor\_flip 0, ver\_flip 0, `anticlockwise_rotation` as 16 bits in 1/65536 turns, repetition period `ue(0)` (this frame only), extension flag 0, then SEI alignment. Clockwise 90° is written as anticlockwise 270° = 49152; clockwise 270° as 16384. Resulting bytes for `-r 90`: `00 00 00 01 06 2F 03 18 00 14 80`; for `-r 270`: `00 00 00 01 06 2F 03 08 00 14 80`.
3. The SEI lives in one read-only, reference-counted `GstMemory`. The media factory's signal, each pipeline's probe and each frame in flight hold their own reference, so the bytes stay valid until the last user is gone, including pipelines still shutting down after `main()` returns.
4. `onMediaConfigure()` installs a buffer probe (`onEncodedFrame()`) on the payloader's sink pad. For every encoded frame it builds a new buffer: a reference to the shared SEI memory followed by the encoder buffer's memories (shared, not copied). Nothing is allocated per frame except the small buffer header. The encoder's own buffer is never modified, so it returns intact to the V4L2 buffer pool.

Because the repetition period is "this frame only", the SEI must be on every frame, which it is. Cost: about 8 bytes and one extra small RTP packet per frame.

**Who honours it:** FFmpeg's H.264 decoder exports it as a display matrix (rotation −90 for `-r 90`), so mpv, ffplay and FFmpeg-based tools show the picture in portrait. VLC and browsers ignore it. The Automate API relay reads the SEI itself and passes the rotation to the browser (see Consumers).

## Night mode

Without the IR-cut filter the IR LEDs light all three colour channels, and the tuning file's colour matrix (made for visible light) turns that into a strong magenta cast with green crushed and blue clipped. In night mode rtspcam sets these libcamera controls on libcamerasrc:

| Property | Day (neutral) | Night | Why |
| --- | --- | --- | --- |
| `saturation` | 1.0 | 0.0 | Greyscale; removes the cast |
| `contrast` | 1.0 | 1.25 | IR light flattens tones; stretch around mid-grey |
| `brightness` | 0.0 | -0.05 | Keeps bright walls from clipping after the stretch |
| `sharpness` | 1.0 | 0.5 | Less sharpening of high-gain sensor noise |

The values are applied by the ISP on the next capture request: no per-frame CPU work, no extra buffers, no pipeline restart, and the stream format is unchanged, so consumers need nothing new. Greyscale frames also leave the chroma planes flat, so the encoder spends almost no bits on them and the bitrate goes to luma detail.

Control API (HTTP/1.1, plain text, loopback only, `Connection: close`):

| Request | Reply |
| --- | --- |
| `GET /mode` | `200`, body `night` or `day` |
| `GET /mode?state=night` | `200`, switches to night, body `night` |
| `GET /mode?state=day` | `200`, switches to day, body `day` |
| Other `state` value | `400` with usage text |
| Other path | `404` |
| Other method | `405` |

How the switch works:

1. `ldrdetect` runs `ldrcmd.sh ir_on` or `ir_off`, which calls `curl "127.0.0.1:8556/mode?state=night"` (or `day`) with the same retry options as the GPIO call.
2. `ir_on` switches rtspcam to greyscale before lifting the IR-cut filter; `ir_off` restores the filter first, then colour, so the overlap shows grey rather than magenta.
3. The control server is a plain `GSocketService` that accepts on the main loop and queues each connection to a one-thread `GThreadPool` (`onControlConnectionAccepted()` → `onControlJob()`), so a slow or stalled client can never block the RTSP main loop. `handleControlConnection()` reads the request head (with a 2 s timeout), answers and closes. `GThreadedSocketService` is deliberately not used: under concurrent requests it crashed the process (reproduced in a stress test; the crash is in GLib's accept path).
4. `LightModeController::requestMode()` does nothing if the mode is unchanged; otherwise it logs `light mode: ...`, saves the mode to `/run/rtspcam.mode` and marks it pending. On the next frame `onCameraFrame()` applies it (see below) and logs `light mode: ... applied`. No process or pipeline restart is involved.
5. `trackCamera()`, called from `onMediaConfigure()`, adds the camera probe to the new pipeline's camera, and `LightModeController::prepareNewCamera()` applies night values before it starts. In day mode nothing is set, so the tuning file's own processing is used untouched.
6. At startup rtspcam reads `/run/rtspcam.mode`, so the mode survives an rtspcam restart (not a reboot; `ldrdetect` sends the current mode when it starts).

### Why the switch happens inside a camera probe

libcamerasrc can silently lose a control change. When it builds a capture request it first moves all pending controls into it, and only then tries to take a free buffer; if none is free it throws the request away, controls included. Set from another thread, a change therefore landed or vanished depending on its timing (about half were lost).

rtspcam makes every switch land first time:

1. The HTTP handler only records the new mode and sets `isApplyPending`.
2. `onCameraFrame()`, a buffer probe on libcamerasrc's output, runs on libcamerasrc's own streaming thread. When a switch is pending it sets the controls and returns `GST_PAD_PROBE_DROP` for that one frame.
3. The dropped buffer goes straight back to libcamerasrc's pool, and the next request is built on the same thread afterwards, so it is sure to find a buffer and carries the new controls to the camera.

Setting the controls on the streaming thread also means they can never be changed while libcamerasrc is building a request. The cost is one skipped frame per switch (66 ms at 15 fps); the encoder and every consumer simply see a frame missing. With nothing pending the probe costs one atomic load per frame.

`LightModeController` owns the mode and is the only code that changes the camera's controls. Its mutex makes "check pending, apply, clear pending" one step across the control worker thread, the camera probe (camera streaming thread), media-configure (RTSP client thread) and the main thread, so a request arriving mid-switch is never lost or applied twice. Its atomic pending flag lets the probe skip all work with one load. `checkLightModeControls()` confirms at startup that libcamerasrc has every property in `LIGHT_MODE_CONTROLS`.

To try other values before changing the table, stop the service and compare stills: `rpicam-still -n --saturation 0 --contrast 1.25 --brightness -0.05 --sharpness 0.5 -o night.jpg`.

## Runtime behaviour

The camera runs only while someone is watching: the pipeline is built when the first client sets up a session and torn down, releasing the camera, when the last one leaves.

- **Shared media:** the factory is `shared=TRUE`, so every client reuses the same capture and encode. A second viewer costs only network output.
- **Session reaping:** a GLib timer runs `gst_rtsp_session_pool_cleanup()` every 10 s, so a client that vanished without TEARDOWN (crash, Wi-Fi drop) times out and stops holding the camera. Sessions expire after gst-rtsp-server's default 60 s timeout.
- **Transports:** both RTP over TCP (interleaved) and UDP are accepted. All known consumers use TCP.
- **Startup:** stdout is line-buffered for journald; options are parsed; the required elements (`libcamerasrc` with `sensor-config`, `v4l2h264enc`, `rtph264pay`) are checked so a missing or old package fails immediately; the server socket is bound with `gst_rtsp_server_create_source()`, which reports bind errors such as "Address already in use".
- **Shutdown:** SIGTERM, SIGINT or SIGHUP quits the main loop and frees everything. Each handler removes itself, so a second identical signal kills the process immediately.

Log lines (journal of `rpi-rtspcam.service`), all prefixed ` rtspcam:  `:

| Line | Meaning |
| --- | --- |
| `1280x720@15 2000000 bit/s, keyframe every 15, sensor 1920x1080:10, rotation 90` | Settings at startup |
| `   rtsp://192.168.x.x:8554/cam ` | One reachable URL per IPv4 interface that is up |
| `client 192.168.1.14 connected` / `disconnected` | Client connection events |
| `camera streaming` / `camera released` | Media prepared (camera on) / unprepared (camera off) |
| `light mode: day (no correction), control http://127.0.0.1:8556/mode` | Mode and control URL at startup |
| `light mode: night (greyscale)` / `light mode: day (no correction)` | Each mode change from the control API |
| `light mode: night applied` / `light mode: day applied` | The camera probe sent the controls (one frame skipped) |
| `error from <element>: <message>` + debug line | Any pipeline error, logged by a bus sync handler |

Pipeline errors are caught by `onBusSyncMessage()`, a sync handler on the media pipeline's bus. It only logs and passes every message on, so gst-rtsp-server's own bus watch still handles the error (the client gets a failure, the media is unprepared).

## Code structure

`rtspcam.cpp` is one file, grouped top to bottom in the order below. `main()` reads as parse → check → read saved light mode → build rotation tag → create server → bind → start control server → run → clean up.

| Section | Main pieces | Role |
| --- | --- | --- |
| Fixed settings | `LISTEN_ADDRESS`, `SESSION_CLEANUP_SECONDS`, option limits, SEI constants, light mode file, control server settings | Every fixed value, named |
| Configuration | `Config`, `NumericOption`, `NUMERIC_OPTIONS`, `RequiredElement`, `REQUIRED_ELEMENTS`, `LightModeControl`, `LIGHT_MODE_CONTROLS` | Option defaults and the tables that drive parsing, checks and night mode |
| Logging | `logLine()`, `logInfo()`, `logError()`, `errorMessageOrFallback()` | Prefixed printf-style lines; errors to stderr |
| Argument parsing | `parseArguments()`, `parseOptionValue()`, `validateCombinedOptions()`, `parseRangedLong()`, `parseSensorMode()`, `findNumericOption()`, `isValidMountPath()`, `isSupportedRotation()`, `isInRange()`, `isEven()`, `printUsage()` | Table-driven validation of every option |
| Startup checks | `checkRequiredElements()`, `checkLightModeControls()`, `elementHasProperty()` | Confirms plugins, the `sensor-config` property and the night mode controls exist |
| Pipeline element helpers | `findElementInBin()`, `addBufferProbe()` | Shared by the orientation tagger and the camera probe |
| Display orientation | `BitWriter`, `appendWithEmulationPrevention()`, `clockwiseDegreesToRotationUnits()`, `buildOrientationSeiNal()`, `createOrientationSeiMemory()`, `releaseSeiMemory()`, `onEncodedFrame()`, `attachOrientationTagger()` | Builds the SEI once and inserts it per frame |
| Night mode | `lightModeText()`, `lightModeDescription()`, `readNightModeFromFile()`, `saveLightModeFile()`, `controlValueForMode()`, `applyLightModeControls()`, `LightModeController`, `onCameraFrame()`, `trackCamera()` | Day/night picture switching |
| Control HTTP server | `HttpStatus`, `readRequestHead()`, `hasCompleteRequestHead()`, `handleRequestLine()`, `handleRequestTarget()`, `handleModeQuery()`, `parseRequestedState()`, `buildReplyBody()`, `sendHttpReply()`, `ControlServer`, `onControlConnectionAccepted()`, `onControlJob()`, `handleControlConnection()`, `createControlServer()`, `destroyControlServer()` | Loopback API used by `ldrcmd.sh` |
| Pipeline | `buildPipelineDescription()` | The launch string |
| Runtime callbacks | `onBusSyncMessage()`, `attachBusErrorLogger()`, `onMediaConfigure()`, `onMediaStateChanged()`, `onClientConnected()`, `copyClientAddress()`, `onClientClosed()`, `freeClientAddress()`, `onSessionCleanup()`, `onShutdownSignal()` | Logging, rotation hook-up, session reaping, shutdown |
| Server | `ServerResources`, `createRtspServer()`, `startRtspServer()`, `releaseServerResources()`, `logStartupSummary()`, `logStreamUrls()`, `isUsableIpv4Interface()`, `logStreamUrl()`, `removeSourceIfPresent()` | Server, shared factory, mount point, cleanup, URL printout |

Conventions to keep when editing:

- Descriptive camelCase names for variables and functions; `UPPER_CASE` for constants; no magic numbers; a comment above every function, struct and class.
- Verbose over compact: no ternary operators, no assignments inside conditions, and calculations split into named intermediate variables (see `clockwiseDegreesToRotationUnits()` or `readRequestHead()`).
- Threads: only `LightModeController` touches the light mode or the camera's controls, and while streaming the controls are set only from the camera probe.
- DRY: numeric options and required elements are tables, logging goes through one helper, signals are an array, probes are added through `addBufferProbe()`, and every exit path releases resources through `releaseServerResources()`.
- Add a numeric option by adding a `Config` field, a `NUMERIC_OPTIONS` row (with named limits), its letter in `OPTION_LETTERS`, and the usage text in the header comment and `printUsage()`.
- Compile warning-free with `-Wall -Wextra`.

## Testing

Changes are checked with a regression harness that runs the real, unmodified binary in a container without a camera. Stand-in `libcamerasrc` and `v4l2h264enc` elements (test pattern + colour balance, and x264) expose the same properties. The suite compares two builds on:

- 34 invalid command-line cases (message, usage line, exit code);
- every control API case: valid and invalid states, wrong paths and methods, malformed, oversized and stalled requests, 100 sequential and 20 parallel requests, LAN access refused, port conflicts;
- restoring the saved mode after a restart;
- live streaming at 0°, 90° and 270°: greyscale switching seen in the decoded frames, exactly one dropped frame per switch, the controls reaching the camera on its streaming thread, the SEI bytes, and the rotation ffprobe reports;
- switching with no viewer, then connecting.

A stress test sends 240 random parallel night/day requests while streaming and checks the camera ends in the last requested mode. AddressSanitizer/UBSan builds are run through the stress and shutdown-while-streaming tests. The stand-ins can't reproduce libcamerasrc's real buffer-pool behaviour, so after a change also run the 20-switch check on the Pi.

## Build and dependencies

Build on the Pi with:

```
g++ -O2 -s -o rtspcam rtspcam.cpp $(pkg-config --cflags --libs gstreamer-rtsp-server-1.0 gstreamer-rtsp-1.0 gstreamer-1.0)
```

`gstreamer-rtsp-1.0` must be listed: `gst_rtsp_connection_get_ip()` (client IP logging) lives in `libgstrtsp-1.0`, and the link fails with "DSO missing from command line" without it. Builds go through the user's compile script into `/opt/rpi/`.

| Package | Provides |
| --- | --- |
| `libgstrtspserver-1.0-dev` | gst-rtsp-server headers and library |
| `libgstreamer-plugins-base1.0-dev` | `gstreamer-rtsp-1.0` (pulled in by the above) |
| `gstreamer1.0-libcamera` (0.7.1+rpt20260429 or newer) | `libcamerasrc` with `sensor-config` |
| `gstreamer1.0-plugins-good` | `v4l2h264enc`, `rtph264pay` |

Other headers used: `glib-unix.h`, `ifaddrs.h`, `arpa/inet.h`, `<vector>`, `<cstdarg>`. No other libraries are linked.

## Deployment

rtspcam runs as `/etc/systemd/system/rpi-rtspcam.service`:

```
[Unit]
Description=IMX462 camera -> hardware H.264 -> RTSP on port 8554 (/cam)
After=network.target
Before=rpi-motion.service

[Service]
# Wait for the Pi's ISP driver so libcamera finds the camera at boot
ExecStartPre=/bin/sh -c 'for i in $(seq 30); do grep -qs bcm2835-isp /sys/class/video4linux/*/name && exit 0; sleep 1; done; exit 1'
ExecStart=/opt/rpi/rtspcam -w 1280 -h 720 -f 15 -b 2000000 -i 15 -s 1920x1080:10 -r 90
Restart=always
RestartSec=5
```

- **ISP wait:** `ExecStartPre` polls up to 30 s for the `bcm2835-isp` V4L2 device; without it, libcamera can start before the ISP driver at boot and find no camera.
- **Ordering:** it starts before the motion recorder, which connects to it. `After=network.target` is enough because the server binds `0.0.0.0` and does not need an address to start.
- **Activation:** the unit shows as `static` (no enabled `[Install]` section), so it starts only when another unit pulls it in. If it must start on its own at boot, add `[Install] WantedBy=multi-user.target` and `systemctl enable` it.
- **After a rebuild or unit change:** `sudo systemctl daemon-reload && sudo systemctl restart rpi-rtspcam`.
- **Logs:** `journalctl -u rpi-rtspcam -n 50 --no-pager`.

The root filesystem is read-only (overlayfs), so binary, unit and `config.txt` changes must be made with the overlay disabled to persist.

Open question: confirm which unit pulls `rpi-rtspcam.service` in at boot, and whether the service runs as root or a dedicated user in the `video` group.

## Consumers and interfaces

rtspcam's only interface is the RTSP stream `rtsp://doorway.home:8554/cam` (also `rtsp://127.0.0.1:8554/cam` on the Pi): one H.264 High-profile video track, 1280x720 at 15 fps, a keyframe every second, SPS/PPS before every keyframe, and the orientation SEI on every frame. Clients should use RTP over TCP.

&#91;embedded content: rtspcam and its consumers\]

Every consumer shares the same encode, so adding viewers costs the Pi only network output.

| Consumer | Where | What it relies on from rtspcam |
| --- | --- | --- |
| Motion recorder (`rpi-motion.service`) | Doorway Pi | RTSP over TCP; starts after rtspcam |
| Automate API relay (`camstream.js` + `fmp4.js`) | Automate server, route `/api/camera/doorway` | 1-second GOPs (one MP4 fragment per keyframe); the display orientation SEI to detect rotation; TCP |
| Browser camera window (`camera.js`) | Any browser via the Automate site | Rotation from the relay's `X-Camera-Rotation` header; codec from `X-Camera-Codec` |
| mpv, ffplay, ffmpeg | Any LAN machine | Honour the SEI rotation |

Changing the keyframe interval, profile, or dropping the SEI affects the relay: it expects about one keyframe per second and reads the rotation from the first keyframe's SEI.

## Constraints and troubleshooting

Hard limits of this hardware: no 90° rotation of pixels, no zero-copy dmabuf import into the encoder, encoder sizes up to 1920 px per side at level 4, and 512 MB RAM shared with the CMA pool.

| Symptom | Cause | Check or fix |
| --- | --- | --- |
| `error from v4l2h264enc0: Failed to process frame` on first frame | Encoder input path rejected the camera buffers | Keep the default copy mode; never add `output-io-mode=dmabuf-import` |
| Lockups, "not enough memory" | CMA pool exhausted | `grep -i cma /proc/meminfo`; `dtoverlay=cma,cma-128` in `config.txt` |
| No camera at boot | ISP driver not ready | The `ExecStartPre` wait; `ls /sys/class/video4linux/*/name` |
| `'libcamerasrc' lacks the 'sensor-config' property` | gstreamer1.0-libcamera too old | Upgrade to 0.7.1+rpt20260429 or newer |
| `cannot listen on 0.0.0.0:8554: Address already in use` | Another instance or old relay still running | `sudo ss -ltnp \| grep 8554` |
| `Device or resource busy` | Another program holds the camera | Stop any `rpicam-*` or libcamera process |
| `'libcamerasrc' lacks the 'saturation' control` | gstreamer1.0-libcamera without control properties | Upgrade; `gst-inspect-1.0 libcamerasrc \| grep -i saturation` |
| `light mode: night` logged but never `applied` | No frames flowing (no client, or the pipeline stalled) | Expected with no viewer: the next pipeline starts in the right mode |
| Stream stays magenta at night | Mode change not reaching rtspcam | `curl 127.0.0.1:8556/mode` should say `night`; journal should show `light mode: night`; force it with `curl "127.0.0.1:8556/mode?state=night"` |
| `cannot listen on 127.0.0.1:8556 (control)` | Port in use | `sudo ss -ltnp \| grep 8556`; pick another with `-c` and update `RTSPCAM_CONTROL_PORT` in `ldrcmd.sh` |
| Clients connect, then `camera released` immediately | Client-side failure (e.g. VLC without its live555 RTSP module) | Test locally: `ffprobe -rtsp_transport tcp rtsp://127.0.0.1:8554/cam` |

Useful checks:

- **Stream health:** `ffmpeg -rtsp_transport tcp -i rtsp://doorway.home:8554/cam -t 10 -c copy -f null -` should report about 15 fps and about 1.3–2 Mbit/s.
- **Listening socket:** `sudo ss -ltnp | grep 8554` should show `0.0.0.0:8554`.
- **Verbose GStreamer logs:** stop the service, then run `GST_DEBUG=rtspclient:4,rtspmedia:4 /opt/rpi/rtspcam` by hand.
- **Viewing:** `mpv --rtsp-transport=tcp rtsp://doorway.home:8554/cam` (shows rotation); the Ubuntu apt build of VLC has no RTSP support.
- **Wi-Fi:** `iw dev wlan0 get power_save` should report off; power saving makes delivery bursty.
