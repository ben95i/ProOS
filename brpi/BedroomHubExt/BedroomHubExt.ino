/*
 * -----------------------------------------------------------
 * Bedroom Hub External - companion node for AutomateHub #1
 * Runs on an Arduino wired to the hub's external serial port #2.
 * Answers register 11 / command 1 with a "Hello World" message and
 * adds WS2812 LED control, using the same framed serial protocol
 * as AutomateHub_1.
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
 * -----------------------------------------------------------
 *
 * Protocol recap:
 *   Incoming (from hub):  <9,9,RRDDD>   e.g. <9,9,11001> = register 11, command 001
 *   Outgoing (to hub):    |message|     e.g. |Hello World|
 *
 * Command map (all registers 11-20 are routed to this node by the hub):
 *   Reg 11: (control) - LED/effect/power commands, plus chair motor:
 *           041 fwd, 042 rev, 043 stop, 044 status (0/1/2)
 *   Reg 12: stage RED   (0-255)
 *   Reg 13: stage GREEN (0-255)
 *   Reg 14: stage BLUE  (0-255)
 *   Reg 15: stage pixel INDEX (0 .. NUM_LEDS-1)
 *   Reg 16: set BRIGHTNESS level (0-255, mapped perceptually onto
 *           MIN_BRIGHTNESS..MAX_BRIGHTNESS - equal level steps look like
 *           equal brightness steps; level 0 is the dimmest setting, never
 *           fully off. Command 011009 echoes the level back linearly.)
 *   Commands reply with the OK / hello text, a status digit (|1|/|0|), or
 *   the error text (unrecognized / out of range / invalid sense reading).
 *
 * Effects run in the background but only ever call strip.show() while the
 * link is idle (no message arriving/queued), so they don't corrupt incoming
 * commands - the idle check is made once more after the frame has been built
 * and before it is written, so a byte that arrives while a frame is being
 * assembled defers the write instead of being clipped by it. Every effect
 * draws on one shared clock,
 * EFFECT_INTERVAL_MS, which sets only the frame RATE (smoothness); each effect
 * then has its own <NAME>_STEP constant that sets its SPEED independently of
 * that clock. See the effect tuning notes for the valid range of each.
 *
 * Effect frames are held off after link traffic, but for how long depends on
 * whether a retry is expected: a command that decoded and was answered will
 * not be retried, so effects resume after EFFECT_RESUME_DELAY_MS; incomplete
 * or corrupted traffic keeps the full EFFECT_RX_HOLDOFF_MS silence so the
 * master's retry lands on a clean link. EFFECT_RESUME_DELAY_MS is sized to
 * span the gap between rapid-fire commands, so a burst keeps pushing the
 * deadline out and the animation stays paused until the burst finishes.
 */

#include <Adafruit_NeoPixel.h>

// --- WS2812 LED strip ---
#define LED_PIN 11      // WS2812 data pin
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
// How often ANY running effect redraws the strip. This is the SMOOTHNESS knob
// only: each effect's speed is set separately below and is metered against
// real elapsed time, so changing this interval does not change how fast
// anything moves. Going lower draws more finely but spends more time with
// interrupts off (a 60-pixel strip.show() blanks them for roughly 1.9 ms, so
// 30 ms spends about 6% of the time unable to receive a link byte); going
// higher makes each frame's jump larger and coarser.
// Valid: 10..1000. Useful: 20..40.
// Do NOT raise this to slow an effect down - that keeps the per-frame jump
// just as large and only shows it less often, which is exactly what reads as
// flicker/stepping. Lower that effect's step constant instead.
const uint16_t EFFECT_INTERVAL_MS = 30;

// --- per-effect speed ---
// Each effect advances by its own step every EFFECT_INTERVAL_MS. Because the
// advance is metered from elapsed milliseconds rather than applied whole per
// frame, the step is a true rate: a frame delayed or skipped by link traffic
// changes how finely the animation is drawn, never how fast it travels.
//
// Hue effects - step is in HUE UNITS per frame (65536 hue units = full wheel):
//   seconds per wheel = 65536 * EFFECT_INTERVAL_MS / (step * 1000)
//   Valid: 1..65535. Useful: 20..800 - at a 30 ms interval that spans about
//   98 s down to 2.5 s per wheel. Past ~800 the per-frame color jump starts
//   to become visible as stepping.
const uint16_t RAINBOW_HUE_STEP  = 100; // 200 = ~9.8 s per full wheel at 30 ms
const uint16_t CYCLE_HUE_STEP    = 65;  // 150 = ~13.1 s per full wheel at 30 ms

// Pattern effects - step is in HUNDREDTHS OF A PIXEL per frame (100 = 1 px):
//   pixels per second = step * 10 / EFFECT_INTERVAL_MS
//   Valid: 1..65535. Useful: 20..300 - at a 30 ms interval that spans about
//   6.7 to 100 pixels per second. Sub-pixel steps are what let a pattern
//   crawl slowly on the shared 30 ms clock instead of needing its own longer
//   frame interval; values above 100 skip pixels and look choppy.
const uint16_t CHASE_PIXEL_STEP  = 60; // 60 = 0.60 px/frame = 20 px/s at 30 ms
const uint16_t COMET_PIXEL_STEP  = 80; // 100 = 1.00 px/frame = 33 px/s at 30 ms

// Ceiling on how much elapsed time a single frame may absorb, in frame
// intervals. Small gaps (a frame or two lost to link traffic) are caught up
// smoothly; a long gap - the effect holdoff during a command burst - is
// clamped, so the animation resumes where it paused instead of jumping.
// Valid: 1..8 (1 = never catch up; the animation simply loses that time).
const uint8_t EFFECT_CATCHUP_FRAMES = 3;

// Gamma correction makes mid-range colors look right, but it also compresses
// the low end so hard that a channel crossing the bottom of its range can pop
// between output values on a slow fade. Set to 0 for a linear (flatter but
// perfectly even) ramp if that stepping is more objectionable than the gamma.
#define EFFECT_USE_GAMMA 1

// Pattern shape for the two lit-pattern effects.
const uint8_t CHASE_SPACING   = 3; // theater chase lights every Nth pixel
const uint8_t COMET_TAIL_LEN  = 5; // pixels in the comet's fading tail (head included)
#if NUM_LEDS < 2
#error "NUM_LEDS must be at least 2 (the comet sweep needs two end points)"
#endif

// The step constants above, converted to phase units per millisecond in
// 1/4096ths ( fixed point) and rounded to nearest. All four are folded by
// the compiler. The sub-unit precision is what lets a slow animation advance
// a fraction of a step per frame instead of rounding down to nothing; a
// uint16_t step can never make one clamped frame's advance
// (rate * EFFECT_INTERVAL_MS * EFFECT_CATCHUP_FRAMES) approach the 32-bit
// accumulator's limit, so no range guard is needed on any value.
#define HUE_RATE(stepPerFrame) \
  ((((uint32_t)(stepPerFrame) << 12) + (EFFECT_INTERVAL_MS / 2)) / EFFECT_INTERVAL_MS)
#define PIXEL_RATE_(stepPerFrame) \
  ((((uint32_t)(stepPerFrame) << 12) + (50UL * EFFECT_INTERVAL_MS)) / (100UL * EFFECT_INTERVAL_MS))

// One full period of each effect's phase, in the same  units. The phase is
// wrapped to this after every advance, so the accumulator can neither drift
// nor overflow however long an effect runs.
#define HUE_PERIOD   (65536UL << 12)                          // one color wheel
#define CHASE_PERIOD ((uint32_t)CHASE_SPACING << 12)          // one chase group
#define COMET_PERIOD ((uint32_t)(2 * (NUM_LEDS - 1)) << 12)   // one there-and-back sweep

// --- link to the hub (hardware USART) ---
// The board's built-in serial port: pin 0 = RX (from hub ExtSerial2 TX),
// pin 1 = TX (to hub ExtSerial2 RX). No port object is declared - the core
// provides "Serial" - and pins 10/11 are now free. Note that on an Uno this
// port is shared with the USB/bootloader connection, so the hub link must be
// disconnected while programming the board.
#define serialBaudRate 9600

// --- link activity LED ---
#define LINK_LED_PIN 13

// --- power control I/O (from AutomateHub_1) ---
#define PWR_TRIG_1 9  // TRIGGER TIP - pulses to switch the attached device
#define PWR_SENS_1 10 // SENSE RING  - reads the attached device's power state

// --- chair motor control I/O ---
// Two active-high inputs on the existing motor driver: 5V runs the motor in
// that direction, 0V stops it. The state machine guarantees the two pins are
// never high at the same time.
#define MOTOR_FWD_PIN 8 // 5V = motor runs forward
#define MOTOR_REV_PIN 7 // 5V = motor runs in reverse

// --- chair motor tuning constants ---
// A motor command starts a pulse of MOTOR_PULSE_MS (the minimum pulse length).
// Repeating the SAME direction command while the motor is running pushes the
// stop deadline back out to "now + MOTOR_PULSE_MS", so a rapid burst of
// commands keeps the motor moving and the run length tracks how long the
// commands keep arriving (velocity control); the motor always coasts to a stop
// MOTOR_PULSE_MS after the last command. After any stop, the motor must sit
// idle for MOTOR_SETTLE_MS before it may start in the OPPOSITE direction, so
// fwd<->rev transitions always pass through 2 s of rest.
//
// Run time is metered by a leaky bucket: every millisecond the motor RUNS
// fills the bucket by 1 ms, and every MOTOR_DUTY_REST_RATIO milliseconds the
// motor rests drains it by 1 ms, limiting the sustained duty cycle to about
// 1/(MOTOR_DUTY_REST_RATIO+1) - a ratio of 9 allows ~10%, a common rating for
// chair/linear actuators (raise the ratio for a lower-duty motor). Because the
// bucket fills as fast as the motor runs, its size is ALSO the longest
// possible single run: a continuous run fills it in at most
// MOTOR_DUTY_BUDGET_MS (1 minute), so no separate run timer is needed. A run
// command is refused (BHE: COOLDOWN) unless the bucket has room for at least
// one minimum pulse, and if the bucket fills mid-run the motor is force-
// stopped and all run commands are locked out for MOTOR_LOCKOUT_MS (cooldown).
const uint16_t MOTOR_PULSE_MS       = 1200;    // minimum pulse / per-command run time
const uint16_t MOTOR_SETTLE_MS      = 1500;    // required off-time before reversing direction
const unsigned long MOTOR_DUTY_BUDGET_MS = 60000UL; // bucket size: max total AND max single run (1 minute)
const uint8_t MOTOR_DUTY_REST_RATIO = 9;            // ms of rest that drain 1 ms of run time
const unsigned long MOTOR_LOCKOUT_MS  = 300000UL;   // cooldown after filling the bucket (5 minutes)

// --- chair motor state (all transitions handled non-blocking in serviceMotor) ---
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
const uint8_t regStageIndex        = 15; // stage target pixel index
const uint8_t regBrightness        = 16; // set overall brightness

// --- control-register (11) command values ---
const uint16_t ctrlLedsOff         = 0; // clear the strip (stops any effect)
const uint16_t ctrlAbout           = 1; // reply with the hello message
const uint16_t ctrlFillAll         = 2; // fill entire strip with staged color (stops effect)
const uint16_t ctrlSetPixel        = 3; // set one pixel (staged index) to staged color (stops effect)
const uint16_t ctrlRainbow         = 4; // start the rainbow effect
const uint16_t ctrlCycle           = 5; // start the color-cycle effect
const uint16_t ctrlPause           = 6; // pause the running effect (freeze, do not clear)
const uint16_t ctrlSweep           = 7; // start the comet sweep effect
const uint16_t ctrlStripStatus     = 8; // report whether the strip is lit or blank
const uint16_t ctrlStripBrightness = 9; // show current brightness level
const uint16_t ctrlChase           = 10; // start the theater-chase effect
const uint16_t ctrl1PowerOn        = 31; // switch device #1 on (if currently off)
const uint16_t ctrl1PowerOff       = 32; // switch device #1 off (if currently on)
const uint16_t ctrl1PowerStatus    = 33; // report device #1's current power state
const uint16_t ctrl1PowerToggle    = 34; // toggle device #1's power state
const uint16_t ctrlMotorFwd        = 41; // run the chair motor forward (repeat to extend)
const uint16_t ctrlMotorRev        = 42; // run the chair motor in reverse (repeat to extend)
const uint16_t ctrlMotorStop       = 43; // stop the chair motor immediately (always allowed)
const uint16_t ctrlMotorStatus     = 44; // report motor state (0=idle, 1=fwd, 2=rev)

// --- reply strings ---
const char aboutMessage[]      = "BHE: by Ben Provenzano III";
const char okMessage[]         = "BHE: OK";
const char errorMessage[]      = "BHE: ERROR!"; // unrecognized register/command or out-of-range value
const char motorWaitMessage[]  = "BHE: WAIT";    // motor must settle before reversing
const char motorCoolMessage[]  = "BHE: COOLDOWN"; // motor commands locked out (cooldown)
const char powerStateOff[]     = "0"; // status reply: device is off
const char powerStateOn[]      = "1"; // status reply: device is on
const char motorStateRev[]     = "2"; // motor status reply: running in reverse

// --- staged LED state (loaded across commands, then applied) ---
uint8_t stagedRed = 0;
uint8_t stagedGreen = 0;
uint8_t stagedBlue = 0;
uint16_t stagedIndex = 0;
uint8_t ledBrightness = MAX_BRIGHTNESS; // scaled brightness actually sent to the strip
uint8_t ledBrightnessLevel = 255;       // last 0-255 level commanded on register 16, kept
                                        // linear so the UI slider can be read straight
                                        // back (the perceptual curve is one-way lossy)

// --- effect state (background animations) ---
const uint8_t effectNone    = 0; // no animation running
const uint8_t effectRainbow = 1; // rainbow hues trailing up the strip
const uint8_t effectCycle   = 2; // whole strip cycling through hues
const uint8_t effectChase   = 3; // theater-chase marquee in the staged color
const uint8_t effectSweep   = 4; // comet with a fading tail sweeping back and forth
uint8_t effectMode = effectNone;
bool effectPaused = false;
// How far the running effect is through its animation, in 1/4096ths () of
// one phase unit - a hue unit for the two hue effects, a pixel for the two
// pattern effects. Only one effect is ever active, so they share this single
// accumulator instead of keeping four separate positions; startEffect() zeroes
// it so an effect always begins at its own origin, and serviceEffects() wraps
// it to that effect's period so it can never drift or overflow.
uint32_t effectPhase = 0;
unsigned long lastEffectUpdate = 0; // millis() of the last frame actually SHOWN

// --- link recovery tuning ---
// Effect frames stay suppressed until effectHoldoffUntil. Raw link traffic
// pushes that deadline out by EFFECT_RX_HOLDOFF_MS, so a command clipped by a
// strip write silences the animation and the master's retry lands on a clean,
// effect-free link. A frame that was decoded AND answered will not be retried,
// so it only holds off for EFFECT_RESUME_DELAY_MS: long enough to bridge the
// gap between rapid-fire commands (a back-to-back ztermcom invocation lands
// roughly 20-50 ms after the previous reply) so a burst keeps pushing the
// deadline out and the animation stays paused until the burst finishes, yet
// short enough that an effect appears to start immediately. This value is also
// how long an effect takes to visibly start, so lower it for a snappier start
// and raise it if a burst is ever spaced further apart than this.
const uint16_t EFFECT_RX_HOLDOFF_MS = 1000;
const uint16_t EFFECT_RESUME_DELAY_MS = 250;
// A partial frame idle this long lost its terminator to a corrupted byte (a
// whole command takes ~11 ms at 9600 baud), so it is abandoned rather than
// allowed to absorb the retry that follows.
const uint16_t FRAME_ABANDON_MS = 100;
// The master sends one terminator byte straight after each frame's '>'. When
// the master is the AutomateHub, that byte is bit-banged by SoftwareSerial,
// which disables interrupts for its whole duration (~1 ms at 9600 baud) and is
// therefore DEAF until it finishes. This port receives that byte in hardware,
// in the background, so a reply sent the instant the '>' decodes would begin
// inside the hub's deaf window and lose its leading '|' - the hub then never
// sees a response start and times out. The reply is therefore held until the
// terminator has actually landed, or this long if a master ever sends a frame
// without one (a direct connection to the Pi, for example).
const uint16_t REPLY_GUARD_MS = 5;
unsigned long lastRxActivity = 0;     // millis() of the last received byte
unsigned long effectHoldoffUntil = 0; // effects stay suppressed until this millis()
// The hub appends one terminator byte after every frame's '>'. It arrives while
// the reply is being sent, so it must not be mistaken for fresh traffic and
// re-arm the long holdoff - that is what delayed every effect start by a second.
bool expectTrailingByte = false;

// --- receive buffer and reader state ---
char serialMessageIn[maxMessage + 1];
uint16_t serialCurPos = 0;
bool serialReading = 0;
bool serialMsgEnd = 0;

///////////////////////////////////////

// Arduino entry point: starts the hardware serial link to the hub,
// configures the power trigger/sense pins, initializes the LED strip to
// all-off, and clears the receive buffer. The hardware port receives on an
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
  // chair motor I/O: both direction lines low (motor off) before and after
  // the pins become outputs, so the motor can never twitch at boot
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

// Main loop: services the hub link first (so incoming bytes are always
// handled promptly), then the chair motor deadlines (cheap millis()
// comparisons only, so a running motor never delays the link), then any
// running LED effect - but only while the link is idle, so an animation
// frame can never overlap an incoming command.
void loop() {
  serviceSerial();
  serviceMotor();
  serviceEffects();
}

// Reads and processes framed commands from the hub link: buffers an
// incoming message one character at a time, and once a full frame has
// arrived, decodes and acts on it. Every received byte stamps lastRxActivity
// and arms the long effect holdoff (a retry may be coming), except the single
// terminator byte that trails an answered frame. A partial frame that stalls
// for FRAME_ABANDON_MS - its terminator was lost to a corrupted byte - is
// abandoned so a retry decodes cleanly instead of being appended to stale data.
void serviceSerial() {
  if (serialMsgEnd == 0) {
    // still collecting a message
    if (Serial.available()) {
      char incomingChar = Serial.read();
      lastRxActivity = millis();
      // The one byte following an answered frame is the hub's terminator:
      // expected protocol noise, so it must not arm the long holdoff. A frame
      // start is never noise, and everything else may still need retrying.
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
    // Waits for the master's post-frame terminator byte to arrive before a reply is
    // sent, giving up after REPLY_GUARD_MS. The byte is only waited FOR, never
    // consumed: it stays in the receive buffer and the existing expectTrailingByte
    // path swallows it on the next service pass, so the effect holdoff behaves
    // exactly as before. Returning as soon as the byte is present means the wait
    // costs nothing on a link that has already delivered it. See REPLY_GUARD_MS for
    // why answering before the terminator loses the reply on a SoftwareSerial
    // master.
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

// Advances the active LED animation by at most one frame per call. Runs only
// when an effect is active, not paused, AND the link is completely idle (no
// message being received and no byte waiting in the RX buffer). This is what
// keeps effects from interfering with commands: strip.show() disables
// interrupts on the Uno, which stalls the USART receive interrupt while a byte
// could be arriving, so a frame is never written while a command could be in
// flight.
//
// The four draw<Effect>Frame() helpers only fill the pixel buffer. Everything
// common to them - the second link check, the strip write, the frame clock and
// the phase advance - lives here, so each of those exists exactly once and an
// effect is added by writing one draw function plus one case below.
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
  // How much animation time this frame is allowed to represent. Frames are
  // never exactly one interval apart (the link is serviced first, and frames
  // are suppressed entirely while it is busy), so effects advance by this
  // measured elapsed time rather than by a fixed step - that is what keeps
  // their speed steady when frames arrive irregularly. The clamp stops a long
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
    case effectChase:
      drawTheaterChaseFrame();
      phaseRate = PIXEL_RATE_(CHASE_PIXEL_STEP);
      phasePeriod = CHASE_PERIOD;
      break;
    case effectSweep:
      drawCometFrame();
      phaseRate = PIXEL_RATE_(COMET_PIXEL_STEP);
      phasePeriod = COMET_PERIOD;
      break;
    default:
      return; // unknown mode: nothing drawn, nothing to show
  }
  // Building a frame takes real time (up to 60 pixels of ColorHSV/gamma), so
  // re-check the link: a byte that arrived meanwhile would be clipped by the
  // interrupts-off strip write. Deferring costs nothing visually - the buffer
  // keeps the finished frame, neither the clock nor the phase moves, and the
  // next idle pass redraws and shows the very same frame.
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
  lastEffectUpdate = millis() - EFFECT_INTERVAL_MS; // draw promptly
}

// Stops any running animation, leaving the strip exactly as it is. Used by the
// static LED commands, which take over the strip themselves right afterwards.
void stopEffect() {
  effectMode = effectNone;
  effectPaused = false;
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

// Builds one frame of the color-cycle effect: the whole strip shows the single
// hue the phase currently sits on, so it walks the wheel as the phase advances.
void drawColorCycleFrame() {
  uint32_t color = effectHueColor((uint16_t)(effectPhase >> 12));
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    strip.setPixelColor(_idx, color);
  }
}

// Returns the color the pattern effects should draw in: the staged
// red/green/blue, falling back to white when nothing has been staged, since a
// black staged color would otherwise make the effect invisible.
uint32_t stagedEffectColor() {
  if ((stagedRed | stagedGreen | stagedBlue) == 0) {
    return strip.Color(255, 255, 255);
  }
  return strip.Color(stagedRed, stagedGreen, stagedBlue);
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

// Builds one frame of the theater-chase effect: every CHASE_SPACING-th pixel
// is lit in the staged color, and the lit group sits wherever the phase points
// within one group, giving the classic marquee crawl. Unlit pixels are
// cleared, so the pattern always draws over whatever was on the strip before.
void drawTheaterChaseFrame() {
  uint32_t chaseColor = stagedEffectColor();
  uint8_t chaseOffset = (uint8_t)(effectPhase >> 12); // phase wraps at CHASE_SPACING
  strip.clear();
  for (uint16_t _idx = chaseOffset; _idx < NUM_LEDS; _idx += CHASE_SPACING) {
    strip.setPixelColor(_idx, chaseColor);
  }
}

// Builds one frame of the comet effect: a bright head in the staged color
// sweeps along the strip and reverses at each end, trailing COMET_TAIL_LEN
// pixels that fade linearly to black. The tail is drawn from the head position
// rather than by fading the previous frame, so repeated brightness rescaling
// cannot make the colors drift.
//
// The phase runs 0 .. 2*(NUM_LEDS-1) and the second half is folded back on
// itself, which produces the there-and-back sweep - and the travel direction
// with it - from the phase alone, with no direction flag or end-of-strip test.
void drawCometFrame() {
  uint32_t baseColor = stagedEffectColor();
  uint16_t sweepPos = (uint16_t)(effectPhase >> 12);
  bool movingForward = (sweepPos < NUM_LEDS);
  int16_t cometHead = movingForward ? (int16_t)sweepPos
                                    : (int16_t)(2 * (NUM_LEDS - 1) - sweepPos);
  strip.clear();
  for (uint8_t _tail = 0; _tail < COMET_TAIL_LEN; _tail++) {
    // tail pixels sit behind the head, so they trail the direction of travel
    int16_t tailPos = movingForward ? (cometHead - _tail) : (cometHead + _tail);
    if (tailPos < 0 || tailPos >= (int16_t)NUM_LEDS) {
      continue;
    }
    // linear fade: full brightness at the head, dark at the end of the tail
    uint8_t fadeScale = (uint8_t)(255 - ((uint16_t)_tail * 255 / COMET_TAIL_LEN));
    strip.setPixelColor(tailPos, scaleColor(baseColor, fadeScale));
  }
}

// Character-by-character framed reader: waits for startInd, buffers
// characters into serialMessageIn (overflow-safe), and sets serialMsgEnd
// when endInd is received. A startInd received MID-frame restarts the
// frame from scratch (the payload never contains '<'): if the previous
// frame's terminator was lost to a corrupted byte, the master's retry
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

// Validates and decodes a received "9,9,RRDDD" message: confirms the
// delimiter count and the leading "9" prefix, then passes the position
// of the numeric payload (after the second delimiter) to processCmdData().
// Malformed input is answered with an error frame so the hub never has
// to wait out its response timeout.
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

// Extracts the fixed-length numeric control code beginning at
// messageStart, splits it into the 2-digit register select and the
// 3-digit command data, and hands the pair to handleCommand().
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

// Dispatches a decoded register/command pair. Register 11 carries the
// hello and LED apply/clear commands; registers 12-16 stage the color
// channels, target index, and brightness; register 17 controls the
// attached device's power. Unknown registers or out-of-range values
// reply with the error frame.
void handleCommand(uint8_t messageRegister, uint16_t messageData) {
  switch (messageRegister) {
    case regControl:
      handleControlCommand(messageData);
      break;
    case regStageRed:
      if (messageData <= 255) {
        stagedRed = (uint8_t)messageData;
        sendResponse(okMessage);
      } else {
        sendResponse(errorMessage);
      }
      break;
    case regStageGreen:
      if (messageData <= 255) {
        stagedGreen = (uint8_t)messageData;
        sendResponse(okMessage);
      } else {
        sendResponse(errorMessage);
      }
      break;
    case regStageBlue:
      if (messageData <= 255) {
        stagedBlue = (uint8_t)messageData;
        sendResponse(okMessage);
      } else {
        sendResponse(errorMessage);
      }
      break;
    case regStageIndex:
      if (messageData < NUM_LEDS) {
        stagedIndex = messageData;
        sendResponse(okMessage);
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

// Handles the control register (11) command values: hello, static LED
// commands (off/fill/set-pixel, which also stop any running effect), the
// background effects, and pause. For the static LED commands
// the reply is sent BEFORE the strip update: on the Uno, Adafruit_NeoPixel's
// show() disables interrupts to bit-bang the strip, which would stall the
// USART transmit interrupt and stretch the gaps between the reply's bytes.
// Sending first keeps the reply clean; the show() then runs during the gap
// before the next command. Effect start and pause only set state - their
// frames are drawn later by serviceEffects() while the link is idle.
void handleControlCommand(uint16_t messageData) {
  switch (messageData) {
    case ctrlAbout:
      sendResponse(aboutMessage);
      break;
    case ctrlLedsOff:
      sendResponse(okMessage);
      stopEffect();
      clearStrip();
      break;
    case ctrlFillAll:
      sendResponse(okMessage);
      stopEffect();
      fillStripWithStagedColor();
      break;
    case ctrlSetPixel:
      sendResponse(okMessage);
      stopEffect();
      setStagedPixelColor();
      break;
    case ctrlRainbow:
      sendResponse(okMessage);
      startEffect(effectRainbow);
      break;
    case ctrlCycle:
      sendResponse(okMessage);
      startEffect(effectCycle);
      break;
    case ctrlChase:
      sendResponse(okMessage);
      startEffect(effectChase);
      break;
    case ctrlSweep:
      sendResponse(okMessage);
      startEffect(effectSweep);
      break;
    case ctrlPause:
      effectPaused = true;
      sendResponse(okMessage);
      break;
    case ctrlStripStatus:
      if (isStripLit()) {
        sendResponse(powerStateOn);
      } else {
        sendResponse(powerStateOff);
      }
      break;
    case ctrlStripBrightness: {
      char brightnessText[4];
      itoa(showLedsBrightness(), brightnessText, 10);
      sendResponse(brightnessText);
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

// Handles the power register (17), mirroring AutomateHub_1's internal
// power logic. Reads the attached device's current state from the sense
// line, then either reports it (status) or pulses the trigger to switch
// it (on/off/toggle).
//   1 = on, 2 = off, 3 = status, 4 = toggle.
// The sense line is inverted (INPUT_PULLUP): a LOW reading means the
// device is on, a HIGH reading means it is off. On/off only pulse when a
// change is actually needed; toggle always pulses. Status replies with
// the on/off digit, or the error frame on an invalid (bouncing) reading.
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

// Samples a digital input pin 16 times back-to-back and returns a
// debounced consensus reading: 1 = all samples high, 0 = all samples low,
// -1 = samples disagreed (bouncing / invalid input).
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

// Generates a 250 ms active-high pulse on the given output pin, used to
// press the power trigger of the attached device.
void powerPulse(int powerPin) {
  digitalWrite(powerPin, HIGH);
  delay(250);
  digitalWrite(powerPin, LOW);
}

// Non-blocking chair motor service, called every loop() pass. While idle it
// returns immediately; while the motor is running it settles the duty bucket
// and does two cheap checks (rollover-safe millis() arithmetic, like the
// effect holdoff): a full bucket - which a continuous run reaches in at most
// MOTOR_DUTY_BUDGET_MS - force-stops the motor and arms the MOTOR_LOCKOUT_MS
// cooldown, and otherwise a passed pulse deadline (MOTOR_PULSE_MS after the
// most recent command) stops the motor normally.
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

// Settles the duty-cycle leaky bucket up to "now". While the motor runs the
// bucket fills 1 ms per elapsed ms; while it rests the bucket drains 1 ms per
// MOTOR_DUTY_REST_RATIO ms. Draining only advances the accounted timestamp by
// whole drained units (drained * ratio), so the sub-ratio remainder is never
// lost to rounding no matter how often this is called. All arithmetic is
// unsigned differences of millis(), so it is rollover-safe.
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

// Drives both motor direction lines low and records the stop, remembering
// which direction just ran (for the direction-settle rule) and when it
// stopped (the settle timer). The duty bucket is settled FIRST, while the
// direction still reads as running, so the final slice of run time is
// counted before draining begins. Safe to call when the motor is already
// idle.
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

// Handles a forward/reverse run command (requestedDirection is motorFwd or
// motorRev) and sends the reply. Enforces, in order:
//   1. Cooldown lockout: after a max-length run, all run commands answer
//      "BHE: COOLDOWN" until MOTOR_LOCKOUT_MS has elapsed.
// Handles a forward/reverse run command (requestedDirection is motorFwd or
// motorRev) and sends the reply. Enforces, in order:
//   1. Cooldown lockout: after filling the run budget, all run commands
//      answer "BHE: COOLDOWN" until MOTOR_LOCKOUT_MS has elapsed.
//   2. Duty budget: the leaky bucket must have room for at least one minimum
//      pulse before a run may start or be extended, otherwise the reply is
//      "BHE: COOLDOWN" and the bucket keeps draining naturally (about
//      MOTOR_DUTY_REST_RATIO seconds of rest buy one second of run time).
//   3. Extension (velocity control): the same direction repeated while
//      running pushes the stop deadline out to now + MOTOR_PULSE_MS; total
//      run time is bounded by the bucket, which serviceMotor enforces as the
//      run progresses.
//   4. Opposite direction while running: the motor is stopped immediately
//      (never driven both ways) and the reply is "BHE: WAIT" - the settle
//      timer then blocks the reversal until MOTOR_SETTLE_MS of rest.
//   5. Direction settle: from idle, starting OPPOSITE to the previous run
//      within MOTOR_SETTLE_MS of its stop answers "BHE: WAIT". Restarting in
//      the SAME direction needs no settle.
//   6. Start: the requested line goes high for a minimum pulse of
//      MOTOR_PULSE_MS, ended later by serviceMotor(). The other line is
//      already low: the motor only starts from idle, and every path to idle
//      (setup and stopMotor) drives both lines low, so the two MOSFET gates
//      can never be high together.
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
  // start the requested direction for the minimum pulse length. The reply is
  // sent first (reply-before-drive, matching the strip commands) so the hub
  // gets its answer before any motor switching noise can appear on the link.
  sendResponse(okMessage);
  motorDirection = requestedDirection;
  motorStopAt = now + MOTOR_PULSE_MS;
  motorDutyLastUpdate = now; // meter run time from this instant (drop any drain remainder)
  digitalWrite((requestedDirection == motorFwd) ? MOTOR_FWD_PIN : MOTOR_REV_PIN, HIGH);
}

// Reports whether the strip is currently emitting any light: scans the
// pixel buffer and returns true as soon as a non-black pixel is found
// (so a lit strip costs almost nothing to detect), false when every pixel
// is black. Adafruit_NeoPixel applies the brightness scaling as colors are
// stored, so this reflects what the strip is ACTUALLY displaying: a running
// or paused effect reads as lit, while a cleared strip or a fill with color
// 0,0,0 reads as blank. Brightness alone can no longer blank the strip,
// since the level maps onto MIN_BRIGHTNESS..MAX_BRIGHTNESS, though a very
// dark color at the dimmest level can still scale down to black.
bool isStripLit() {
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    if (strip.getPixelColor(_idx) != 0) {
      return true;
    }
  }
  return false;
}

// Reports the brightness as the linear 0-255 level last commanded on register
// 16, which is exactly what the UI slider was set to. The stored level is
// returned rather than back-calculated from the hardware brightness: the
// perceptual curve in setBrightnessLevel() collapses many levels onto the same
// hardware value at the dim end, so the hardware value alone can no longer
// recover the level.
uint16_t showLedsBrightness() {
  return ledBrightnessLevel;
}

// Turns the whole strip off. The strip's brightness is already whatever
// setBrightnessLevel() last applied, so it is not re-sent here (nor in the
// two helpers below) - that call was a no-op on every path.
void clearStrip() {
  strip.clear();
  strip.show();
}

// Fills every pixel with the currently staged red/green/blue color.
void fillStripWithStagedColor() {
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    strip.setPixelColor(_idx, stagedRed, stagedGreen, stagedBlue);
  }
  strip.show();
}

// Sets the single staged-index pixel to the staged red/green/blue color,
// leaving the rest of the strip unchanged.
void setStagedPixelColor() {
  strip.setPixelColor(stagedIndex, stagedRed, stagedGreen, stagedBlue);
  strip.show();
}

// Sets the display brightness from a 0-255 level, mapped PERCEPTUALLY onto the
// compile-time MIN_BRIGHTNESS..MAX_BRIGHTNESS window. The eye's response to
// light is roughly logarithmic: a linear map spends most of its visible change
// in the bottom few slider steps and the whole top half looks the same. The
// level is therefore squared before scaling (hw = MIN + span * (level/255)^2,
// the standard integer approximation of the perceptual curve), which is the
// inverse shape - equal LEVEL steps then read as equal BRIGHTNESS steps.
// Level 0 still gives MIN_BRIGHTNESS (dimmest visible, never fully dark) and
// 255 still gives MAX_BRIGHTNESS; only the path between them changes. Keeping
// the hardware brightness above 0 also avoids Adafruit_NeoPixel's
// setBrightness(0) case, which rescales the pixel buffer to all-zero and
// permanently discards the stored colors. The raw level is stored for
// showLedsBrightness(), since the curve rounds too coarsely to invert. Applies
// immediately by rescaling the current display; a running effect also picks it
// up on its next frame. Called after the reply is sent, so the show() here
// still respects the reply-before-show rule.
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

// Sends a delimiter-framed ("|message|\n") response back to the hub over the
// hardware serial link. HardwareSerial::print() only queues bytes into the
// transmit buffer and returns, so the reply is flushed before returning: the
// callers that update the strip right after replying (reply-before-show)
// depend on the reply already being on the wire before show() turns interrupts
// off. HardwareSerial::flush() waits for the TRANSMIT buffer to drain and,
// unlike SoftwareSerial's, leaves the receive buffer alone, so a command that
// arrives during the reply is still queued and handled. Reception continues on
// interrupt throughout, so nothing is lost while this waits.
void sendResponse(const char* message) {
  Serial.print(respDelimiter);
  Serial.print(message);
  Serial.print(respDelimiter);
  Serial.print('\n');
  Serial.flush();
}
