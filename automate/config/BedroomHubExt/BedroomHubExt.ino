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
 * v4 - 07/26/2026 - strip lit-status query (011008) + fast effect resume
 * v5 - 07/27/2026 - brightness floor, burst-aware effect resume, trailing
 *                   terminator no longer re-arms the long holdoff
 * v6 - 07/31/2026 - theater-chase and comet effects, per-effect speed
 * v7 - 08/01/2026 - adjustable-chair motor control (non-blocking pulses,
 *                   burst extension, run-time limit + cooldown lockout,
 *                   direction settle time)
 * v8 - 08/01/2026 - motor cumulative duty-cycle limiter (leaky-bucket run
 *                   budget) + both-pins-high hardware interlock guard
 * -----------------------------------------------------------
 *
 * Link wiring (SoftwareSerial, through the same RS-232 level shifter
 * the hub uses):
 *   node LINK_RX (pin 10) <-- hub ExtSerial2 TX (hub pin 3)
 *   node LINK_TX (pin 11) --> hub ExtSerial2 RX (hub pin 12)
 *   common ground between the two boards.
 *
 *   Using SoftwareSerial keeps the hardware UART (pins 0/1) free for
 *   USB uploads and the Serial Monitor while the hub link is active.
 *
 * WS2812 wiring:
 *   data -> LED_PIN (default pin 6), plus 5V and ground to the strip.
 *
 * Chair motor wiring (existing driver hardware, 0V = off / 5V = on):
 *   forward input -> MOTOR_FWD_PIN (pin 4)
 *   reverse input -> MOTOR_REV_PIN (pin 12)
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
 *   Reg 16: set BRIGHTNESS level (0-255, mapped onto MIN_BRIGHTNESS..
 *           MAX_BRIGHTNESS; level 0 is the dimmest setting, never fully off)
 *   Commands reply with the OK / hello text, a status digit (|1|/|0|), or
 *   the error text (unrecognized / out of range / invalid sense reading).
 *
 * Effects run in the background but only ever call strip.show() while the
 * link is idle (no message arriving/queued), so they don't corrupt incoming
 * commands. Each effect has its own <NAME>_INTERVAL_MS speed constant.
 *
 * Effect frames are held off after link traffic, but for how long depends on
 * whether a retry is expected: a command that decoded and was answered will
 * not be retried, so effects resume after EFFECT_RESUME_DELAY_MS; incomplete
 * or corrupted traffic keeps the full EFFECT_RX_HOLDOFF_MS silence so the
 * master's retry lands on a clean link. EFFECT_RESUME_DELAY_MS is sized to
 * span the gap between rapid-fire commands, so a burst keeps pushing the
 * deadline out and the animation stays paused until the burst finishes.
 */

#include <SoftwareSerial.h>
#include <Adafruit_NeoPixel.h>

// --- WS2812 LED strip ---
#define LED_PIN 6      // WS2812 data pin (keep clear of the link pins 10/11)
#define NUM_LEDS 60    // number of pixels on the strip (change to match hardware)
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- LED brightness and effect tuning constants ---
#define MAX_BRIGHTNESS 120 // brightness ceiling (0-255); a level of 255 maps here
#define MIN_BRIGHTNESS 1   // brightness floor (1-254); a level of 0 maps here, so the
                           // strip is never driven at an actual brightness of 0
#if MIN_BRIGHTNESS >= MAX_BRIGHTNESS
#error "MIN_BRIGHTNESS must be less than MAX_BRIGHTNESS"
#endif
#if MIN_BRIGHTNESS < 1
#error "MIN_BRIGHTNESS must be at least 1 (a value of 0 would blank the strip)"
#endif
// --- per-effect speed (ms between animation frames; lower = faster) ---
// Each effect has its own frame interval so their speeds are set independently.
// Going too low raises the odds of a strip write clipping an incoming command.
const uint16_t RAINBOW_INTERVAL_MS = 65; // rainbow scroll frame interval
const uint16_t CYCLE_INTERVAL_MS   = 85; // color-cycle frame interval
const uint16_t CHASE_INTERVAL_MS   = 50; // theater-chase frame interval
const uint16_t COMET_INTERVAL_MS   = 30; // comet sweep frame interval
// How far each hue animation advances per frame: with the interval above this
// is the second half of "speed", and it also sets how smooth the sweep looks.
const uint16_t RAINBOW_HUE_STEP = 256;  // hue advance per frame for the rainbow scroll
const uint16_t CYCLE_HUE_STEP   = 256;  // hue advance per frame for the color cycle
// Pattern shape for the two lit-pattern effects.
const uint8_t CHASE_SPACING   = 3; // theater chase lights every Nth pixel
const uint8_t COMET_TAIL_LEN  = 5; // pixels in the comet's fading tail (head included)

// --- link to the hub (SoftwareSerial) ---
#define serialBaudRate 9600
#define LINK_RX_PIN 10 // receive from hub ExtSerial2 TX
#define LINK_TX_PIN 11 // transmit to hub ExtSerial2 RX
SoftwareSerial LinkSerial(LINK_RX_PIN, LINK_TX_PIN);

// --- power control I/O (from AutomateHub_1) ---
#define PWR_TRIG_1 9 // TRIGGER TIP - pulses to switch the attached device
#define PWR_SENS_1 2 // SENSE RING  - reads the attached device's power state

// --- chair motor control I/O ---
// Two active-high inputs on the existing motor driver: 5V runs the motor in
// that direction, 0V stops it. The state machine guarantees the two pins are
// never high at the same time.
#define MOTOR_FWD_PIN 4  // 5V = motor runs forward
#define MOTOR_REV_PIN 12 // 5V = motor runs in reverse

// --- chair motor tuning constants ---
// A motor command starts a pulse of MOTOR_PULSE_MS (the minimum pulse length).
// Repeating the SAME direction command while the motor is running pushes the
// stop deadline back out to "now + MOTOR_PULSE_MS", so a rapid burst of
// commands keeps the motor moving and the run length tracks how long the
// commands keep arriving (velocity control); the motor always coasts to a stop
// MOTOR_PULSE_MS after the last command. A single run is hard-capped at
// MOTOR_MAX_RUN_MS: hitting the cap force-stops the motor and locks out all
// motor run commands for MOTOR_LOCKOUT_MS (cooldown). After any stop, the
// motor must sit idle for MOTOR_SETTLE_MS before it may start in the OPPOSITE
// direction, so fwd<->rev transitions always pass through 2 s of rest.
const uint16_t MOTOR_PULSE_MS       = 1000;    // minimum pulse / per-command run time
const unsigned long MOTOR_MAX_RUN_MS  = 45000UL;  // longest single run (45 seconds)
const unsigned long MOTOR_LOCKOUT_MS  = 300000UL; // cooldown after a max-length run (5 minutes)
const uint16_t MOTOR_SETTLE_MS      = 2000;    // required off-time before reversing direction

// --- chair motor cumulative duty-cycle limiter (leaky bucket) ---
// The single-run cap above cannot stop many short runs back-to-back from
// overheating the actuator, so total run time is also metered: every
// millisecond the motor RUNS fills the bucket by 1 ms, and every
// MOTOR_DUTY_REST_RATIO milliseconds the motor rests drains it by 1 ms. The
// sustained duty cycle is therefore limited to about
// 1/(MOTOR_DUTY_REST_RATIO+1) - a ratio of 9 allows ~10%, a common rating for
// chair/linear actuators (raise the ratio for a lower-duty motor). A run
// command is refused (BHE: COOLDOWN) unless the bucket has room for at least
// one minimum pulse, and if the bucket fills mid-run the motor is force-
// stopped and the 5-minute lockout is armed, just like the single-run cap.
const unsigned long MOTOR_DUTY_BUDGET_MS = 60000UL; // bucket size: total run time it can hold
const uint8_t MOTOR_DUTY_REST_RATIO = 9;            // ms of rest that drain 1 ms of run time

// --- chair motor state (all transitions handled non-blocking in serviceMotor) ---
const uint8_t motorIdle = 0; // also used as "no previous direction" at boot
const uint8_t motorFwd  = 1;
const uint8_t motorRev  = 2;
uint8_t motorDirection = motorIdle;     // direction currently driven (idle = pins low)
uint8_t motorLastDirection = motorIdle; // direction of the previous run (for the settle rule)
unsigned long motorRunStart = 0;        // millis() when the current run began
unsigned long motorStopAt = 0;          // millis() deadline when the current run ends
unsigned long motorLastStop = 0;        // millis() when the motor last stopped (settle timer)
unsigned long motorLockoutUntil = 0;    // millis() deadline when the cooldown lockout ends
bool motorLockedOut = false;            // true while the cooldown lockout is active
unsigned long motorDutyUsedMs = 0;      // leaky bucket: accumulated run time not yet drained
unsigned long motorDutyLastUpdate = 0;  // millis() up to which the bucket has been accounted

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
const uint16_t ctrlPause           = 6; // pause effects (freeze, do not clear)
const uint16_t ctrlResume          = 7; // resume effects
const uint16_t ctrlStripStatus     = 8; // report whether the strip is lit or blank
const uint16_t ctrlStripBrightness = 9; // show current brightness level
const uint16_t ctrlChase           = 10; // start the theater-chase effect
const uint16_t ctrlComet           = 11; // start the comet sweep effect

const uint16_t ctrl1PowerOn        = 31; // switch device #1 on (if currently off)
const uint16_t ctrl1PowerOff       = 32; // switch device #1 off (if currently on)
const uint16_t ctrl1PowerStatus    = 33; // report device #1's current power state
const uint16_t ctrl1PowerToggle    = 34; // toggle device #1's power state

const uint16_t ctrlMotorFwd        = 41; // run the chair motor forward (repeat to extend)
const uint16_t ctrlMotorRev        = 42; // run the chair motor in reverse (repeat to extend)
const uint16_t ctrlMotorStop       = 43; // stop the chair motor immediately (always allowed)
const uint16_t ctrlMotorStatus     = 44; // report motor state (0=idle, 1=fwd, 2=rev)

// --- reply strings ---
const char aboutMessage[]  = "BHE: by Ben Provenzano III";
const char okMessage[]     = "BHE: OK";
const char errorMessage[]  = "BHE: ERROR!"; // unrecognized register/command or out-of-range value
const char powerStateOn[]  = "1"; // status reply: device is on
const char powerStateOff[]    = "0"; // status reply: device is off
const char motorStateRev[]     = "2";            // motor status reply: running in reverse
const char motorWaitMessage[]  = "BHE: WAIT";    // motor must settle before reversing
const char motorCoolMessage[]  = "BHE: COOLDOWN"; // motor commands locked out (cooldown)

// --- staged LED state (loaded across commands, then applied) ---
uint8_t stagedRed = 0;
uint8_t stagedGreen = 0;
uint8_t stagedBlue = 0;
uint16_t stagedIndex = 0;
uint8_t ledBrightness = MAX_BRIGHTNESS; // scaled brightness actually sent to the strip

// --- effect state (background animations) ---
const uint8_t effectNone    = 0; // no animation running
const uint8_t effectRainbow = 1; // rainbow hues trailing up the strip
const uint8_t effectCycle   = 2; // whole strip cycling through hues
const uint8_t effectChase   = 3; // theater-chase marquee in the staged color
const uint8_t effectComet   = 4; // comet with a fading tail sweeping back and forth
uint8_t effectMode = effectNone;
bool effectPaused = false;
uint16_t rainbowBaseHue = 0;      // scroll offset for the rainbow effect
uint16_t cycleHue = 0;            // current hue for the color-cycle effect
uint8_t chaseOffset = 0;          // which pixel of each group the chase lights
int16_t cometHead = 0;            // current comet head position along the strip
bool cometForward = true;         // comet sweep direction
unsigned long lastEffectUpdate = 0; // millis() timestamp of the last frame

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

// Arduino entry point: starts the link serial port (and makes it the
// active SoftwareSerial listener), configures the power trigger/sense
// pins, initializes the LED strip to all-off, and clears the receive
// buffer.
void setup() {
  LinkSerial.begin(serialBaudRate);
  LinkSerial.listen(); // single SoftwareSerial port -> ensure it is listening
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
  strip.show(); // all pixels off at boot
  serialMessageIn[0] = nullTrm;
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
    if (LinkSerial.available()) {
      char incomingChar = LinkSerial.read();
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
      // mid-frame with no new data: abandon the frame if it has stalled
      if ((millis() - lastRxActivity) > FRAME_ABANDON_MS) {
        serialReading = 0;
        serialCurPos = 0;
        serialMessageIn[0] = nullTrm;
      }
    }
  } else {
    // a full frame has arrived -> decode and act on it
    decodeMessage();
    // the frame was decoded and answered, so the master will not retry it:
    // resume effects after the burst window and ignore the hub's terminator
    expectTrailingByte = true;
    effectHoldoffUntil = millis() + EFFECT_RESUME_DELAY_MS;
    // clear for the next message
    serialMessageIn[0] = nullTrm;
    serialMsgEnd = 0;
  }
}

// Advances the active LED animation by at most one frame per call. Runs
// only when an effect is active, not paused, AND the link is completely
// idle (no message being received and no byte waiting in the RX buffer).
// This is what keeps effects from interfering with commands: strip.show()
// disables interrupts on the Uno, which would corrupt a SoftwareSerial
// byte arriving at that instant, so a frame is never drawn while a
// command could be in flight. currentEffectInterval() paces the frame rate,
// so each effect animates at its own configured speed.
void serviceEffects() {
  // nothing to draw if no effect is running or it is paused (frozen)
  if (effectMode == effectNone || effectPaused) {
    return;
  }
  // never drive the strip while a command is arriving or already queued
  if (serialReading || serialMsgEnd || LinkSerial.available()) {
    return;
  }
  // hold off until the link has settled (see the link recovery tuning notes).
  // The signed cast keeps the comparison correct across millis() rollover.
  unsigned long now = millis();
  if ((long)(now - effectHoldoffUntil) < 0) {
    return;
  }
  // frame-rate gate (this effect's own animation speed)
  if ((now - lastEffectUpdate) < currentEffectInterval()) {
    return;
  }
  lastEffectUpdate = now;
  // draw one frame of the active effect
  switch (effectMode) {
    case effectRainbow:
      renderRainbowFrame();
      break;
    case effectCycle:
      renderColorCycleFrame();
      break;
    case effectChase:
      renderTheaterChaseFrame();
      break;
    case effectComet:
      renderCometFrame();
      break;
  }
}

// Selects an effect and un-pauses it, scheduling the first frame to draw
// on the next idle service pass.
// Returns the frame interval (in ms) of the effect currently selected, which
// is what gives each effect its own independently adjustable speed.
uint16_t currentEffectInterval() {
  switch (effectMode) {
    case effectRainbow:
      return RAINBOW_INTERVAL_MS;
    case effectCycle:
      return CYCLE_INTERVAL_MS;
    case effectChase:
      return CHASE_INTERVAL_MS;
    case effectComet:
      return COMET_INTERVAL_MS;
    default:
      return 0;
  }
}

void startEffect(uint8_t mode) {
  effectMode = mode;
  effectPaused = false;
  lastEffectUpdate = millis() - currentEffectInterval(); // draw promptly
}

// Renders one frame of the rainbow effect: a full hue spread mapped across
// the strip, scrolled by a growing base-hue offset each frame so the colors
// trail up the strip.
void renderRainbowFrame() {
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    uint16_t pixelHue = rainbowBaseHue + (uint16_t)((uint32_t)_idx * 65536UL / NUM_LEDS);
    strip.setPixelColor(_idx, strip.gamma32(strip.ColorHSV(pixelHue)));
  }
  strip.show();
  rainbowBaseHue += RAINBOW_HUE_STEP; // scroll for the next frame
}

// Renders one frame of the color-cycle effect: the whole strip shows a
// single color whose hue advances each frame, cycling through the wheel.
void renderColorCycleFrame() {
  uint32_t color = strip.gamma32(strip.ColorHSV(cycleHue));
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    strip.setPixelColor(_idx, color);
  }
  strip.show();
  cycleHue += CYCLE_HUE_STEP; // advance the hue for the next frame
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

// Renders one frame of the theater-chase effect: every CHASE_SPACING-th pixel
// is lit in the staged color and the lit group shifts one pixel along each
// frame, giving the classic marquee crawl. Unlit pixels are cleared, so the
// pattern always draws over whatever was on the strip before.
void renderTheaterChaseFrame() {
  uint32_t chaseColor = stagedEffectColor();
  strip.clear();
  for (uint16_t _idx = chaseOffset; _idx < NUM_LEDS; _idx += CHASE_SPACING) {
    strip.setPixelColor(_idx, chaseColor);
  }
  strip.show();
  // shift the lit group for the next frame, wrapping within one group
  chaseOffset++;
  if (chaseOffset >= CHASE_SPACING) {
    chaseOffset = 0;
  }
}

// Renders one frame of the comet effect: a bright head in the staged color
// sweeps along the strip and reverses at each end, trailing COMET_TAIL_LEN
// pixels that fade linearly to black. The tail is drawn from the head position
// rather than by fading the previous frame, so repeated brightness rescaling
// cannot make the colors drift.
void renderCometFrame() {
  uint32_t baseColor = stagedEffectColor();
  uint8_t baseRed = (uint8_t)(baseColor >> 16);
  uint8_t baseGreen = (uint8_t)(baseColor >> 8);
  uint8_t baseBlue = (uint8_t)baseColor;
  strip.clear();
  for (uint8_t _tail = 0; _tail < COMET_TAIL_LEN; _tail++) {
    // tail pixels sit behind the head, so they trail the direction of travel
    int16_t tailPos = cometForward ? (cometHead - _tail) : (cometHead + _tail);
    if (tailPos < 0 || tailPos >= (int16_t)NUM_LEDS) {
      continue;
    }
    // linear fade: full brightness at the head, dark at the end of the tail
    uint16_t fadeScale = 255 - ((uint16_t)_tail * 255 / COMET_TAIL_LEN);
    strip.setPixelColor(tailPos,
                        (uint8_t)((baseRed * fadeScale) / 255),
                        (uint8_t)((baseGreen * fadeScale) / 255),
                        (uint8_t)((baseBlue * fadeScale) / 255));
  }
  strip.show();
  // advance the head, reversing when it reaches either end of the strip
  if (cometForward == true) {
    if ((cometHead + 1) < (int16_t)NUM_LEDS) {
      cometHead++;
    } else {
      cometForward = false;
    }
  } else {
    if (cometHead > 0) {
      cometHead--;
    } else {
      cometForward = true;
    }
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
// two background effects, and pause/resume. For the static LED commands
// the reply is sent BEFORE the strip update: on the Uno, Adafruit_NeoPixel's
// show() disables interrupts to bit-bang the strip, and transmitting on
// SoftwareSerial immediately after a show() corrupts the outgoing frame.
// Sending first keeps the reply clean; the show() then runs during the gap
// before the next command. Effect start/pause/resume only set state - their
// frames are drawn later by serviceEffects() while the link is idle.
void handleControlCommand(uint16_t messageData) {
  switch (messageData) {
    case ctrlAbout:
      sendResponse(aboutMessage);
      break;
    case ctrlLedsOff:
      sendResponse(okMessage);
      effectMode = effectNone; // static command overrides any effect
      clearStrip();
      break;
    case ctrlFillAll:
      sendResponse(okMessage);
      effectMode = effectNone;
      fillStripWithStagedColor();
      break;
    case ctrlSetPixel:
      sendResponse(okMessage);
      effectMode = effectNone;
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
    case ctrlComet:
      sendResponse(okMessage);
      startEffect(effectComet);
      break;
    case ctrlPause:
      effectPaused = true;
      sendResponse(okMessage);
      break;
    case ctrlResume:
      effectPaused = false;
      sendResponse(okMessage);
      break;
    case ctrlStripStatus:
      if (isStripLit()) {
        sendResponse(powerStateOn);
      } else {
        sendResponse(powerStateOff);
      }
      break;
    case ctrlStripBrightness:
      if (isStripLit()) {
        char textBuffer[4];
        uint16_t brightnessSpan = MAX_BRIGHTNESS - MIN_BRIGHTNESS;
        uint16_t normalizedLevel = ((uint16_t)(ledBrightness - MIN_BRIGHTNESS) * 255) / brightnessSpan;
        itoa(normalizedLevel, textBuffer, 10);
        sendResponse(textBuffer);
      } else {
        sendResponse(powerStateOff);
      }
      break;
    case ctrlMotorFwd:
      handleMotorRunCommand(motorFwd);
      break;
    case ctrlMotorRev:
      handleMotorRunCommand(motorRev);
      break;
    case ctrlMotorStop:
      // an explicit stop is always honored, even during the cooldown lockout
      stopMotor(millis());
      sendResponse(okMessage);
      break;
    case ctrlMotorStatus:
      // report which direction (if any) the motor is currently driven in
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

// Non-blocking chair motor service, called every loop() pass. First runs a
// hardware interlock guard: the motor driver does NOT protect itself against
// both inputs being high, so if both direction pins ever read high at once
// (which the state machine should make impossible), everything is forced low
// and the cooldown lockout is armed - a fault that severe should not clear
// silently. While the motor is running it then settles the duty bucket and
// does three cheap deadline checks (signed millis() subtractions,
// rollover-safe like the effect holdoff): the duty bucket filling or the run
// reaching the MOTOR_MAX_RUN_MS hard cap force-stops the motor and arms the
// MOTOR_LOCKOUT_MS cooldown; otherwise, if the pulse deadline has passed
// (MOTOR_PULSE_MS after the most recent command), the motor stops normally.
// While idle only the interlock guard runs, so the motor never adds
// measurable work to the loop.
void serviceMotor() {
  // hardware interlock guard: never allow both direction lines high
  if ((digitalRead(MOTOR_FWD_PIN) == HIGH) && (digitalRead(MOTOR_REV_PIN) == HIGH)) {
    stopMotor(millis());
    motorLockedOut = true;
    motorLockoutUntil = millis() + MOTOR_LOCKOUT_MS;
    return;
  }
  if (motorDirection == motorIdle) {
    return;
  }
  unsigned long now = millis();
  updateMotorDutyAccumulator(now);
  // duty bucket full: cumulative run time spent -> force-stop + cooldown
  if (motorDutyUsedMs >= MOTOR_DUTY_BUDGET_MS) {
    stopMotor(now);
    motorLockedOut = true;
    motorLockoutUntil = now + MOTOR_LOCKOUT_MS;
    return;
  }
  // hard run-time cap: force-stop and lock out further runs for the cooldown
  if ((now - motorRunStart) >= MOTOR_MAX_RUN_MS) {
    stopMotor(now);
    motorLockedOut = true;
    motorLockoutUntil = now + MOTOR_LOCKOUT_MS;
    return;
  }
  // normal end of pulse: MOTOR_PULSE_MS elapsed since the last run command
  if ((long)(now - motorStopAt) >= 0) {
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
//   2. Duty budget: the leaky bucket must have room for at least one minimum
//      pulse before a run may start or be extended, otherwise the reply is
//      "BHE: COOLDOWN" and the bucket keeps draining naturally (about
//      MOTOR_DUTY_REST_RATIO seconds of rest buy one second of run time).
//   3. Extension (velocity control): the same direction repeated while
//      running pushes the stop deadline out to now + MOTOR_PULSE_MS, clamped
//      so the total run can never exceed MOTOR_MAX_RUN_MS (the duty bucket is
//      enforced separately in serviceMotor as the run progresses).
//   4. Opposite direction while running: the motor is stopped immediately
//      (never driven both ways) and the reply is "BHE: WAIT" - the settle
//      timer then blocks the reversal until MOTOR_SETTLE_MS of rest.
//   5. Direction settle: from idle, starting OPPOSITE to the previous run
//      within MOTOR_SETTLE_MS of its stop answers "BHE: WAIT". Restarting in
//      the SAME direction needs no settle.
//   6. Start: the requested line goes high (the other is already low) for a
//      minimum pulse of MOTOR_PULSE_MS, ended later by serviceMotor().
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
      // same direction repeated -> extend the pulse (velocity control),
      // clamped to the hard run-time cap measured from the run's start
      unsigned long extendedStopAt = now + MOTOR_PULSE_MS;
      if ((extendedStopAt - motorRunStart) > MOTOR_MAX_RUN_MS) {
        extendedStopAt = motorRunStart + MOTOR_MAX_RUN_MS;
      }
      motorStopAt = extendedStopAt;
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
  motorRunStart = now;
  motorStopAt = now + MOTOR_PULSE_MS;
  motorDutyLastUpdate = now; // meter run time from this instant (drop any drain remainder)
  if (requestedDirection == motorFwd) {
    digitalWrite(MOTOR_REV_PIN, LOW);
    digitalWrite(MOTOR_FWD_PIN, HIGH);
  } else {
    digitalWrite(MOTOR_FWD_PIN, LOW);
    digitalWrite(MOTOR_REV_PIN, HIGH);
  }
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

// Sets the display brightness from a 0-255 level, mapped onto the compile-time
// MIN_BRIGHTNESS..MAX_BRIGHTNESS window: level 0 gives MIN_BRIGHTNESS (dimmest
// visible, never fully dark) and level 255 gives MAX_BRIGHTNESS. Keeping the
// hardware brightness above 0 also avoids Adafruit_NeoPixel's setBrightness(0)
// case, which rescales the pixel buffer to all-zero and permanently discards
// the stored colors. Applies immediately by rescaling the current display; a
// running effect also picks it up on its next frame. Called after the reply is
// sent, so the show() here still respects the reply-before-show rule.
void setBrightnessLevel(uint8_t level) {
  uint16_t brightnessSpan = MAX_BRIGHTNESS - MIN_BRIGHTNESS;
  uint16_t scaledOffset = ((uint16_t)level * brightnessSpan) / 255;
  ledBrightness = (uint8_t)(MIN_BRIGHTNESS + scaledOffset);
  strip.setBrightness(ledBrightness);
  strip.show();
}

// Sends a delimiter-framed ("|message|\n") response back to the hub over
// the link serial port. SoftwareSerial.print() is blocking, so each byte
// is fully transmitted before the function returns. No flush() is used:
// SoftwareSerial::flush() clears the RECEIVE buffer, which would discard a
// queued next command.
void sendResponse(const char* message) {
  LinkSerial.print(respDelimiter);
  LinkSerial.print(message);
  LinkSerial.print(respDelimiter);
  LinkSerial.print('\n');
}
