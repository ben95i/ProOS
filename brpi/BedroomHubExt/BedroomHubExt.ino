/*
 * -----------------------------------------------------------
 * Bedroom Hub External - companion node for AutomateHub #1
 * Runs on an ATmega328P wired to the hub's external serial port #2. Adds
 * WS2812 strip control, switched-outlet control and bed motor control, using
 * the same framed serial protocol as AutomateHub_1.
 * -----------------------------------------------------------
 *
 * v1 - 07/26/2026 - SoftwareSerial link, hello only
 * v2 - 07/26/2026 - hardware Serial (pins 0/1) link + WS2812 control
 * v3 - 07/26/2026 - back to SoftwareSerial link (RX 10 / TX 11) + WS2812 control
 * v4 - 07/26/2026 - strip lit-status query (11008) + fast effect resume
 * v5 - 07/27/2026 - brightness floor, burst-aware effect resume, trailing
 *                   terminator no longer re-arms the long holdoff
 * v6 - 07/31/2026 - theater-chase and comet effects, per-effect speed
 * v7 - 08/01/2026 - adjustable-chair motor control (non-blocking pulses,
 *                   burst extension, run-time limit + cooldown lockout,
 *                   direction settle time)
 * v8 - 08/01/2026 - motor cumulative duty-cycle limiter (leaky-bucket run
 *                   budget) + both-pins-high hardware interlock guard
 * v9 - 08/01/2026 - motor code simplified: duty bucket alone now enforces
 *                   the 60 s run cap (redundant run timer/clamp removed),
 *                   pin read-back guard dropped (GPIOs drive MOSFET gates
 *                   directly and every start passes through both-pins-low)
 * v10- 08/02/2026 - smooth hue animation: the rainbow/cycle hue now advances
 *                   from elapsed time in 1/4096-hue fixed point instead of a
 *                   fixed jump per frame, so a frame delayed or skipped by
 *                   link traffic no longer changes the animation speed. Frame
 *                   intervals lowered (speed unchanged, see the tuning notes)
 *                   for finer per-frame color steps, catch-up is clamped so a
 *                   long holdoff cannot cause a color jump, gamma is now a
 *                   compile-time option, and every effect re-checks the link
 *                   immediately before strip.show() so the raised frame rate
 *                   does not cost command responsiveness
 * v11- 08/02/2026 - LED/effect code consolidated: one global EFFECT_INTERVAL_MS
 *                   for every effect (per-effect intervals and
 *                   currentEffectInterval() dropped), a per-effect SPEED step
 *                   for all four effects including the two pattern ones, the
 *                   four separate position variables replaced by one shared
 *                   phase accumulator, and the link re-check / strip.show() /
 *                   phase advance hoisted out of the four renderers into
 *                   serviceEffects() so each exists exactly once. static_assert
 *                   guards removed - the valid range of every tuning constant
 *                   is documented instead
 * v12- 08/02/2026 - effect resume command (011007) removed; pause is now
 *                   cleared by re-issuing an effect command, which also
 *                   rewinds that effect to its origin. Command value 7 is
 *                   left unused so the rest of the map is unchanged
 * v13- 08/02/2026 - perceptual brightness: register 16's 0-255 level is now
 *                   mapped through a square-law curve (the standard integer
 *                   approximation of the eye's response) before it is applied
 *                   to the strip, so the level scales evenly in PERCEIVED
 *                   steps instead of bunching all visible change into the
 *                   bottom of the range. The commanded level itself is stored
 *                   and echoed back linearly, so the UI slider is unaffected
 * v14- 08/11/2026 - hub link moved from SoftwareSerial (RX 10 / TX 11) to the
 *                   Uno's hardware USART on pins 0/1; the SoftwareSerial
 *                   library, its port object and its pin defines are gone.
 *                   sendResponse() now ends with Serial.flush() so a reply is
 *                   still completely on the wire before the function returns,
 *                   which is what the reply-before-show ordering relies on:
 *                   HardwareSerial::print() only queues into the TX buffer,
 *                   and unlike SoftwareSerial::flush(), HardwareSerial's
 *                   flush() drains that TX buffer and does NOT touch the
 *                   receive buffer, so no queued command is discarded.
 *                   Nothing else changed - protocol, command map, effects,
 *                   power and motor logic are all untouched
 * v16- 08/11/2026 - fix: no response reached the hub when forwarded through
 *                   AutomateHub's SoftwareSerial ExtSerial2 (direct to the Pi
 *                   worked). The hub is deaf while it bit-bangs the terminator
 *                   byte it sends after each frame's '>', and the hardware
 *                   USART here answers fast enough to land inside that window,
 *                   so the reply's leading '|' was lost and the hub timed out.
 *                   The reply now waits for that terminator (REPLY_GUARD_MS
 *                   cap) before it is sent
 * v21- 08/11/2026 - fix: the fade never ran. Its abort test was linkIsBusy(),
 *                   but the fade is reached from inside decodeMessage(), where
 *                   serialMsgEnd is still set for the command being answered
 *                   and the master's terminator byte is already buffered by
 *                   waitForMasterTerminator() - so it was always true and the
 *                   fade cut to black on step 1, blanking instantly. It now
 *                   samples the pending byte count once and aborts only on
 *                   growth past it, which is what a NEW command looks like.
 *                   Command 011000 also fades a static color out now, not just
 *                   a running effect; color-to-color changes stay instant
 * v20- 08/14/2026 - fade snapshot stored as three bytes per pixel instead of a
 *                   packed 32-bit color, saving one byte per pixel of stack
 *                   (180 rather than 240 at 60 pixels) for the length of a
 *                   fade. It is repacked per pixel per step so scaleColor()
 *                   still does the scaling; no behavior change
 * v19- 08/14/2026 - removed command 011002 (fill all) and its case; staging
 *                   the three color channels is now the only path to a solid
 *                   color, and fillStripWithStagedColor() is reached only from
 *                   there. Command value 2 joins 3 and 10 as a gap that
 *                   answers with the error frame. Tidied alongside it, with no
 *                   change in behavior: fadeStripOut() now reuses scaleColor()
 *                   rather than repeating its math per channel, drops the
 *                   clearStrip() that used to rewrite an already-black strip
 *                   after the last fade step, and returns immediately when the
 *                   strip is not lit instead of writing black over black
 * v18- 08/14/2026 - stopEffect() now fades the strip out over STOP_FADE_MS
 *                   instead of leaving the last frame frozen on the pixels.
 *                   Staging all three color channels (registers 12/13/14)
 *                   fills the strip on its own; command 011002 still works but
 *                   is no longer required. Removed: command 011003 (set one
 *                   pixel), command 011010 (theater chase) and register 15
 *                   (stage pixel index), along with their effect, draw
 *                   function, pattern constants and the staged-color helper
 *                   that only the chase used. Remaining command values are
 *                   UNCHANGED - 3 and 10 are simply gaps now and reply with
 *                   the error frame, so nothing else on the hub side moves
 * v17- 08/14/2026 - color cycle: pixels are now fanned across a small hue
 *                   range (CYCLE_DITHER_SPREAD) so the strip dissolves across
 *                   an output quantization step instead of every pixel
 *                   stepping together, which is what made the orange-to-red
 *                   part of the wheel visibly jump.
 *                   Effect 7 is now a loading bar rather than a comet: a solid
 *                   bar fills the strip over a dimmed background of the
 *                   opposite hue, then restarts from the other end with a
 *                   fresh random color pair. COMET_TAIL_LEN is gone and
 *                   COMET_PIXEL_STEP / COMET_PERIOD are now BAR_PIXEL_STEP /
 *                   BAR_PERIOD; the bar ignores the staged color, so registers
 *                   12-14 now only feed the theater chase
 *
 * Link: hardware UART at 9600 baud - pin 0 = RX (hub ExtSerial2 TX), pin 1 =
 * TX (hub ExtSerial2 RX). On an Uno this port is shared with the USB/bootloader
 * connection, so the hub link must be disconnected while programming. Pin 13
 * (built-in LED) lights while a frame is being received and answered.
 *
 * Protocol:
 *   Incoming (from hub):  <9,9,RRDDD>   e.g. <9,9,11001> = register 11, cmd 001
 *   Outgoing (to hub):    |message|     e.g. |Hello World|
 *
 * Command map (the hub routes registers 11-20 to this node):
 *   Reg 11: control commands
 *           000 LEDs off (fades out)  001 hello               004 rainbow
 *           005 color cycle           006 pause               007 loading bar
 *           008 strip lit? (0/1)      009 brightness level read-back
 *           031/032/033/034 outlet on / off / status / toggle
 *           041/042/043/044 motor fwd / rev / stop / status (0=idle,1=fwd,2=rev)
 *           002, 003 and 010 are unused and answer with the error frame.
 *   Reg 12: stage RED   (0-255)
 *   Reg 13: stage GREEN (0-255)
 *   Reg 14: stage BLUE  (0-255) - and paints the strip
 *           Send 12/13/14 in that order to set a solid color; it is the only
 *           way to set one. Red and green stage silently and blue paints all
 *           three at once, so the strip never shows a half-mixed color, and the
 *           fill stops (and fades out) any running effect. Keying the paint to
 *           blue also keeps it idempotent, which matters because the master
 *           retries any command it gets no reply to: a repeat simply repaints
 *           the same color instead of leaving the channels out of step.
 *   Reg 16: BRIGHTNESS level (0-255), mapped perceptually onto
 *           MIN_BRIGHTNESS..MAX_BRIGHTNESS. Level 0 is the dimmest setting,
 *           never fully off; 011009 echoes the level back linearly.
 *   Replies are the OK / hello text, a status digit, or the error text.
 *
 * Effects run in the background but only call strip.show() while the link is
 * idle, so an animation frame can never clip an incoming command - the idle
 * check is repeated after the frame is built and before it is written. Every
 * effect shares one frame clock (EFFECT_INTERVAL_MS, which sets smoothness
 * only) and one phase accumulator; each has its own <NAME>_STEP constant for
 * speed, metered against real elapsed time so irregular frames never change it.
 *
 * After link traffic, effect frames are held off: EFFECT_RESUME_DELAY_MS for a
 * frame that was decoded and answered, since no retry is coming, and the longer
 * EFFECT_RX_HOLDOFF_MS for anything incomplete or corrupted, so the master's
 * retry lands on a quiet link.
 */

#include <Adafruit_NeoPixel.h>

// --- WS2812 LED strip ---
#define LED_PIN 11     // WS2812 data pin
#define NUM_LEDS 60    // number of pixels on the strip (change to match hardware)
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- LED brightness and effect tuning constants ---
#define MAX_BRIGHTNESS 160 // brightness ceiling (0-255); a level of 255 maps here
#define MIN_BRIGHTNESS 1   // brightness floor (1-254); a level of 0 maps here, so the
                           // strip is never driven at an actual brightness of 0
#if MIN_BRIGHTNESS >= MAX_BRIGHTNESS
#error "MIN_BRIGHTNESS must be less than MAX_BRIGHTNESS"
#endif
#if MIN_BRIGHTNESS < 1
#error "MIN_BRIGHTNESS must be at least 1 (a value of 0 would blank the strip)"
#endif
// --- effect frame rate (one shared clock for every effect) ---
// How often any running effect redraws. Smoothness only: each effect's speed is
// set separately below and metered against elapsed time, so changing this does
// not change how fast anything moves. Lower draws more finely but spends more
// time with interrupts off (a 60-pixel show() blanks them for ~1.9 ms, so 30 ms
// spends about 6% of the time unable to receive); higher makes each frame's
// jump coarser. To slow an effect down, lower its step constant instead -
// raising this keeps the jump as large and only shows it less often, which is
// what reads as stepping. Valid: 10..1000. Useful: 20..40.
const uint16_t EFFECT_INTERVAL_MS = 30;

// --- per-effect speed ---
// Each effect advances by its own step every EFFECT_INTERVAL_MS, metered from
// elapsed milliseconds rather than applied whole per frame, so the step is a
// true rate: a delayed or skipped frame changes how finely the animation is
// drawn, never how fast it travels.
//
// Hue steps are in hue units per frame (65536 units = full wheel):
//   seconds per wheel = 65536 * EFFECT_INTERVAL_MS / (step * 1000)
//   Valid 1..65535, useful 20..800 (about 98 s to 2.5 s per wheel at 30 ms).
//   Past ~800 the per-frame color jump shows as stepping.
const uint16_t RAINBOW_HUE_STEP  = 100; // 200 = ~9.8 s per full wheel at 30 ms
const uint16_t CYCLE_HUE_STEP    = 65;  // 150 = ~13.1 s per full wheel at 30 ms

// Pattern steps are in hundredths of a pixel per frame (100 = 1 px):
//   pixels per second = step * 10 / EFFECT_INTERVAL_MS
//   Valid 1..65535, useful 20..300 (6.7 to 100 px/s at 30 ms). Sub-pixel steps
//   let a pattern crawl on the shared clock instead of needing its own longer
//   frame interval; above 100 it skips pixels and looks choppy.
const uint16_t BAR_PIXEL_STEP    = 80; // 100 = 1.00 px/frame = 33 px/s at 30 ms

// Ceiling on the elapsed time one frame may absorb, in frame intervals. A
// small gap is caught up smoothly; a long one - the holdoff during a command
// burst - is clamped so the animation resumes where it paused instead of
// jumping. Valid: 1..8 (1 = never catch up; that time is simply lost).
const uint8_t EFFECT_CATCHUP_FRAMES = 3;

// --- stop fade ---
// How long the strip takes to dim to black when an effect stops or the LEDs are
// switched off, and in how many steps. This is the one place the sketch holds
// the loop on purpose, so it is kept short: the reply is sent and flushed
// first, incoming bytes buffer in the USART meanwhile, and the fade cuts
// straight to black as soon as a new command appears.
//   STOP_FADE_MS valid 0..5000 (0 disables the fade - the strip blanks at once).
//   STOP_FADE_STEPS valid 2..255. Keep STOP_FADE_MS/steps at or above ~15 ms,
//   or a step is shorter than the strip write itself.
const uint16_t STOP_FADE_MS    = 600;
const uint8_t  STOP_FADE_STEPS = 30;

// Gamma correction makes mid-range colors look right, but compresses the low
// end hard enough that a channel crossing the bottom of its range can pop on a
// slow fade. Set to 0 for a linear ramp: flatter, but perfectly even.
#define EFFECT_USE_GAMMA 1

// Spatial hue dither for the color-cycle effect, in hue units. That effect
// shows one color on every pixel, so when a channel's output steps by one -
// unavoidable, since the strip takes whole 0-255 values and near red/orange the
// green channel has few levels left after gamma and brightness scaling - all 60
// pixels step at the same instant and the strip visibly jumps. Fanning the
// pixels over a tiny hue range staggers those steps into a dissolve between two
// adjacent levels. The spread wants to be roughly the hue distance between two
// output steps: a few hundred units near the bottom of a channel.
//   Valid 0..65535, useful 300..1200. 0 makes every pixel identical (and the
//   strip jump); above ~2000 the spread shows as a faint gradient.
const uint16_t CYCLE_DITHER_SPREAD = 700;
// Step between neighboring pixels' dither ranks. Coprime with NUM_LEDS so the
// ranks form a permutation and adjacent pixels get widely separated offsets -
// a scattered dissolve rather than an edge wiping along the strip. Any coprime
// value works; 23 and 60 are.
const uint16_t CYCLE_DITHER_STRIDE = 23;

// How far the loading bar's background is scaled toward black (255 = left
// alone). Bar and background are opposite hues, which separates them by color;
// dimming the background separates them by brightness too, so the fill edge
// stays obvious whichever random pair comes up.
//   Useful: 20..80. Above ~100 a bright background can rival the bar.
const uint8_t BAR_BACK_SCALE  = 45;
#if NUM_LEDS < 2
#error "NUM_LEDS must be at least 2 (the loading bar needs somewhere to fill)"
#endif

// The step constants above as phase units per millisecond, in 1/4096ths fixed
// point, rounded to nearest and folded by the compiler. The sub-unit precision
// lets a slow animation advance a fraction of a step per frame instead of
// rounding down to nothing. A uint16_t step can never push one clamped frame's
// advance (rate * EFFECT_INTERVAL_MS * EFFECT_CATCHUP_FRAMES) near the 32-bit
// accumulator's limit, so no range guard is needed.
#define HUE_RATE(stepPerFrame) \
  ((((uint32_t)(stepPerFrame) << 12) + (EFFECT_INTERVAL_MS / 2)) / EFFECT_INTERVAL_MS)
#define PIXEL_RATE_(stepPerFrame) \
  ((((uint32_t)(stepPerFrame) << 12) + (50UL * EFFECT_INTERVAL_MS)) / (100UL * EFFECT_INTERVAL_MS))

// One full period of each effect's phase, in the same units. The phase is
// wrapped to this after every advance, so the accumulator can neither drift nor
// overflow however long an effect runs.
#define HUE_PERIOD   (65536UL << 12)                          // one color wheel
#define BAR_PERIOD   ((uint32_t)(2 * NUM_LEDS) << 12)         // both fill passes

// --- link to the hub (hardware USART) ---
// Pin 0 = RX, pin 1 = TX. No port object is declared; the core provides
// "Serial". See the header notes on sharing this port with USB.
#define serialBaudRate 9600

// --- link activity LED ---
#define LINK_LED_PIN 13

// --- power control I/O (from AutomateHub_1) ---
#define PWR_TRIG_1 9  // TRIGGER TIP - pulses to switch the attached device
#define PWR_SENS_1 10 // SENSE RING  - reads the attached device's power state

// --- bed motor control I/O ---
// Two active-high inputs on the existing motor driver: 5V runs the motor in
// that direction, 0V stops it. The state machine guarantees the two pins are
// never high at the same time.
#define MOTOR_FWD_PIN 8 // 5V = motor runs forward
#define MOTOR_REV_PIN 7 // 5V = motor runs in reverse

// --- bed motor tuning constants ---
// A command starts a pulse of MOTOR_PULSE_MS. Repeating the same direction
// while running pushes the stop deadline back out to now + MOTOR_PULSE_MS, so a
// burst of commands keeps the motor moving and the run tracks how long they
// keep arriving (velocity control); it always stops MOTOR_PULSE_MS after the
// last one. After any stop the motor must idle for MOTOR_SETTLE_MS before
// starting in the opposite direction.
//
// Run time is metered by a leaky bucket: each millisecond of running fills it
// by 1 ms, each MOTOR_DUTY_REST_RATIO ms of rest drains 1 ms, capping the
// sustained duty cycle near 1/(ratio+1) - 9 allows ~10%, typical for linear
// actuators (raise it for a lower-duty motor). Since the bucket fills as fast
// as the motor runs, its size is also the longest possible single run, so no
// separate run timer is needed. A command is refused (BHE: COOLDOWN) unless the
// bucket has room for one minimum pulse; if it fills mid-run the motor is
// force-stopped and run commands are locked out for MOTOR_LOCKOUT_MS.
const uint16_t MOTOR_PULSE_MS       = 1200;    // minimum pulse / per-command run time
const uint16_t MOTOR_SETTLE_MS      = 1500;    // required off-time before reversing direction
const unsigned long MOTOR_DUTY_BUDGET_MS = 60000UL; // bucket size: max total AND max single run (1 minute)
const uint8_t MOTOR_DUTY_REST_RATIO = 9;            // ms of rest that drain 1 ms of run time
const unsigned long MOTOR_LOCKOUT_MS  = 300000UL;   // cooldown after filling the bucket (5 minutes)

// --- bed motor state (all transitions handled non-blocking in serviceMotor) ---
const uint8_t motorIdle = 0; // also used as "no previous direction" at boot
const uint8_t motorFwd  = 1;
const uint8_t motorRev  = 2;
uint8_t motorDirection = motorIdle;     // direction currently driven (idle = pins low)
uint8_t motorLastDirection = motorIdle; // direction of the previous run (for the settle rule)
unsigned long motorStopAt = 0;          // millis() deadline when the current run ends
unsigned long motorLastStop = 0;        // millis() when the motor last stopped (settle timer)
unsigned long motorLockoutUntil = 0;    // millis() deadline when the cooldown lockout ends
unsigned long motorDutyUsedMs = 0;      // leaky bucket: accumulated run time not yet drained
unsigned long motorDutyLastUpdate = 0;  // millis() up to which the bucket has been accounted
bool motorLockedOut = false;            // true while the cooldown lockout is active

// --- protocol constants (match AutomateHub_1) ---
const char nullTrm = '\0';
const uint8_t maxMessage = 32;
const uint8_t cmdRegLen = 2; // register field length (2 digits)
const uint8_t cmdDatLen = 3; // data field length (3 digits)
const char serialDataStart = '<';
const char serialDataEnd = '>';
const char respDelimiter = '|';

// --- register map (which command register does what) ---
const uint8_t regControl           = 11; // general commands
const uint8_t regStageRed          = 12; // stage red channel
const uint8_t regStageGreen        = 13; // stage green channel
const uint8_t regStageBlue         = 14; // stage blue channel
const uint8_t regBrightness        = 16; // set overall brightness

// --- control-register (11) command values ---
const uint16_t ctrlAbout           = 0; // reply with the hello message
const uint16_t ctrlStopLeds        = 1; // clear the strip (stops any effect)
const uint16_t ctrlPauseLeds       = 2; // pause the running effect (freeze, do not clear)
const uint16_t ctrlStatusLeds      = 3; // report whether the strip is lit or blank
const uint16_t ctrlBrightnessLeds  = 4; // show current brightness level
const uint16_t ctrlRainbow         = 5; // start the rainbow effect
const uint16_t ctrlCycle           = 6; // start the color-cycle effect
const uint16_t ctrlSweep           = 7; // start the loading bar effect
const uint16_t ctrl1PowerOn        = 31; // switch device #1 on (if currently off)
const uint16_t ctrl1PowerOff       = 32; // switch device #1 off (if currently on)
const uint16_t ctrl1PowerStatus    = 33; // report device #1's current power state
const uint16_t ctrl1PowerToggle    = 34; // toggle device #1's power state
const uint16_t ctrlMotorFwd        = 41; // run the bed motor forward (repeat to extend)
const uint16_t ctrlMotorRev        = 42; // run the bed motor in reverse (repeat to extend)
const uint16_t ctrlMotorStop       = 43; // stop the bed motor immediately (always allowed)
const uint16_t ctrlMotorStatus     = 44; // report motor state (0=idle, 1=fwd, 2=rev)

// --- reply strings ---
const char aboutMessage[]          = "BHE: by Ben Provenzano III";
const char okMessage[]             = "BHE: OK";
const char errorMessage[]          = "BHE: ERROR!"; // unrecognized register/command or out-of-range value
const char motorWaitMessage[]      = "BHE: WAIT";    // motor must settle before reversing
const char motorCoolMessage[]      = "BHE: COOLDOWN"; // motor commands locked out (cooldown)
const char powerStateOff[]         = "0"; // status reply: device is off
const char powerStateOn[]          = "1"; // status reply: device is on
const char motorStateRev[]         = "2"; // motor status reply: running in reverse

// --- staged LED state (loaded across commands, then applied) ---
uint8_t stagedRed = 0;
uint8_t stagedGreen = 0;
uint8_t stagedBlue = 0;
uint8_t ledBrightness = MAX_BRIGHTNESS; // scaled brightness actually sent to the strip
uint8_t ledBrightnessLevel = 255;       // last 0-255 level commanded on register 16, kept
                                        // linear so the UI slider can be read straight
                                        // back (the perceptual curve is one-way lossy)

// --- effect state (background animations) ---
const uint8_t effectNone    = 0; // no animation running
const uint8_t effectRainbow = 1; // rainbow hues trailing up the strip
const uint8_t effectCycle   = 2; // whole strip cycling through hues
const uint8_t effectSweep   = 4; // loading bar filling the strip, alternating ends
uint8_t effectMode = effectNone;
bool effectPaused = false;
// How far the running effect is through its animation, in 1/4096ths of one
// phase unit - a hue unit for the hue effects, a pixel for the loading bar.
// Only one effect is ever active, so they share this one accumulator;
// startEffect() zeroes it and serviceEffects() wraps it to the effect's
// period.
uint32_t effectPhase = 0;
unsigned long lastEffectUpdate = 0; // millis() of the last frame actually SHOWN
// --- loading bar state ---
// The bar re-rolls its colors on every reset, so it must notice one:
// barLastPass holds the pass the current colors were picked for, and
// BAR_PASS_NONE forces a fresh pick on the effect's first frame.
const uint8_t BAR_PASS_NONE = 255;
uint8_t barLastPass = BAR_PASS_NONE;
uint32_t barFillColor = 0;
uint32_t barBackColor = 0;

// --- link recovery tuning ---
// Effect frames stay suppressed until effectHoldoffUntil. Raw link traffic
// pushes that deadline out by EFFECT_RX_HOLDOFF_MS, so a command clipped by a
// strip write silences the animation and the master's retry lands on a clean
// link. A frame that was decoded and answered will not be retried, so it holds
// off only for EFFECT_RESUME_DELAY_MS - long enough to bridge the gap between
// rapid-fire commands (back-to-back ztermcom calls land 20-50 ms apart) so a
// burst keeps the animation paused until it finishes, short enough that an
// effect appears to start at once. That delay is also how long an effect takes
// to visibly start: lower it for a snappier start, raise it if a burst is ever
// spaced wider.
const uint16_t EFFECT_RX_HOLDOFF_MS = 1000;
const uint16_t EFFECT_RESUME_DELAY_MS = 250;
// A partial frame idle this long lost its terminator to a corrupted byte (a
// whole command takes ~11 ms at 9600 baud), so it is abandoned rather than
// allowed to absorb the retry that follows.
const uint16_t FRAME_ABANDON_MS = 100;
// The master sends one terminator byte straight after each frame's '>'. The
// AutomateHub bit-bangs that byte on SoftwareSerial, which disables interrupts
// for its whole duration (~1 ms at 9600) and is deaf until it finishes. This
// port receives it in hardware, in the background, so a reply sent the instant
// the '>' decodes would start inside that deaf window and lose its leading '|',
// and the hub would time out waiting for a response. The reply is therefore
// held until the terminator lands, or this long if a master sends none at all
// (a direct connection to the Pi, for example).
const uint16_t REPLY_GUARD_MS = 5;
unsigned long lastRxActivity = 0;     // millis() of the last received byte
unsigned long effectHoldoffUntil = 0; // effects stay suppressed until this millis()
// Set after an answered frame so the hub's trailing terminator byte is not
// mistaken for fresh traffic and made to re-arm the long holdoff.
bool expectTrailingByte = false;

// --- receive buffer and reader state ---
char serialMessageIn[maxMessage + 1];
uint16_t serialCurPos = 0;
bool serialReading = 0;
bool serialMsgEnd = 0;

///////////////////////////////////////

// Arduino entry point: opens the hub link, configures the power and motor I/O,
// blanks the strip and clears the receive buffer. The hardware port receives on
// interrupt from the moment it is opened, so nothing has to be told to listen.
void setup() {
  pinMode(LINK_LED_PIN, OUTPUT);
  digitalWrite(LINK_LED_PIN, HIGH);
  // start serial
  Serial.begin(serialBaudRate);
  // power control I/O
  pinMode(PWR_TRIG_1, OUTPUT);
  digitalWrite(PWR_TRIG_1, LOW);
  pinMode(PWR_SENS_1, INPUT_PULLUP);
  // both direction lines low before and after they become outputs, so the
  // motor can never twitch at boot
  digitalWrite(MOTOR_FWD_PIN, LOW);
  digitalWrite(MOTOR_REV_PIN, LOW);
  pinMode(MOTOR_FWD_PIN, OUTPUT);
  pinMode(MOTOR_REV_PIN, OUTPUT);
  strip.begin();
  strip.setBrightness(ledBrightness);
  strip.clear();
  strip.show();
  serialMessageIn[0] = nullTrm;
  delay(1000);
  digitalWrite(LINK_LED_PIN, LOW);
}

// Services the hub link first so incoming bytes are handled promptly, then the
// motor deadlines (millis() comparisons only, so a running motor never delays
// the link), then any running effect - which draws only while the link is
// idle, so a frame can never overlap an incoming command.
void loop() {
  serviceSerial();
  serviceMotor();
  serviceEffects();
}

// Reads framed commands from the link one character at a time and decodes a
// frame once it completes. Every received byte stamps lastRxActivity and arms
// the long effect holdoff (a retry may be coming), except the terminator byte
// trailing an answered frame. A partial frame stalled for FRAME_ABANDON_MS lost
// its terminator to a corrupted byte and is abandoned, so a retry decodes
// cleanly instead of being appended to stale data. The built-in LED is lit for
// the whole transaction.
void serviceSerial() {
  if (serialMsgEnd == 0) {
    // still collecting a message
    if (Serial.available()) {
      char incomingChar = Serial.read();
      lastRxActivity = millis();
      // the byte following an answered frame is the hub's terminator: expected
      // noise, so it must not arm the long holdoff. A frame start never is.
      bool isExpectedTerminator = expectTrailingByte && (incomingChar != serialDataStart);
      expectTrailingByte = false;
      if (isExpectedTerminator == false) {
        effectHoldoffUntil = lastRxActivity + EFFECT_RX_HOLDOFF_MS;
      }
      readSerialData(incomingChar, serialDataStart, serialDataEnd);
    } else if (serialReading == 1) {
      digitalWrite(LINK_LED_PIN, HIGH);
      // mid-frame with no new data: abandon the frame if it has stalled
      if ((millis() - lastRxActivity) > FRAME_ABANDON_MS) {
        serialReading = 0;
        serialCurPos = 0;
        serialMessageIn[0] = nullTrm;
      }
    }
  } else {
    // Wait for the master's terminator byte before replying, giving up after
    // REPLY_GUARD_MS (see that constant for why replying sooner loses the reply
    // on a SoftwareSerial master). The byte is only waited for, never consumed -
    // expectTrailingByte swallows it on the next service pass.
    unsigned long guardStart = millis();
    while ((Serial.available() == 0) &&
          ((millis() - guardStart) < REPLY_GUARD_MS)) {
      ; // spin: nothing else may run here, the reply is the next thing out
    }
    // a full frame has arrived -> decode and act on it
    decodeMessage();
    // the frame was decoded and answered, so the master will not retry it:
    // resume effects after the burst window and ignore the hub's terminator
    expectTrailingByte = true;
    effectHoldoffUntil = millis() + EFFECT_RESUME_DELAY_MS;
    // clear for the next message
    serialMessageIn[0] = nullTrm;
    serialMsgEnd = 0;
    digitalWrite(LINK_LED_PIN, LOW);
  }
}

// Advances the active animation by at most one frame per call. Runs only when
// an effect is active, not paused, and the link is completely idle. That is what
// keeps effects clear of commands: strip.show() disables interrupts, stalling
// the USART receive interrupt, so a frame is never written while a command could
// be in flight.
//
// The draw<Effect>Frame() helpers only fill the pixel buffer. Everything common
// to them - the second link check, the strip write, the frame clock and the
// phase advance - lives here, so an effect is added by writing one draw
// function plus one case below.
void serviceEffects() {
  // nothing to draw if no effect is running or it is paused (frozen)
  if (effectMode == effectNone || effectPaused) {
    return;
  }
  // never drive the strip while a command is arriving or already queued
  if (linkIsBusy()) {
    return;
  }
  // hold off until the link has settled (see the link recovery tuning notes).
  // The signed cast keeps the comparison correct across millis() rollover.
  unsigned long now = millis();
  if ((long)(now - effectHoldoffUntil) < 0) {
    return;
  }
  // frame-rate gate (the one shared animation clock)
  unsigned long sinceLastFrame = now - lastEffectUpdate;
  if (sinceLastFrame < EFFECT_INTERVAL_MS) {
    return;
  }
  // How much animation time this frame may represent. Frames are never exactly
  // one interval apart, so effects advance by measured elapsed time rather than
  // a fixed step, which is what keeps their speed steady. The clamp stops a long
  // suppression from being paid back as one large jump.
  const uint16_t catchupCap = (uint16_t)EFFECT_INTERVAL_MS * EFFECT_CATCHUP_FRAMES;
  uint16_t frameElapsedMs = (sinceLastFrame > catchupCap) ? catchupCap
                                                          : (uint16_t)sinceLastFrame;
  // Build the frame into the pixel buffer, and pick up the speed and period
  // of whichever effect was drawn so the phase can be advanced below.
  uint32_t phaseRate;
  uint32_t phasePeriod;
  switch (effectMode) {
    case effectRainbow:
      drawRainbowFrame();
      phaseRate = HUE_RATE(RAINBOW_HUE_STEP);
      phasePeriod = HUE_PERIOD;
      break;
    case effectCycle:
      drawColorCycleFrame();
      phaseRate = HUE_RATE(CYCLE_HUE_STEP);
      phasePeriod = HUE_PERIOD;
      break;
    case effectSweep:
      drawLoadingBarFrame();
      phaseRate = PIXEL_RATE_(BAR_PIXEL_STEP);
      phasePeriod = BAR_PERIOD;
      break;
    default:
      return; // unknown mode: nothing drawn, nothing to show
  }
  // Building a frame takes real time, so re-check the link: a byte that arrived
  // meanwhile would be clipped by the interrupts-off strip write. Deferring
  // costs nothing - the clock and phase do not move, so the next idle pass shows
  // the very same frame.
  if (linkIsBusy()) {
    return;
  }
  strip.show();
  lastEffectUpdate = now; // only a frame that was actually SHOWN moves the clock
  // advance the phase by this frame's worth of travel, wrapped to the effect's
  // period (the clamp above bounds this to a couple of subtractions)
  effectPhase += phaseRate * (uint32_t)frameElapsedMs;
  while (effectPhase >= phasePeriod) {
    effectPhase -= phasePeriod;
  }
}

// Selects an effect, rewinds it to the start of its animation, un-pauses it,
// and schedules the first frame to draw on the next idle service pass.
void startEffect(uint8_t mode) {
  effectMode = mode;
  effectPaused = false;
  effectPhase = 0;                               // begin at the effect's origin
  barLastPass = BAR_PASS_NONE;                   // loading bar re-rolls its colors
  lastEffectUpdate = millis() - EFFECT_INTERVAL_MS; // draw promptly
}

// Stops any running animation and fades what it left on the strip to black,
// ending blanked. A fill following a stop therefore reads as the old animation
// dimming away before the new color appears. The fade runs only if an effect
// was actually running, so replacing one static color with another changes over
// immediately.
void stopEffect() {
  bool effectWasRunning = (effectMode != effectNone);
  effectMode = effectNone;
  effectPaused = false;
  if (effectWasRunning) {
    fadeStripOut();
  }
}

// Dims the strip's contents to black in STOP_FADE_STEPS even steps, ending
// blank on every path.
//
// The pixels are snapshotted first and every step scales from that snapshot,
// never from the previous step: rescaling the live buffer compounds its
// rounding, dragging the low end to black early and turning the last third of
// the fade into a hard cut. setBrightness() is avoided for the same reason - it
// rescales the pixel buffer in place, and setBrightness(0) zeroes it
// permanently. Scaling from the snapshot keeps every step an unrounded fraction
// of the original, and never touches the brightness the user set.
//
// The snapshot is three bytes per pixel rather than packed 32-bit colors, whose
// top byte would always be empty here: 180 bytes of stack instead of 240 at 60
// pixels, held only for the length of the fade.
void fadeStripOut() {
  if (isStripLit() == false) {
    return; // already blank: nothing to dim and nothing to rewrite
  }
  if (STOP_FADE_MS == 0) {
    clearStrip(); // fade disabled: blank instantly
    return;
  }
  // getPixelColor() divides the display brightness back out, so the fade works
  // in staged-color space and setPixelColor() re-applies it on the way out.
  uint8_t fadeSnapshot[NUM_LEDS][3];
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    uint32_t pixelColor = strip.getPixelColor(_idx);
    fadeSnapshot[_idx][0] = (uint8_t)(pixelColor >> 16); // red
    fadeSnapshot[_idx][1] = (uint8_t)(pixelColor >> 8);  // green
    fadeSnapshot[_idx][2] = (uint8_t)pixelColor;         // blue
  }
  // Abort test for the loop below. linkIsBusy() cannot be used: this fade runs
  // from inside decodeMessage(), where serialMsgEnd is still set for the command
  // being answered and the master's terminator byte is already buffered, so it
  // is always true here and would kill the fade on its first step. Only traffic
  // arriving AFTER the fade starts matters, so the pending byte count is sampled
  // once and growth past it counts as a new command. serialReading cannot change
  // meanwhile, since the link is not serviced during the fade.
  int linkPendingAtStart = Serial.available();
  const uint16_t fadeStepMs = STOP_FADE_MS / STOP_FADE_STEPS;
  for (uint8_t _step = 1; _step <= STOP_FADE_STEPS; _step++) {
    // a NEW command arriving mid-fade wins: cut to black and let it be serviced
    if (Serial.available() > linkPendingAtStart) {
      clearStrip();
      return;
    }
    // full brightness before the first step, exactly black on the last one
    uint8_t fadeScale = (uint8_t)(255 - (((uint32_t)_step * 255) / STOP_FADE_STEPS));
    for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
      // repack for scaleColor() so its scaling rule is shared rather than
      // copied into this loop
      uint32_t snapshotColor = ((uint32_t)fadeSnapshot[_idx][0] << 16) |
                               ((uint32_t)fadeSnapshot[_idx][1] << 8) |
                               (uint32_t)fadeSnapshot[_idx][2];
      strip.setPixelColor(_idx, scaleColor(snapshotColor, fadeScale));
    }
    strip.show();
    // Hold the step without delay(): serviceMotor() enforces the motor's run
    // and duty deadlines, and a blocking delay would let a motor overrun its
    // stop time by most of the fade. The link cannot be serviced from inside a
    // half-drawn fade, but bytes buffer in the USART and the check above cuts
    // the fade short on the next step.
    unsigned long stepStarted = millis();
    while ((millis() - stepStarted) < fadeStepMs) {
      serviceMotor();
    }
  }
  // the last step scaled by 0, so the strip already shows black - a
  // clearStrip() here would only rewrite it
}

// True while the link has traffic in flight: a frame being received, a decoded
// frame waiting to be answered, or a byte already sitting in the RX buffer.
bool linkIsBusy() {
  return (serialReading || serialMsgEnd || Serial.available());
}

// Converts a 16-bit hue to the packed color the effects draw with, applying
// gamma correction unless EFFECT_USE_GAMMA is 0.
uint32_t effectHueColor(uint16_t hue) {
#if EFFECT_USE_GAMMA
  return strip.gamma32(strip.ColorHSV(hue));
#else
  return strip.ColorHSV(hue);
#endif
}

// Builds one frame of the rainbow effect: a full hue spread mapped across the
// strip, scrolled by the current phase so the colors trail up the strip.
void drawRainbowFrame() {
  uint16_t baseHue = (uint16_t)(effectPhase >> 12); // phase wraps at one wheel
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    uint16_t pixelHue = baseHue + (uint16_t)((uint32_t)_idx * 65536UL / NUM_LEDS);
    strip.setPixelColor(_idx, effectHueColor(pixelHue));
  }
}

// Builds one frame of the color-cycle effect: the whole strip shows the hue the
// phase sits on, walking the wheel as the phase advances. Each pixel is nudged
// by a fraction of CYCLE_DITHER_SPREAD so the strip does not cross an output
// quantization step all at once - see that constant.
void drawColorCycleFrame() {
  uint16_t baseHue = (uint16_t)(effectPhase >> 12); // phase wraps at one wheel
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    // scatter the dither ranks so neighbors are far apart in the spread
    uint16_t ditherRank = (uint16_t)(((uint32_t)_idx * CYCLE_DITHER_STRIDE) % NUM_LEDS);
    uint16_t hueOffset = (uint16_t)(((uint32_t)ditherRank * CYCLE_DITHER_SPREAD) / NUM_LEDS);
    strip.setPixelColor(_idx, effectHueColor(baseHue + hueOffset));
  }
}

// Scales a packed color toward black: 255 leaves it unchanged, 0 blacks it
// out. The +1 makes the divisor 256 so the scaling is a shift rather than a
// divide, and a scale of 255 still returns the color exactly.
uint32_t scaleColor(uint32_t color, uint8_t scale) {
  uint16_t _mul = (uint16_t)scale + 1;
  return strip.Color((uint8_t)((((color >> 16) & 0xFF) * _mul) >> 8),
                     (uint8_t)((((color >> 8) & 0xFF) * _mul) >> 8),
                     (uint8_t)(((color & 0xFF) * _mul) >> 8));
}

// Rolls a fresh color pair for the loading bar: a random background hue and its
// opposite on the wheel for the fill, the largest hue separation available, so
// the pair contrasts however the roll lands. The background is also dimmed by
// BAR_BACK_SCALE, since hue alone is not enough when a roll lands on two hues
// the eye reads as similarly dark.
//
// random() replays one fixed sequence unless seeded. It is seeded on first use
// rather than in setup() because the effect is started by a hub command, so the
// microsecond timer at that moment is unpredictable - at boot it would read
// nearly the same value every time.
void pickBarColors() {
  static bool barSeeded = false;
  if (barSeeded == false) {
    randomSeed(micros());
    barSeeded = true;
  }
  uint16_t backHue = (uint16_t)random(65536);
  barBackColor = scaleColor(effectHueColor(backHue), BAR_BACK_SCALE);
  barFillColor = effectHueColor((uint16_t)(backHue + 32768)); // opposite hue
}

// Builds one frame of the loading bar effect: a solid bar grows from one end of
// the strip to the other over a dimmed background of the opposite hue, then
// restarts from the OTHER end with a fresh random color pair.
//
// The phase runs 0 .. 2*NUM_LEDS-1 and its top half is the return pass, so the
// fill direction comes from the phase alone with no direction flag. Only the
// color re-roll needs memory, since it happens once per pass rather than once
// per frame; barLastPass provides it.
void drawLoadingBarFrame() {
  uint16_t barPos = (uint16_t)(effectPhase >> 12); // 0 .. 2*NUM_LEDS-1
  // pass 0 fills from pixel 0 upward, pass 1 fills from the far end downward
  uint8_t barPass = (barPos < NUM_LEDS) ? 0 : 1;
  if (barPass != barLastPass) {
    pickBarColors(); // the bar just reset (or just started)
    barLastPass = barPass;
  }
  // one pixel lit at the start of a pass, the whole strip at the end of it
  uint16_t filledCount = (barPass == 0) ? (barPos + 1) : (barPos - NUM_LEDS + 1);
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    // _idx counts from the filling end, barIdx is the pixel that end maps to
    uint16_t barIdx = (barPass == 0) ? _idx : (uint16_t)(NUM_LEDS - 1 - _idx);
    strip.setPixelColor(barIdx, (_idx < filledCount) ? barFillColor : barBackColor);
  }
}

// Character-by-character framed reader: waits for startInd, buffers characters
// into serialMessageIn (overflow-safe), and sets serialMsgEnd on endInd. A
// startInd received mid-frame restarts the frame, since the payload never
// contains '<': if a terminator was lost to a corrupted byte, the master's retry
// begins a clean message instead of being appended to the stale one.
void readSerialData(char rc, char startInd, char endInd) {
  if (serialReading == 1) {
    // end-of-reading
    if (rc == endInd) {
      // terminate the string
      serialMessageIn[serialCurPos] = nullTrm;
      serialReading = 0;
      serialCurPos = 0;
      serialMsgEnd = 1;
    } else if (rc == startInd) {
      // new frame beginning mid-read -> discard the stale partial frame
      serialCurPos = 0;
    } else {
      // store characters in buffer
      serialMessageIn[serialCurPos] = rc;
      serialCurPos++;
      // prevent overflow
      if (serialCurPos >= maxMessage) {
        serialCurPos = maxMessage - 1;
      }
    }
  } else {
    // start reading
    if (rc == startInd) {
      serialReading = 1;
      serialCurPos = 0;
      serialMsgEnd = 0;
    }
  }
}

// Validates and decodes a received "9,9,RRDDD" message: confirms the delimiter
// count and the leading "9" prefix, then passes the position of the numeric
// payload to processCmdData(). Malformed input is answered with an error frame
// so the hub never has to wait out its response timeout.
void decodeMessage() {
  const char fieldDelimiter = ',';
  // count field delimiters
  uint8_t delimiterCount = 0;
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    char _vchr = serialMessageIn[_idx];
    if (_vchr == nullTrm) {
      break;
    }
    if (_vchr == fieldDelimiter) {
      delimiterCount++;
    }
  }
  if (delimiterCount < 2) {
    sendResponse(errorMessage);
    return;
  }
  // find the first delimiter position (end of the prefix field)
  uint8_t firstDelimPos = 0;
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    char _vchr = serialMessageIn[_idx];
    if (_vchr == nullTrm) {
      break;
    }
    if (_vchr == fieldDelimiter) {
      firstDelimPos = _idx;
      break;
    }
  }
  // read the prefix field into a small buffer
  char prefixBuffer[11]; // matches the hub's 10-char prefix limit + terminator
  uint8_t prefixLen = 0;
  for (uint8_t _idx = 0; _idx < firstDelimPos; _idx++) {
    if (prefixLen >= 10) {
      break;
    }
    prefixBuffer[prefixLen] = serialMessageIn[_idx];
    prefixLen++;
  }
  prefixBuffer[prefixLen] = nullTrm;
  uint8_t prefixValue = atoi(prefixBuffer);
  // find the second delimiter position (payload begins right after it)
  uint8_t delimSeen = 0;
  uint8_t secondDelimPos = 0;
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    char _vchr = serialMessageIn[_idx];
    if (_vchr == nullTrm) {
      break;
    }
    if (_vchr == fieldDelimiter) {
      if (delimSeen == 1) {
        secondDelimPos = _idx;
        break;
      }
      delimSeen++;
    }
  }
  // validate the prefix and process the command payload
  if (prefixValue == 9) {
    processCmdData(secondDelimPos + 1);
  } else {
    sendResponse(errorMessage); // bad prefix
  }
}

// Extracts the fixed-length control code at messageStart, splits it into the
// 2-digit register and 3-digit data fields, and hands the pair to
// handleCommand().
void processCmdData(uint8_t messageStart) {
  uint8_t _numcnt = 0;
  uint8_t _cmdlen = cmdRegLen + cmdDatLen;
  char _cmdarr[_cmdlen + 1];
  // extract the control code digits from the message
  for (uint8_t _idx = messageStart; _idx < maxMessage; _idx++) {
    char _curchar = serialMessageIn[_idx];
    if (_curchar == nullTrm) {
      break;
    }
    if (isDigit(_curchar) && _numcnt < _cmdlen) {
      _cmdarr[_numcnt] = _curchar;
      _numcnt++;
    }
  }
  _cmdarr[_numcnt] = nullTrm;
  // validate control code length
  if (_numcnt != _cmdlen) {
    sendResponse(errorMessage); // wrong length
    return;
  }
  // extract register select (first cmdRegLen digits)
  char _regarr[cmdRegLen + 1];
  uint8_t _regidx = 0;
  for (uint8_t _idx = 0; _idx < cmdRegLen; _idx++) {
    _regarr[_regidx] = _cmdarr[_idx];
    _regidx++;
  }
  _regarr[_regidx] = nullTrm;
  uint8_t messageRegister = atoi(_regarr);
  // extract control data (remaining cmdDatLen digits, starting after register)
  char _datarr[cmdDatLen + 1];
  uint8_t _dataidx = 0;
  for (uint8_t _idx = cmdRegLen; _idx < _cmdlen; _idx++) {
    _datarr[_dataidx] = _cmdarr[_idx];
    _dataidx++;
  }
  _datarr[_dataidx] = nullTrm;
  uint16_t messageData = atoi(_datarr);
  // act on the decoded register/command
  handleCommand(messageRegister, messageData);
}

// Dispatches a decoded register/command pair: register 11 carries every control
// command, 12-14 the color channels and 16 the brightness. Unknown registers
// and out-of-range values reply with the error frame.
void handleCommand(uint8_t messageRegister, uint16_t messageData) {
  switch (messageRegister) {
    case regControl:
      handleControlCommand(messageData);
      break;
    case regStageRed:
      if (messageData <= 255) {
        stageColorChannel((uint8_t)messageData, &stagedRed);
      } else {
        sendResponse(errorMessage);
      }
      break;
    case regStageGreen:
      if (messageData <= 255) {
        stageColorChannel((uint8_t)messageData, &stagedGreen);
      } else {
        sendResponse(errorMessage);
      }
      break;
    case regStageBlue:
      if (messageData <= 255) {
        applyStagedColor((uint8_t)messageData); // blue paints the strip
      } else {
        sendResponse(errorMessage);
      }
      break;
    case regBrightness:
      if (messageData <= 255) {
        setBrightnessLevel((uint8_t)messageData);
        sendResponse(okMessage);
      } else {
        sendResponse(errorMessage);
      }
      break;
    default:
      sendResponse(errorMessage);
      break;
  }
}

// Stages one color channel and answers the master, applying nothing. Red and
// green use this; blue paints the strip (below).
void stageColorChannel(uint8_t channelValue, uint8_t* channelTarget) {
  *channelTarget = channelValue;
  sendResponse(okMessage);
}

// Stages blue - the last channel a color set sends - then answers the master and
// paints the strip with all three staged channels. A color set MUST therefore
// end with blue; red and green alone stage without painting.
//
// Blue is the trigger rather than "all three staged since the last fill"
// because the master retries any command it gets no reply to, so a channel can
// arrive twice or go missing. Counting channels leaves that bookkeeping one out
// of step for good, and the next color set then fills early, mixing two new
// channels with one stale one. Keying the fill to blue holds no state to fall
// out of step: a duplicate repaints the same color, and a dropped command costs
// at most one stale fill that the next set corrects.
//
// The reply goes out before the strip is touched, like the other LED commands:
// show() disables interrupts and would stretch the gaps between the reply's
// bytes. sendResponse() flushes, so the reply is fully on the wire by then.
void applyStagedColor(uint8_t blueValue) {
  stagedBlue = blueValue;
  sendResponse(okMessage);
  stopEffect(); // a running effect fades out first, then the color appears
  fillStripWithStagedColor();
}

// Handles the register 11 command values: hello, LEDs off, the background
// effects, pause, the strip status and brightness read-backs, and the motor
// commands; anything else falls through to handlePowerCommand(). The reply is
// sent before any strip update, because show() disables interrupts to bit-bang
// the strip and would stretch the gaps between the reply's bytes. Effect start
// and pause only set state - their frames are drawn later by serviceEffects()
// while the link is idle.
void handleControlCommand(uint16_t messageData) {
  switch (messageData) {
    case ctrlAbout:
      sendResponse(aboutMessage);
      break;
    case ctrlStopLeds:
      sendResponse(okMessage);
      stopEffect();
      fadeStripOut(); // fades a static color out too; a no-op after an effect
                      // fade, which has already left the strip blank
      break;
    case ctrlRainbow:
      sendResponse(okMessage);
      startEffect(effectRainbow);
      break;
    case ctrlCycle:
      sendResponse(okMessage);
      startEffect(effectCycle);
      break;
    case ctrlSweep:
      sendResponse(okMessage);
      startEffect(effectSweep);
      break;
    case ctrlPauseLeds:
      effectPaused = true;
      sendResponse(okMessage);
      break;
    case ctrlStatusLeds:
      if (isStripLit()) {
        sendResponse(powerStateOn);
      } else {
        sendResponse(powerStateOff);
      }
      break;
    case ctrlBrightnessLeds: {
      if (isStripLit()) {
        char brightnessText[4];
        itoa(showLedsBrightness(), brightnessText, 10);
        sendResponse(brightnessText);
      } else {
        sendResponse(powerStateOff);
      }
      break;
    }
    case ctrlMotorFwd:
      handleMotorRunCommand(motorFwd);
      break;
    case ctrlMotorRev:
      handleMotorRunCommand(motorRev);
      break;
    case ctrlMotorStop:
      stopMotor(millis());
      sendResponse(okMessage);
      break;
    case ctrlMotorStatus:
      switch (motorDirection) {
        case motorFwd:
          sendResponse(powerStateOn);  // "1" = running forward
          break;
        case motorRev:
          sendResponse(motorStateRev); // "2" = running in reverse
          break;
        default:
          sendResponse(powerStateOff); // "0" = idle
          break;
      }
      break;
    default:
      handlePowerCommand(messageData);
      break;
  }
}

// Handles the outlet commands (031 on, 032 off, 033 status, 034 toggle),
// mirroring AutomateHub_1's power logic. Reads the device's state from the sense
// line, then either reports it or pulses the trigger to switch it. The sense
// line is inverted (INPUT_PULLUP), so LOW means on. On/off pulse only when a
// change is needed; toggle always pulses. Status replies with the on/off digit,
// or the error frame on an invalid (bouncing) reading. Any command value outside
// this range is rejected here, which is what makes unused register 11 values
// answer with the error frame.
void handlePowerCommand(uint16_t messageData) {
  // reject anything outside the defined power command range
  if (messageData < ctrl1PowerOn || messageData > ctrl1PowerToggle) {
    sendResponse(errorMessage);
    return;
  }
  int senseState = powerSense(PWR_SENS_1);
  if (messageData == ctrl1PowerStatus) {
    // report the current power state (sense is inverted)
    switch (senseState) {
      case 1: // sense high -> device off
        sendResponse(powerStateOff);
        break;
      case 0: // sense low -> device on
        sendResponse(powerStateOn);
        break;
      default: // invalid / bouncing reading
        sendResponse(errorMessage);
        break;
    }
  } else {
    // on / off / toggle -> decide whether to pulse the trigger
    if (senseState == 0) { // device currently on
      if (messageData == ctrl1PowerOff) {
        powerPulse(PWR_TRIG_1);
      }
    } else { // device currently off (or invalid reading)
      if (messageData == ctrl1PowerOn) {
        powerPulse(PWR_TRIG_1);
      }
    }
    if (messageData == ctrl1PowerToggle) {
      powerPulse(PWR_TRIG_1);
    }
    sendResponse(okMessage);
  }
}

// Samples a pin 16 times back-to-back for a debounced consensus reading:
// 1 = all high, 0 = all low, -1 = samples disagreed (bouncing/invalid).
int powerSense(int powerSensePin) {
  const int inputReadings = 16;
  int total = 0;
  for (int idx = 0; idx < inputReadings; idx++) {
    // read then add the reading to the total
    total = total + digitalRead(powerSensePin);
  }
  switch (total) {
    case inputReadings:
      // high reading
      return 1;
    case 0:
      // low reading
      return 0;
    default:
      // invalid reading
      return -1;
  }
}

// Generates a 250 ms active-high pulse, used to press the outlet's trigger.
void powerPulse(int powerPin) {
  digitalWrite(powerPin, HIGH);
  delay(250);
  digitalWrite(powerPin, LOW);
}

// Non-blocking motor service, called every loop() pass. Returns immediately
// while idle; while running it settles the duty bucket and makes two
// rollover-safe checks: a full bucket force-stops the motor and arms the
// cooldown, and otherwise a passed pulse deadline stops it normally.
void serviceMotor() {
  if (motorDirection == motorIdle) {
    return;
  }
  unsigned long now = millis();
  updateMotorDutyAccumulator(now);
  if (motorDutyUsedMs >= MOTOR_DUTY_BUDGET_MS) {
    // run budget spent (also caps a single run at 1 min) -> stop + cooldown
    stopMotor(now);
    motorLockedOut = true;
    motorLockoutUntil = now + MOTOR_LOCKOUT_MS;
  } else if ((long)(now - motorStopAt) >= 0) {
    // normal end of pulse: MOTOR_PULSE_MS elapsed since the last run command
    stopMotor(now);
  }
}

// Settles the duty-cycle leaky bucket up to "now": running fills it 1 ms per
// elapsed ms, resting drains 1 ms per MOTOR_DUTY_REST_RATIO ms. Draining
// advances the accounted timestamp only by whole drained units, so the
// remainder survives however often this is called. All arithmetic is unsigned
// millis() differences, so it is rollover-safe.
void updateMotorDutyAccumulator(unsigned long now) {
  unsigned long elapsed = now - motorDutyLastUpdate;
  if (motorDirection != motorIdle) {
    // running: every elapsed millisecond fills the bucket
    motorDutyUsedMs += elapsed;
    motorDutyLastUpdate = now;
  } else {
    // resting: drain the bucket at the configured ratio
    unsigned long drained = elapsed / MOTOR_DUTY_REST_RATIO;
    if (drained >= motorDutyUsedMs) {
      motorDutyUsedMs = 0;
      motorDutyLastUpdate = now; // bucket empty: nothing left to meter
    } else {
      motorDutyUsedMs -= drained;
      motorDutyLastUpdate += drained * MOTOR_DUTY_REST_RATIO; // keep remainder
    }
  }
}

// Drives both direction lines low and records which direction just ran and when
// it stopped, for the settle rule. The duty bucket is settled FIRST, while the
// direction still reads as running, so the last slice of run time is counted
// before draining begins. Safe to call when already idle.
void stopMotor(unsigned long now) {
  updateMotorDutyAccumulator(now);
  digitalWrite(MOTOR_FWD_PIN, LOW);
  digitalWrite(MOTOR_REV_PIN, LOW);
  if (motorDirection != motorIdle) {
    motorLastDirection = motorDirection;
    motorLastStop = now;
  }
  motorDirection = motorIdle;
}

// Handles a forward/reverse run command and sends the reply, enforcing in
// order:
//   1. Cooldown lockout: after filling the run budget, run commands answer
//      "BHE: COOLDOWN" until MOTOR_LOCKOUT_MS has elapsed.
//   2. Duty budget: the bucket must have room for one minimum pulse to start or
//      extend a run, otherwise "BHE: COOLDOWN" while it drains naturally (about
//      MOTOR_DUTY_REST_RATIO seconds of rest buy one second of run time).
//   3. Extension: the same direction repeated while running pushes the stop
//      deadline out to now + MOTOR_PULSE_MS. Total run time stays bounded by
//      the bucket, which serviceMotor() enforces as the run progresses.
//   4. Opposite direction while running: stopped immediately (never driven both
//      ways) and answered "BHE: WAIT"; the settle timer then blocks the
//      reversal until MOTOR_SETTLE_MS of rest.
//   5. Direction settle: from idle, starting opposite to the previous run within
//      MOTOR_SETTLE_MS of its stop answers "BHE: WAIT". The same direction needs
//      no settle.
//   6. Start: the requested line goes high for MOTOR_PULSE_MS, ended later by
//      serviceMotor(). The other line is already low, since the motor only
//      starts from idle and every path to idle drives both lines low, so the two
//      MOSFET gates can never be high together.
void handleMotorRunCommand(uint8_t requestedDirection) {
  unsigned long now = millis();
  // cooldown lockout check (flag + deadline, so rollover cannot re-lock)
  if (motorLockedOut) {
    if ((long)(now - motorLockoutUntil) < 0) {
      sendResponse(motorCoolMessage);
      return;
    }
    motorLockedOut = false; // cooldown has elapsed
  }
  // settle the duty bucket, then require headroom for one minimum pulse
  updateMotorDutyAccumulator(now);
  if ((motorDutyUsedMs + MOTOR_PULSE_MS) > MOTOR_DUTY_BUDGET_MS) {
    sendResponse(motorCoolMessage);
    return;
  }
  if (motorDirection != motorIdle) {
    if (motorDirection == requestedDirection) {
      // same direction repeated -> extend the pulse (velocity control)
      motorStopAt = now + MOTOR_PULSE_MS;
      sendResponse(okMessage);
    } else {
      // opposite direction while running -> stop now, settle before reversing
      stopMotor(now);
      sendResponse(motorWaitMessage);
    }
    return;
  }
  // idle: block a direction REVERSAL until the settle time has passed
  if ((motorLastDirection != motorIdle) &&
      (motorLastDirection != requestedDirection) &&
      ((now - motorLastStop) < MOTOR_SETTLE_MS)) {
    sendResponse(motorWaitMessage);
    return;
  }
  // Start for the minimum pulse length. The reply goes first, matching the
  // strip commands, so the hub is answered before any motor switching noise can
  // appear on the link.
  sendResponse(okMessage);
  motorDirection = requestedDirection;
  motorStopAt = now + MOTOR_PULSE_MS;
  motorDutyLastUpdate = now; // meter run time from this instant (drop any drain remainder)
  digitalWrite((requestedDirection == motorFwd) ? MOTOR_FWD_PIN : MOTOR_REV_PIN, HIGH);
}

// Reports whether the strip is emitting any light, returning true as soon as a
// non-black pixel is found so a lit strip costs almost nothing to detect.
// Brightness scaling is applied as colors are stored, so this reflects what the
// strip ACTUALLY displays: a running or paused effect reads as lit, a cleared
// strip or a 0,0,0 fill as blank. Brightness alone cannot blank the strip, since
// the level maps onto MIN_BRIGHTNESS..MAX_BRIGHTNESS, though a very dark color
// at the dimmest level can still scale to black.
bool isStripLit() {
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    if (strip.getPixelColor(_idx) != 0) {
      return true;
    }
  }
  return false;
}

// Reports the linear 0-255 level last commanded on register 16 - exactly what
// the UI slider was set to. The stored level is returned rather than
// back-calculated, because the perceptual curve in setBrightnessLevel()
// collapses many levels onto the same hardware value at the dim end.
uint16_t showLedsBrightness() {
  return ledBrightnessLevel;
}

// Turns the whole strip off. Brightness is already whatever
// setBrightnessLevel() last applied, so it is not re-sent here.
void clearStrip() {
  strip.clear();
  strip.show();
}

// Fills every pixel with the staged red/green/blue color. Holds no state, so
// calling it twice with the same channels simply paints the same color again.
void fillStripWithStagedColor() {
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    strip.setPixelColor(_idx, stagedRed, stagedGreen, stagedBlue);
  }
  strip.show();
}

// Sets the display brightness from a 0-255 level, mapped perceptually onto the
// MIN_BRIGHTNESS..MAX_BRIGHTNESS window. The eye's response to light is roughly
// logarithmic, so a linear map spends most of its visible change in the bottom
// few slider steps. Squaring the level first (hw = MIN + span * (level/255)^2)
// is the inverse shape, so equal level steps read as equal brightness steps.
// Level 0 still gives MIN_BRIGHTNESS and 255 still gives MAX_BRIGHTNESS; only
// the path between them changes. Staying above 0 also avoids
// setBrightness(0), which rescales the pixel buffer to all-zero and discards
// the stored colors. The raw level is kept for showLedsBrightness(), since the
// curve rounds too coarsely to invert. Applies at once by rescaling the current
// display; a running effect picks it up on its next frame. Called after the
// reply is sent, so the show() here still respects reply-before-show.
void setBrightnessLevel(uint8_t level) {
  ledBrightnessLevel = level; // remembered linearly for the UI read-back
  uint16_t brightnessSpan = MAX_BRIGHTNESS - MIN_BRIGHTNESS;
  // (level^2 * span) / 255^2, rounded to nearest; peaks below 8M so it fits
  // comfortably in the uint32_t intermediate
  uint32_t curvedOffset =
      ((uint32_t)level * level * brightnessSpan + (65025UL / 2)) / 65025UL;
  ledBrightness = (uint8_t)(MIN_BRIGHTNESS + curvedOffset);
  strip.setBrightness(ledBrightness);
  strip.show();
}

// Sends a delimiter-framed ("|message|\n") reply to the hub. print() only
// queues bytes into the transmit buffer, so the reply is flushed before
// returning: callers that update the strip immediately afterwards depend on it
// already being on the wire before show() turns interrupts off.
// HardwareSerial::flush() drains the TRANSMIT buffer and, unlike
// SoftwareSerial's, leaves the receive buffer alone, so a command arriving
// during the reply is still queued. Reception continues on interrupt
// throughout, so nothing is lost while this waits.
void sendResponse(const char* message) {
  Serial.print(respDelimiter);
  Serial.print(message);
  Serial.print(respDelimiter);
  Serial.print('\n');
  Serial.flush();
}
