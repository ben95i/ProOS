# Plan: replace the voice-switched gate with SpeexDSP echo cancellation

Status (2026-10-19): steps 2 and 4 are in the code (reference tap, SpeexDSP in the mic task, `ECHO_MODE` switch, `DIAG aec` line), waiting for simulator and board results; memory comes from the heap for now (the A2DP-buffer arena is deferred until the heap readings show it is needed). Earlier: steps 0 and 0b measured on the board. Sizing settled: fixed-point SpeexDSP, 120 ms tail, memory from the A2DP buffer plus ~20 KB of heap. Step 1 done in code (SpeexDSP vendored, built, not called; host check `make -C sim speexcheck`); next, its build on the board and the check's figures, then step 2. Target board: Wemos LOLIN32 (ESP32-D0WDQ6, 2 × 240 MHz, 520 KB SRAM, no PSRAM, 4 MB flash).

## Goal

The person on the other phone must not hear their own voice back, with no tuning step, in every call. The gate fails because every one of its thresholds is an absolute level, and the levels change from call to call and during a call: the caller's loudness, CVSD against mSBC, and above all the head unit's volume knob, which sits after the DAC and changes the speaker-to-mic coupling every time it is turned. An adaptive echo canceller normalises by the reference signal's own power and re-learns the echo path continuously, so none of those changes need a setting.

## Why SpeexDSP

| Option | RAM | CPU (core 1, 16 kHz) | Fit |
| --- | --- | --- | --- |
| **SpeexDSP 1.2.1: MDF echo canceller + preprocessor** | ~80 KB (below) | est. 15–35% | Open source (BSD), float C, any frame size, full-duplex, built-in residual echo and noise suppression with defaults that need no tuning |
| WebRTC AECM | ~30–40 KB | est. 5–15% | Lighter, fixed point, own delay estimator; suppression-style, more pumping in double talk, 10 ms frames (needs a FIFO), no longer developed upstream |
| ESP-SR AEC (Espressif) | 114 KB for AEC alone, plus the AFE layer | ~11% | Closed library, 32 ms frames (adds latency); support on the original ESP32 is unclear, and without PSRAM it does not fit beside Bluedroid |
| Hand-written NLMS | small | 1600 taps at 16 kHz is ~90 MHz | Too slow for a car-length echo tail without frequency-domain blocks, which is what Speex already is |

AECM stays the fallback if step 0 shows the RAM is not there.

## How it will work

```
phone ── SCO ──► bluetoothHandsFreeReceiveCallback ──► I2S0 TX DMA ──► DAC ─► head unit ─► speakers
                                                           │ on_sent ISR: copy what was just played
                                                           ▼
                                                   reference ring (int16)
                                                           │
mic ─► I2S1 RX DMA ─► microphoneTask: HPF/LPF ─► int16 ─► speex_echo_cancellation(mic, ref)
                                                           ─► speex_preprocess_run (residual echo + noise)
                                                           ─► MICROPHONE_GAIN, clamp ─► microphoneStreamBuffer ─► phone
```

### The reference is the key part

The canceller needs the exact samples that left the DAC, aligned with the mic. Taking them from the BT receive callback (as the gate does) is not good enough: the TX DMA queue in between varies, and underruns play zeros the callback never sees.

- **Tap the DMA, not the callback.** Register `i2s_channel_register_event_callback()` with an `on_sent` handler on the TX channel, inside `createTransmitChannel()` (it must be registered before `i2s_channel_enable()`, and the channel is rebuilt on every profile change). `on_sent` reports the descriptor that just finished playing (`dma_buf`, `size`). With `auto_clear_after_cb` (what `auto_clear = true` already sets) the buffer is cleared after the callback returns, so the callback sees what was really played, including the zeros of an underrun. Verify on the board that the reference carries audio, since the IDF docs are ambiguous here.
- **ISR work is a copy only.** `IRAM_ATTR`, no float: copy the right channel (call audio) of the 60-frame descriptor into a single-producer / single-consumer `int16_t` ring (1024 samples, 2 KB, in DRAM), and only while the call profile is active.
- **No clock drift.** TX (I2S0) and RX (I2S1) are both masters on the same PLL at the same rate, so the offset between reference and mic is fixed for the life of the channels. The BT-to-I2S rate mismatch happens before the DAC and never reaches the canceller.
- **Sync point.** When the mic task reads its first frame of a call, it drops everything in the ring so its read position equals the write position. The reference then leads the mic by at most one frame, and the echo path the filter must cover is just DAC → head unit → speaker → air → mic, plus the cabin's reverberation.
- **Keep alignment.** Register `on_recv_q_ovf` on the RX channel; an RX overflow drops mic samples, so drop the same number from the ring and count it. A ring that runs short is padded with zeros and counted. Both counts go in the log.
- **Codec change mid-call or channel rebuild:** recreate the canceller and sync again (it re-converges in 1–3 s; the preprocessor covers that time).

### Processing order in `microphoneTask`

1. Read 120 samples (7.5 ms at 16 kHz; 60 at 8 kHz).
2. High-pass 200 Hz and low-pass 7 kHz as now (linear, harmless to the canceller), converted to `int16` **without** `MICROPHONE_GAIN`, so clipping cannot make the echo non-linear.
3. Take the next 120 reference samples from the ring.
4. `speex_echo_cancellation(echoState, mic, ref, out)`.
5. `speex_preprocess_run(preprocessState, out)`, linked to the echo state, with Speex's defaults: residual echo −40 dB (−15 dB while the near end talks), noise suppression −15 dB, AGC, VAD and dereverb off.
6. `MICROPHONE_GAIN` and clamp, then the existing backlog trim and `xStreamBufferSend()`.

The frame stays 120 samples, so no extra latency is added. The canceller runs in the mic task on core 1 (Bluedroid and the controller own core 0), never in a Bluedroid callback. The BT callback only records the requested rate; the mic task creates, recreates and destroys the Speex states itself, so nothing frees them while they are in use.

### What goes away

The calibration assistant, the gate's tuning log and `GATE-TUNING-GUIDE.md` are already gone (2026-10-19). The gate itself (`AEC_*` block, both passes, `farEndPeakLevel`, its coefficients in `calculateFilterAlphas()`) goes once the canceller has passed in the car. Until then a compile switch selects `ECHO_MODE_NONE` / `ECHO_MODE_GATE` / `ECHO_MODE_SPEEX` for A/B tests. `AEC_SPEAKER_ATTENUATION_DB` stays as an optional speaker trim.

## Measured on the board (step 0, one 33 s mSBC call, 2026-10-19)

| Reading | Value | Meaning |
| --- | --- | --- |
| Heap after init (A2DP not yet connected) | 48 KB free, 46 KB largest block | The 40 KB A2DP ring buffer is already taken |
| Heap at the moment call audio connects | 43 KB free, 36 KB largest | A short dip while SCO is set up |
| Heap during the call | **64 KB free, 36 KB largest block** | Steady for the whole call |
| Lowest heap since boot | **29 KB** | While the phone was connecting (A2DP + HFP) |
| Mic task per frame (gate) | 47 µs average, 76 µs max, of 7500 µs | 0.6% of the frame |
| Core 0 idle | 59% | Bluedroid (`BTU_TASK` 21%, mSBC coding), controller 12%, HCI 6% |
| Core 1 idle | **98%** | |
| Mic task stack unused | 4372 of 6144 B | |

**CPU is not a constraint:** even the pessimistic 35% for float Speex leaves core 1 over 60% idle. **RAM is the constraint.** The float build with a 120 ms tail needs ~88 KB; the call leaves 64 KB, and Bluedroid must keep a working reserve (it dropped to 29 KB free while connecting). Not tested yet: a CVSD (8 kHz) call, which needs about half.

## Measured again after step 0b (three mSBC calls, 2026-10-19)

| Reading | Step 0 | Step 0b | Change |
| --- | --- | --- | --- |
| Heap after init | 48 KB free, 46 KB largest | **54 KB free, 52 KB largest** | +6 KB from releasing the BLE controller RAM |
| Heap during a call | 64 KB free, 36 KB largest | **68 KB free, 34 KB largest** | +4 KB |
| Lowest heap since boot | 29 KB | **30 KB** | Still while the phone connects, before any call |
| Mic task per frame (gate) | 47 µs | 63–66 µs, max 91 µs | Under 1.2% of the 7500 µs frame |
| Core 0 / core 1 idle | 59% / 98% | 62% / 97–98% | |

Unused stack, in bytes (allocated in brackets where it is ours to set): `BTU_TASK` 5732 (8092), `microphoneTask` 4380 (6144), `esp_timer` 3564, `BTC_TASK` 2504 (4096), `audioOutputTask` 2408 (4096), `btController` 2124, `Tmr Svc` 1680, `touchPanelTask` 1664 (2048), `hciT` 1604, `diagnosticsTask` 1296 (4096), idle tasks ~1.1 K, `ipc0` 388. These figures include music playback and reconnects, but not first-time pairing. Nothing is close to overflowing; `BTU_TASK` could give ~2 KB back by going to 6144.

After the memory savings (unused Bluetooth profiles off, smaller stacks and buffers; three mSBC calls, 2026-10-19): heap after init **74 KB** (72 KB largest block), during a call **90 KB free, 64 KB largest**, lowest since boot **55 KB**. Every trimmed stack keeps over 1 KB spare (`touchPanelTask` 1128, `Tmr Svc` 1176, `BTC_TASK` 1348, `audioOutputTask` 1384, `esp_timer` 2028, `BTU_TASK` 4348). With ~60 KB for SpeexDSP, a call would keep ~30 KB free taking it all from the heap, or ~70 KB reusing the A2DP buffer as planned.

**SpeexDSP check on the PC (`make -C sim speexcheck`, 2026-10-19), fixed point, 20 s of synthetic white-noise echo:**

| Configuration | Echo canceller | Preprocessor | Total | ERLE, canceller alone | ERLE, with residual suppression | Leaks |
| --- | --- | --- | --- | --- | --- | --- |
| 16 kHz, 120 ms tail | 43.6 KB | 13.9 KB | **57.5 KB** | 37.0 dB | 67.6 dB | none |
| 16 kHz, 90 ms tail | 36.1 KB | 13.9 KB | 50.0 KB | 37.7 dB | 67.8 dB | none |
| 8 kHz, 120 ms tail | 22.3 KB | 7.8 KB | 30.1 KB | 35.0 dB | 61.9 dB | none |

The sizes match the estimates (~58 / ~51 / ~30 KB). The echo here is linear and noise-like, an easy case; real speech in a car will give less, which steps 3 and 6 measure. Host time was 43 µs per frame at 16 kHz on the PC; the ESP32 will be many times slower, and step 6 measures it.

**The gate in the simulator, as a baseline:** with only the far end talking, the uplink is 3.6–4.4 dB *louder* than the echo at the mic (scenarios 1–5 and 10). `MICROPHONE_GAIN` adds 6 dB, so the gate removes only about 2 dB of echo. Its tuned threshold (`AEC_ECHO_COUPLING_DB` −20) expects a much weaker echo than this model produces, so it takes the echo for the driver talking and stays open. That is the same fixed-threshold failure seen in the car.

**First simulator run with SpeexDSP in the firmware (2026-10-19).** "Mic-referred suppression" while only the far end talks includes the +6 dB `MICROPHONE_GAIN`, so the echo actually removed is about 6 dB more than shown:

| Scenario | Gate | SpeexDSP | Note |
| --- | --- | --- | --- |
| 1 · 2: 180 s call, ±60 ppm | −3.8 dB | **18.7 / 19.0 dB** | ~25 dB of echo removed |
| 10: 60 s, far end only | −3.8 dB | **16.5 dB** | |
| 3: 3.5 s calls between music | −3.6 dB | 8.5 dB | Each call is mostly convergence time |
| 4: codec swap every 4 s | −3.6 dB | 7.2 dB | Re-converges after each swap |
| 5: 2.5 s calls, abrupt loss | −4.4 dB | 2.3 dB | As 3, shorter still |

No new underruns, overruns or warnings, and `chans=2`; scenario 9 still delivers 100% to the DAC. New in scenarios 1–4: 1–3 "silent 200 ms windows while talking" per run (`deadRun=200ms`), where none occurred before. The likeliest cause is the residual suppressor muting the start of near-end speech during double talk. Step 3's near-end-loss metric measures it, and it is the thing to listen for in the car. The 8 kHz check with the firmware's 120-sample frames needs 42.5 KB.

Not measured yet: a CVSD (8 kHz) call. It needs about half the 16 kHz memory and CPU, so it cannot be the limiting case; it is tested with the canceller in step 6.

## Memory plan (revised from the readings)

### Sizes

N is twice the frame (240 at 16 kHz), M the number of filter blocks; sizes from the allocations in `mdf.c` and `preprocess.c`.

| 16 kHz, frame 120 | Float, M = 16 (120 ms) | Float, M = 12 (90 ms) | **Fixed point, M = 12 (90 ms)** | Fixed point, M = 16 (120 ms) |
| --- | --- | --- | --- | --- |
| Echo canceller | ~62 KB | ~51 KB | **~37 KB** | ~44 KB |
| Preprocessor | ~18 KB | ~18 KB | **~14 KB** | ~14 KB |
| Reference ring (1024 samples, 64 ms) | 2 KB | 2 KB | **2 KB** | 2 KB |
| **Total** | ~82 KB | ~71 KB | **~53 KB** | ~60 KB |

8 kHz needs roughly half of each. The ring shrinks from 8 KB to 2 KB: the sync point keeps it within a frame or two, so 64 ms is ample.

### Decision

1. **Fixed-point build** (`FIXED_POINT`, `USE_KISS_FFT`). SpeexDSP was written for fixed-point DSPs and its fixed-point echo canceller is its most-used form. Arrays become 16-bit, and the CPU it needs is no issue with core 1 98% idle. The float build stays a compile option for an A/B comparison if memory allows (see 3).
2. **Reuse the A2DP ring buffer's 40 KB during calls.** It sits idle while a call is up. It becomes a static 40 KB arena: in music mode the ring buffer is built on it with `xRingbufferCreateStatic()`; when call audio connects, `audioOutputTask` confirms it holds no item (a flag it sets when parked), the ring buffer is deleted, and Speex is allocated from the arena by a bump allocator through the `OVERRIDE_SPEEX_ALLOC` hook; at call end Speex is destroyed and the ring buffer rebuilt on the same arena. A static arena is essential: freeing and re-allocating the ring buffer from the heap would need a 40 KB contiguous block again after the call, and the largest block during a call is only 36 KB, so music could fail to come back.
   - **Chosen after step 0b: fixed point, M = 16 (120 ms tail).** ~60 KB = 40 KB arena + ~20 KB from the heap, leaving ~48 KB free during a call, well above the 30 KB low point Bluedroid reaches while connecting. The longer tail covers an unknown head-unit delay; step 6's impulse-response read-out shows whether it can be cut to M = 12 (saves ~7 KB).
   - Float at M = 12 (~71 KB, 31 KB from the heap) would leave ~37 KB, too close to that low point to be the default; it stays a build option for the simulator's A/B comparison. Float at M = 16 does not fit.
   - Speex is created in the mic task after the 80 ms settle delay, not inside the Bluetooth callback, so the heap is past the dip at audio connect. If the allocation fails, the call carries on with the plain mic path and logs it.
3. **Recover controller RAM first (done in step 0b: +6 KB).** `initializeBluetoothStack()` does not call `esp_bt_controller_mem_release(ESP_BT_MODE_BLE)` before `esp_bt_controller_init()`, which Espressif's own Classic-only A2DP examples do. The controller reserves 56 KB (`CONFIG_BTDM_RESERVE_DRAM=0xdb5c`), and releasing the BLE part hands some of it back to the heap. How much is not documented, so the `DIAG heap after init` line measures it. If it frees 20 KB or more, the float build fits too.
4. **Smaller savings, only if needed (not needed for the chosen sizing):** Bluedroid's `CONFIG_BT_ACL_CONNECTIONS` from 4 to 2 (one phone, plus headroom); `CONFIG_BT_BTU_TASK_STACK_SIZE` from 8092 down, if the stack readings (step 0b) show it unused.

`speex_alloc` keeps a live-allocation counter, so a leak shows in the log and in the simulator.

## CPU plan

Measured: core 1 is 98% idle during a call, and the gate's mic processing takes 47 µs of each 7500 µs frame. Speex's ~11 FFTs per frame and its filter updates are estimated at 15–35% of core 1 in float, similar in fixed point. There is room, so no FFT or frame-size optimisation is planned. Step 6 measures the real figure with the same `DIAG mic` line.

The mic task's stack has 4.3 KB unused; Speex's calls use some stack, so the reading after step 4 decides whether 6 KB is still enough. The RX DMA goes from 4 to 6 descriptors (+1 KB) so a slow frame cannot overflow the mic queue.

## Steps

Each step builds and is checked on its own; the gate stays in charge until step 5.

0. **Measure the budget (done; readings above).** `DIAG_LOG_ENABLED` in `main.c`: once per second during a call, the mic task's time per frame (`esp_timer_get_time()` around the processing, average and maximum against the frame period), `heap_caps_get_free_size` / `get_largest_free_block` / `get_minimum_free_size` (8-bit), and the mic task's `uxTaskGetStackHighWaterMark()`; every 10 s each core's idle share and the busiest tasks from `uxTaskGetSystemState()` deltas (`CONFIG_FREERTOS_USE_TRACE_FACILITY`, `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`); the heap at boot and at call start. One call on the board decides the memory choices above.
0b. **Recover RAM and measure again (done; readings above).** Call `esp_bt_controller_mem_release(ESP_BT_MODE_BLE)` before `esp_bt_controller_init()`; add each task's unused stack (`usStackHighWaterMark` from `uxTaskGetSystemState()`) to the `DIAG cpu` line, for the Bluetooth tasks' stacks; set the flash size to the board's real 4 MB (`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`; the boot log warns the image says 2 MB). One mSBC call and one CVSD call (HD voice off on the phone) then settle float against fixed point and the tail length.
1. **(Done in code.) Vendor SpeexDSP 1.2.1** as an IDF component `components/speexdsp/`: `mdf.c`, `preprocess.c`, `filterbank.c`, `fftwrap.c`, `smallft.c` and their headers, `COPYING` (BSD), a hand-written `speexdsp_config_types.h`, `os_support_custom.h` with the allocator hook, and compile definitions `FIXED_POINT USE_KISS_FFT EXPORT= OS_SUPPORT_CUSTOM` (with `kiss_fft.c` and `kiss_fftr.c`; `FLOATING_POINT USE_SMALLFT` and `smallft.c` as the float option). `main/CMakeLists.txt` stays as it is: with no `REQUIRES` list, `main` already gets every component; listing only `speexdsp` would drop the others. It builds but nothing calls it yet.
2. **Reference tap**: `on_sent` and `on_recv_q_ovf` callbacks, the ring, the sync point, and counters in the log. Still the gate. Check in the simulator that the ring holds exactly the samples the simulated DAC played.
3. **Simulator upgrades** (next section), then record a baseline with the gate. It should reproduce the field failure.
4. **Canceller in the mic task** behind `ECHO_MODE_SPEEX`, with states created / recreated / destroyed in the mic task, and its own log line (below).
5. **Simulator pass**: every scenario, old and new, meets the pass criteria below.
6. **Board, bench**: a boot-time self-test in a debug build runs 1000 synthetic frames through both Speex states and prints µs per frame and heap used, before any call. Then calls in the car (next section).
7. **Clean-up**: remove the gate (the calibration assistant, tuning log and guide are already gone); document the canceller in `CLAUDE.md`.

## Simulator changes

- **Build SpeexDSP for the host**: add the same sources to `sim/Makefile` with the same defines; route `speex_alloc` to `calloc` with a counter, and report leaks at the end of each scenario.
- **Mock the new IDF calls** in `stubs/espmock.h` / `.c`: `i2s_event_callbacks_t`, `i2s_event_data_t`, `i2s_channel_register_event_callback()`, `auto_clear_after_cb` / `auto_clear_before_cb` in `i2s_chan_config_t`, `IRAM_ATTR`, `heap_caps_*`, `esp_cpu_get_cycle_count()`, `uxTaskGetStackHighWaterMark()`. `mockI2sTxPop()` calls `on_sent` with the descriptor it played (zeros on an underrun), and `mockI2sRxPush()` calls `on_recv_q_ovf` when it drops one.
- **A realistic car echo.** Today's model is a 140-tap (9 ms) impulse with an 11 ms delay, far easier than a real cabin. Replace it with a bulk delay of 20–80 ms (per scenario), ~64 taps of early reflections, and a reverberant tail from two or three feedback comb filters (60–80 ms decay). That keeps the cost per sample low enough to run at speedup 10–20, unlike a 1600-tap convolution. Keep the mild amplifier non-linearity and add road noise at a realistic level.
- **Level changes**: a head-unit volume step of ±6 dB mid-call, and a different caller loudness on each call. These are the conditions that break the gate.
- **New metrics**:
  - ERLE: echo power at the mic against echo power left in the uplink, over far-end-only stretches, as now, but also over time.
  - Convergence time: from call start, and after a volume step, until ERLE passes 20 dB.
  - Near-end loss in double talk: uplink power against the clean near-end voice's power while both talk (the gate loses ~32 dB here; the target is a few dB).
  - Alignment counters: ring short, ring trimmed, RX overflows.
  - Speex allocations still live at the end.
- **New scenarios**: 11 far-end only with the long tail; 12 continuous double talk; 13 volume steps mid-call; 14 echo-delay sweep 20 / 40 / 60 / 80 ms; 15 caller loudness varied per call. Scenarios 1–5, 9 and 10 still run, and must show no new underruns, overruns or warnings and `chans=2`.
- **Replay recordings**: read paired reference / mic WAV files (from the optional board dump below) through the same processing, so a real car can be tested at the PC.

**Pass criteria (simulator):** ERLE of 25 dB or more within 3 s of far-end speech, back above 20 dB within 2 s of a volume step, near-end loss in double talk of 6 dB or less, every delay in the sweep handled, no leaks, no regression in scenarios 1–10.

## Measuring on the board

**Log line, once per second during a call** (behind a switch, off in normal use):

```
AEC 16k frame 1840/2950 us (25%/39%) erle 31 dB delay 42 ms ring 118 short 0 trim 0 rxovf 0 txund 0 heap 61K/38K
```

- Frame time average / maximum against the 7500 µs budget.
- ERLE estimate (mic power against output power while the far end talks).
- Echo delay: the peak position of `SPEEX_ECHO_GET_IMPULSE_RESPONSE`, read every 10 s rather than every second (it costs an inverse FFT per block). It shows the real DAC → head unit → mic delay, and whether the 120 ms tail is too short or can be cut.
- Ring level and the alignment counters, TX underruns (`on_send_q_ovf`), and heap free / minimum.

**In-car test, in order**, engine running, normal volume:

1. Call a second phone held by someone else (or your own voicemail and listen back), far end talking only. They should hear no echo after the first 1–3 s.
2. Turn the head unit volume up and down mid-call. The echo should stay away, or come back for under 2 s.
3. Talk over each other. Both voices should come through, unlike the gate.
4. Short calls, long calls, CVSD and mSBC (turn HD voice off on the phone to force CVSD).
5. A highway-speed call, for road noise.

**Optional audio dump** for hard cases: a debug mode streams the reference and the mic (16-bit, 16 kHz, two channels, 64 KB/s) over the USB serial port at 2 Mbaud (the LOLIN32's CH340 handles it) with logging off; a PC script splits it into WAV files, and the simulator replays them (above). This is how an echo that only happens in the real car gets fixed at the desk.

## Risks

- **Head-unit DSP delay** longer than the tail: the impulse-response read-out shows it; lengthen the tail (costs RAM and CPU).
- **Amplifier distortion** at high volume is non-linear and cannot be cancelled; the preprocessor's residual suppression covers part of it, and `AEC_SPEAKER_ATTENUATION_DB` remains for the rest.
- **`on_sent` buffer contents**: if the board shows the buffer already cleared, set `auto_clear_before_cb = false`, `auto_clear_after_cb = true` explicitly, or keep a copy of each written frame indexed by descriptor.
- **Heap** during a call: decided by step 0 before any Speex code is written.
