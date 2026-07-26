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
 * Protocol recap:
 *   Incoming (from hub):  <9,9,RRDDD>   e.g. <9,9,11001> = register 11, command 001
 *   Outgoing (to hub):    |message|     e.g. |Hello World|
 *
 * Command map (all registers 11-20 are routed to this node by the hub):
 *   Reg 11 (control): 000 = all LEDs off (stops any effect)
 *                     001 = Hello World
 *                     002 = fill whole strip with the staged color (stops effect)
 *                     003 = set single pixel [staged index] to staged color (stops effect)
 *                     004 = start RAINBOW effect (hues trail up the strip)
 *                     005 = start COLOR-CYCLE effect (whole strip cycles hue)
 *                     006 = pause effects (freeze in place, LEDs not cleared)
 *                     007 = resume effects
 *   Reg 12: stage RED   (0-255)
 *   Reg 13: stage GREEN (0-255)
 *   Reg 14: stage BLUE  (0-255)
 *   Reg 15: stage pixel INDEX (0 .. NUM_LEDS-1)
 *   Reg 16: set BRIGHTNESS level (0-255; 0 = off, 255 = MAX_BRIGHTNESS ceiling)
 *   Reg 17 (power): 001 = power on, 002 = power off,
 *                   003 = report status, 004 = toggle power
 *   Every command replies |OK|, |Hello World|, a status digit (|1|/|0|),
 *   or |X| (unrecognized / out of range / invalid sense reading).
 *
 * Effects run in the background but only ever call strip.show() while the
 * link is idle (no message arriving/queued), so they don't corrupt incoming
 * commands. EFFECT_INTERVAL_MS sets the animation speed.
 */

#include <SoftwareSerial.h>
#include <Adafruit_NeoPixel.h>

// --- WS2812 LED strip ---
#define LED_PIN 6      // WS2812 data pin (keep clear of the link pins 10/11)
#define NUM_LEDS 60    // number of pixels on the strip (change to match hardware)
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- LED brightness and effect tuning constants ---
#define MAX_BRIGHTNESS 100 // brightness ceiling (0-255); a level of 255 maps here
const uint16_t EFFECT_INTERVAL_MS = 50; // ms between animation frames (lower = faster;
                                        // too low raises the odds of clipping a command)
const uint16_t RAINBOW_HUE_STEP = 256;  // hue advance per frame for the rainbow scroll
const uint16_t CYCLE_HUE_STEP   = 256;  // hue advance per frame for the color cycle

// --- link to the hub (SoftwareSerial) ---
#define serialBaudRate 9600
#define LINK_RX_PIN 10 // receive from hub ExtSerial2 TX
#define LINK_TX_PIN 11 // transmit to hub ExtSerial2 RX
SoftwareSerial LinkSerial(LINK_RX_PIN, LINK_TX_PIN);

// --- power control I/O (from AutomateHub_1) ---
#define PWR_TRIG_1 9 // TRIGGER TIP - pulses to switch the attached device
#define PWR_SENS_1 2 // SENSE RING  - reads the attached device's power state

// --- protocol constants (match AutomateHub_1) ---
const char nullTrm = '\0';
const uint8_t maxMessage = 32;
const uint8_t cmdRegLen = 2; // register field length (2 digits)
const uint8_t cmdDatLen = 3; // data field length (3 digits)
const char serialDataStart = '<';
const char serialDataEnd = '>';
const char respDelimiter = '|';

// --- register map (which command register does what) ---
const uint8_t regControl    = 11; // hello + LED apply/clear commands
const uint8_t regStageRed   = 12; // stage red channel
const uint8_t regStageGreen = 13; // stage green channel
const uint8_t regStageBlue  = 14; // stage blue channel
const uint8_t regStageIndex = 15; // stage target pixel index
const uint8_t regBrightness = 16; // set overall brightness
const uint8_t regPower      = 17; // power control (on/off/status/toggle)

// --- control-register (11) command values ---
const uint16_t ctrlLedsOff   = 0; // clear the whole strip (stops any effect)
const uint16_t ctrlHello     = 1; // reply with the hello message
const uint16_t ctrlFillAll   = 2; // fill entire strip with staged color (stops effect)
const uint16_t ctrlSetPixel  = 3; // set one pixel (staged index) to staged color (stops effect)
const uint16_t ctrlRainbow   = 4; // start the rainbow effect
const uint16_t ctrlCycle     = 5; // start the color-cycle effect
const uint16_t ctrlPause     = 6; // pause effects (freeze, do not clear)
const uint16_t ctrlResume    = 7; // resume effects

// --- power-register (17) command values ---
const uint16_t powerOn     = 1; // switch the device on (if currently off)
const uint16_t powerOff    = 2; // switch the device off (if currently on)
const uint16_t powerStatus = 3; // report the device's current power state
const uint16_t powerToggle = 4; // toggle the device's power

// --- reply strings ---
const char helloMessage[]  = "BedroomHubExt: v1.0 Automation Controller";
const char okMessage[]     = "BedroomHubExt: OK";
const char errorMessage[]  = "BedroomHubExt: INVALID COMMAND!"; // unrecognized register/command or out-of-range value
const char powerStateOn[]  = "1"; // status reply: device is on
const char powerStateOff[] = "0"; // status reply: device is off

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
uint8_t effectMode = effectNone;
bool effectPaused = false;
uint16_t rainbowBaseHue = 0;      // scroll offset for the rainbow effect
uint16_t cycleHue = 0;            // current hue for the color-cycle effect
unsigned long lastEffectUpdate = 0; // millis() timestamp of the last frame

// --- link recovery tuning ---
// Suppress effect frames for this long after ANY received byte (even a
// corrupted one). A clipped command therefore silences the animation, so
// the master's ~500 ms retry arrives on a clean, effect-free link.
const uint16_t EFFECT_RX_HOLDOFF_MS = 1500;
// Abandon a partial frame after this much RX silence. A complete command
// takes ~11 ms at 9600 baud, so a frame idle this long lost its terminator
// (clipped by an effect show()) and must not absorb the next retry.
const uint16_t FRAME_ABANDON_MS = 100;
unsigned long lastRxActivity = 0;  // millis() timestamp of the last received byte

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
  strip.begin();
  strip.setBrightness(ledBrightness);
  strip.clear();
  strip.show(); // all pixels off at boot
  serialMessageIn[0] = nullTrm;
}

// Main loop: services the hub link first (so incoming bytes are always
// handled promptly), then advances any running LED effect - but only
// while the link is idle, so an animation frame can never overlap an
// incoming command.
void loop() {
  serviceSerial();
  serviceEffects();
}

// Reads and processes framed commands from the hub link: buffers an
// incoming message one character at a time, and once a full frame has
// arrived, decodes and acts on it. Every received byte stamps
// lastRxActivity (which also holds off effect rendering), and a partial
// frame that stalls for FRAME_ABANDON_MS - its terminator was lost to a
// corrupted byte - is abandoned so a retry decodes cleanly instead of
// being appended to stale data.
void serviceSerial() {
  if (serialMsgEnd == 0) {
    // still collecting a message
    if (LinkSerial.available()) {
      lastRxActivity = millis(); // any byte (even garbage) counts as activity
      readSerialData(LinkSerial.read(), serialDataStart, serialDataEnd);
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
// command could be in flight. EFFECT_INTERVAL_MS paces the frame rate.
void serviceEffects() {
  // nothing to draw if no effect is running or it is paused (frozen)
  if (effectMode == effectNone || effectPaused) {
    return;
  }
  // never drive the strip while a command is arriving or already queued
  if (serialReading || serialMsgEnd || LinkSerial.available()) {
    return;
  }
  // hold off after recent RX activity: a clipped command's bytes suppress
  // the animation so the master's retry lands on a clean, silent link
  if ((millis() - lastRxActivity) < EFFECT_RX_HOLDOFF_MS) {
    return;
  }
  // frame-rate gate (animation speed)
  unsigned long now = millis();
  if ((now - lastEffectUpdate) < EFFECT_INTERVAL_MS) {
    return;
  }
  lastEffectUpdate = now;
  // draw one frame of the active effect
  if (effectMode == effectRainbow) {
    renderRainbowFrame();
  } else if (effectMode == effectCycle) {
    renderColorCycleFrame();
  }
}

// Selects an effect and un-pauses it, scheduling the first frame to draw
// on the next idle service pass.
void startEffect(uint8_t mode) {
  effectMode = mode;
  effectPaused = false;
  lastEffectUpdate = millis() - EFFECT_INTERVAL_MS; // draw promptly
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
        sendResponse(okMessage);
        setBrightnessLevel((uint8_t)messageData);
      } else {
        sendResponse(errorMessage);
      }
      break;
    case regPower:
      handlePowerCommand(messageData);
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
    case ctrlHello:
      sendResponse(helloMessage);
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
    case ctrlPause:
      effectPaused = true; // freeze in place, leave the LEDs lit
      sendResponse(okMessage);
      break;
    case ctrlResume:
      effectPaused = false;
      sendResponse(okMessage);
      break;
    default:
      sendResponse(errorMessage);
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
  if (messageData < powerOn || messageData > powerToggle) {
    sendResponse(errorMessage);
    return;
  }
  int senseState = powerSense(PWR_SENS_1);
  if (messageData == powerStatus) {
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
      if (messageData == powerOff) {
        powerPulse(PWR_TRIG_1);
      }
    } else { // device currently off (or invalid reading)
      if (messageData == powerOn) {
        powerPulse(PWR_TRIG_1);
      }
    }
    if (messageData == powerToggle) {
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

// Turns the whole strip off.
void clearStrip() {
  strip.setBrightness(ledBrightness);
  strip.clear();
  strip.show();
}

// Fills every pixel with the currently staged red/green/blue color.
void fillStripWithStagedColor() {
  strip.setBrightness(ledBrightness);
  for (uint16_t _idx = 0; _idx < NUM_LEDS; _idx++) {
    strip.setPixelColor(_idx, stagedRed, stagedGreen, stagedBlue);
  }
  strip.show();
}

// Sets the single staged-index pixel to the staged red/green/blue color,
// leaving the rest of the strip unchanged.
void setStagedPixelColor() {
  strip.setBrightness(ledBrightness);
  strip.setPixelColor(stagedIndex, stagedRed, stagedGreen, stagedBlue);
  strip.show();
}

// Sets the display brightness from a 0-255 level where 0 is off and 255 is
// MAX_BRIGHTNESS (the compile-time ceiling): actual = level * MAX_BRIGHTNESS
// / 255. Applies immediately by rescaling the current display; a running
// effect also picks it up on its next frame. Called after the reply is
// sent, so the show() here still respects the reply-before-show rule.
void setBrightnessLevel(uint8_t level) {
  ledBrightness = (uint8_t)(((uint16_t)level * MAX_BRIGHTNESS) / 255);
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
