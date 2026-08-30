///////////////////////////////////////////////////////////////////////////////
// Preamp Controller
// by Ben Provenzano III
// v3.0 06/11/2023
// v4.3 03/29/2026
// v4.4 08/30/2026 - refactor:
//   * RS-232 layer updated to the BedroomHub v3 protocol implementation
//     (data-field extraction fix, reader-state reset on forward timeout,
//     single "active external port" pointer, burst reads).
//   * Long blocking waits (startup / shutdown / progress bars / motor
//     settling) replaced with millis()-driven state machines; the superloop
//     now keeps servicing serial while those sequences run.
//   * Constant tables moved to flash (PROGMEM / F()), tunables made const.
//   * LCD writes coalesced and change-gated to cut I2C traffic.
// v4.5 08/30/2026 - the 0-100 volume percentage is now converted once, stored
//   in volPercent, and shared by the panel and writeVolumeStatus().
// v4.6 08/30/2026 - volRelayRestMs enforces a non-blocking settling gap between
//   attenuator relay switches; adjust requests arriving inside it are dropped.
// v4.7 08/30/2026 - a pending showMessage() redraw could no longer repaint the
//   input name over the standby screen after a power-down.
// v4.8 08/30/2026 - reusable non-blocking display animations: lcdSpinner*,
//   lcdSlide* and lcdPopup*. COM Error! popup on a bad/absent DAC reply.
// v4.9 08/30/2026 - classic "-\|/" spinner via a borrowed CGRAM slot for the
//   backslash; slide speed slowed and pulled out as lcdSlideStepMs.
// v4.10 08/30/2026 - labels slide in reading-order (lcdSlideFromRight), plus a
//   lcdSlideReveal mode; spinner and slide both slowed.
// Wire protocol, command set and user-visible display output are unchanged.
///////////////////////////////////////////////////////////////////////////////

// shared libraries
#include <Arduino.h>
#include <SoftwareSerial.h>
#include <Wire.h>

// local libraries
#include "NeoTimer.h"
#include "LiquidCrystal_I2C.h"  // custom for MCP23008-E/P, power button support

///////////////////////////////////////////////////////////////////////////////
// configuration
///////////////////////////////////////////////////////////////////////////////

// I2C addresses
static const uint8_t inputResetAddr = 0x3A;
static const uint8_t inputSetAddr = 0x3B;
static const uint8_t volResetAddr = 0x3E;
static const uint8_t volSetAddr = 0x3F;
static const uint8_t lcdAddr = 0x27;

// general constants
static const char nullTrm = '\0';

// 16x2 display
LiquidCrystal_I2C lcd(lcdAddr);
static const uint8_t lcdCols = 16;            // number of columns in the LCD
static const uint8_t lcdRows = 2;             // number of rows in the LCD
static const uint8_t lcdBacklightPin = 9;     // display backlight pin
static const uint8_t lcdOffBrightness = 140;  // standby-off LCD brightness level ***
static const uint8_t lcdOnBrightness = 255;   // online LCD brightness level ***

// custom characters, CGRAM slot == row index (kept in flash, copied on demand)
static const uint8_t customChars[8][8] PROGMEM = {
  { B00001, B00011, B00101, B01001, B01001, B01011, B11011, B11000 },  // 0 sound
  { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10 },                  // 1 bar 1/5
  { 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18 },                  // 2 bar 2/5
  { 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C },                  // 3 bar 3/5
  { 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E },                  // 4 bar 4/5
  { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F },                  // 5 bar 5/5
  { B00001, B00011, B01111, B01111, B01111, B00011, B00001, B00000 },  // 6 speaker
  { B00000, B01010, B11111, B11111, B01110, B00100, B00000, B00000 }   // 7 heart
};
static const uint8_t glyphSound = 0;
static const uint8_t glyphBarFull = 5;
static const uint8_t glyphSpeaker = 6;
static const uint8_t glyphHeart = 7;

Neotimer delayDrawInputs = Neotimer(4000);  // pause for 4-seconds before showing

// serial resources
#define serialBaudRate 9600
static const uint8_t preampRegister = 15;
static const uint8_t dacRegister = 16;
static const uint8_t ampRegister = 17;
#define DAC_RX_PIN 11  // 232-RX 3.5mm TRS Ring (DAC)
#define DAC_TX_PIN 12  // 232-TX 3.5mm TRS Tip (DAC)
SoftwareSerial ExtTxRx(DAC_RX_PIN, DAC_TX_PIN);
#define AMP_RX_PIN 6  // Not Used
#define AMP_TX_PIN 8  // 232-TX RCA Jack (Class-D Amp)
SoftwareSerial ExtTxOnly(AMP_RX_PIN, AMP_TX_PIN);
// NOTE: SoftwareSerial can only RECEIVE on one port at a time. ExtTxOnly is
// transmit-only, so ExtTxRx is the only port ever placed in listen mode; the
// pointer below tracks the port a forwarded response will arrive on.
SoftwareSerial *activeExtSerial = &ExtTxRx;
static const int16_t offCode = -1;
static const uint8_t maxMessage = 16;
static const uint8_t cmdRegLen = 2;      // register length (99***)
static const uint8_t cmdDatLen = 3;      // data length (**999)
static const uint8_t cmdTotalLen = cmdRegLen + cmdDatLen;
static const uint8_t maxLineChars = 10;  // longest accepted line-prefix field
static const uint8_t serialReadBurst = 24;  // max chars drained per loop pass
static const char serialDataStart = '<';
static const char serialDataEnd = '>';
static const char respDelimiter = '|';
char serialMessageOut[maxMessage + 1];
char serialMessageIn[maxMessage + 1];
// locally originated payloads are built here so an in-flight receive on
// serialMessageIn can never be clobbered mid-message
char extMsgOut[cmdTotalLen + 1];
uint8_t serialCurPos = 0;
bool serialFwrdMode = 0;
bool serialReading = 0;
bool serialMsgEnd = 0;
static const uint16_t maxFwrdWait = 500;  // max wait in (ms) for external serial response
Neotimer maxFwrdRead = Neotimer();
uint8_t writeRegister = 0;
uint16_t writeCommand = 0;
bool disableResponse = 0;

// input selector
static const uint8_t inputCount = 6;
static const uint8_t inputNameLen = 10;
uint8_t inputSelected = 1;
static const uint8_t inputRelayCount = 3;
static const char inputNames[inputCount][inputNameLen + 1] PROGMEM = {
  "DIGI PC   ",
  "AUX       ",
  "PHONO     ",
  "COAXIAL   ",
  "OPTICAL   ",
  "DIGI AUTO "
};

// volume control
static const uint8_t volCoarseSteps = 6;  // volume steps to skip for coarse adjust ***
static const uint8_t volMaxLimit = 90;    // max percentage of volume range (100%) when in limit mode ***
static const uint8_t volMinLimit = 40;    // min percentage of volume range (0%)
static const uint8_t volRelayCount = 8;   // number of relays on volume attenuator board ***
bool volLimitState = 1;                   // limit on by default
#define volControlDown 1
#define volControlUp 2
#define volControlSlow 1
#define volControlFast 2
uint8_t volLastLevel = 0;  // last un-muted level the host is told about
uint8_t volPercent = 0;    // volLastLevel on the 0-100 scale, single source of
                           // truth for both the panel and writeVolumeStatus()
uint8_t volLevel = 0;
uint8_t volSpan = 0;
uint8_t volMax = 0;
uint8_t volMin = 0;
bool volMute = 0;
// Latching-relay settling gap. After the attenuator coils are pulsed they are
// left alone for this long; volume adjust requests that arrive inside the
// window are dropped rather than queued. Raise it if the relays chatter. ***
static const uint16_t volRelayRestMs = 100;
uint32_t volRelayRestMillis = 0;
bool volRelayResting = false;

// high-pass filter
#define hpfRelayPin 10  // HPF relay pin
bool hpfState = 0;

// motor pins
#define potAnalogPin A1  // pot reading pin A1
#define motorPinCW 16    // motor H-bridge CW pin
#define motorPinCCW 17   // motor H-bridge CCW pin
// motor resources
#define motorInit 1       // motor initialization
#define motorSettled 2    // pot at resting state
#define motorInMotion 3   // pot is moving right now
#define motorCoasting 4   // pot just passed read value
#define potThreshold 4    // pot value change threshold ***
#define potRereads 6      // amount of pot readings ***
#define potMinRange 30    // lowest pot reading
#define potMaxRange 1023  // highest pot reading
static const uint8_t motorStopSettleMs = 25;  // wait after the motor is cut
static const uint8_t motorCoastSampleMs = 20; // coast re-sample interval
int16_t potValueLast = 0;                     // range from 0-1023
uint8_t volPotLast = 0;
uint8_t potState = 0;
uint32_t motorSampleMillis = 0;
uint8_t motorSampleWait = 0;

// power pins
#define powerRelayPin 7
#define powerButtonPin 5
// power resources
static const uint8_t startDelay = 5;      // startup delay in seconds, unmutes after ***
static const uint8_t shutdownTime = 3;    // shutdown delay before turning off aux power ***
static const uint8_t initStartDelay = 2;  // delay on initial cold start ***
static const uint8_t debounceDelay = 50;  // button debounce delay in ms
static const uint8_t buttonPollMs = 10;   // power button I2C poll interval
static const uint16_t startLimitMsgMs = 650;   // "Volume Limit" splash hold
static const uint16_t startSettleMs = 350;     // relay settling after startup
static const uint16_t standbyHoldMs = 500;     // standby splash hold
uint32_t powerButtonMillis = 0;
uint32_t powerButtonPollMillis = 0;
uint8_t lastPowerButton = 0;
uint8_t powerButton = 0;
bool powerCycle = 0;
bool powerState = 0;

// power on/off sequencer (replaces the blocking startup()/shutdown() routines)
#define pwrSeqIdle 0
#define pwrSeqStartLimitMsg 1  // showing "Volume Limit ..."
#define pwrSeqStartBar 2       // "Starting Up..." progress bar
#define pwrSeqStartSettle 3    // relays settling, then restore input
#define pwrSeqShutBar 4        // "Shutting Down..." progress bar
#define pwrSeqShutStandby 5    // standby splash, then drop aux power
uint8_t powerSeqState = pwrSeqIdle;
uint32_t powerSeqMillis = 0;

// non-blocking progress bar state
uint8_t barRowsLeft = 0;
uint8_t barLine = 0;
uint8_t barCol = 0;
uint8_t barSeg = 0;
uint16_t barStepMs = 0;
uint32_t barStepMillis = 0;

// display change-tracking (avoids redundant I2C traffic)
uint8_t lcdBarLastBlock = 0xFF;
uint8_t lcdBarLastTip = 0xFF;
uint8_t lcdInputLastDrawn = 0xFF;
int16_t lcdVolLastPct = -1;

///////////////////////////////////////////////////////////////////////////////
// reusable display animations
//
// Each one is a start/step/stop trio in the same shape as lcdTimedBar*: state
// lives in a handful of bytes, one frame is emitted per step() call, and the
// step() functions are pumped unconditionally from loop() where they are
// no-ops while idle. Keep frames to a few cells -- through the MCP23008 every
// character costs roughly 0.7 ms on a 100 kHz bus.
///////////////////////////////////////////////////////////////////////////////

// --- spinner ---------------------------------------------------------------
// Classic "-\|/" rotating single-cell activity indicator.
//
// Three of the four frames are ROM characters, but 0x5C is the yen sign on the
// HD44780 A00 character ROM rather than a backslash, so that frame is drawn
// from a CGRAM slot instead. The slot is borrowed on lcdSpinnerStart() and
// restored from customChars[] on lcdSpinnerStop(), which makes the spinner
// render identically on A00 and A02 modules.
//
// Slot 7 (the heart) is borrowed because it is only ever on screen during the
// standby easter egg, which cannot overlap a spinner. Point this at a
// different slot if you ever spin one up alongside the heart. ***
static const uint8_t lcdSpinnerGlyphSlot = glyphHeart;
static const uint8_t lcdSpinnerBackslash[8] PROGMEM = {
  // mirrors the ROM '/' so both diagonals sit at the same height
  0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00
};
static const uint8_t lcdSpinnerFrameCount = 4;
static const uint8_t lcdSpinnerFrames[lcdSpinnerFrameCount] PROGMEM = {
  '-', lcdSpinnerGlyphSlot, '|', '/'
};
// 250 ms per frame is one full revolution per second. ***
static const uint16_t lcdSpinnerStepMs = 175;
uint8_t lcdSpinnerCol = 0;
uint8_t lcdSpinnerRow = 0;
uint8_t lcdSpinnerFrame = 0;
uint32_t lcdSpinnerMillis = 0;
bool lcdSpinnerActive = false;

// --- text slide ------------------------------------------------------------
// Slides a flash string into a fixed-width field one column per frame.
// Direction of travel. A rigid block of text entering at the LEFT edge shows
// its trailing character first ("C", "PC", " PC", ...), which reads as garbled
// text; lcdSlideFromLeft is kept for non-text uses but is rarely what you want
// for a label. lcdSlideFromRight and lcdSlideReveal both preserve reading
// order, so the label is legible in every frame.
#define lcdSlideFromLeft 0   // travels rightwards, trailing edge appears first
#define lcdSlideFromRight 1  // travels leftwards, first character appears first
#define lcdSlideReveal 2     // stays in place, uncovered one column at a time
static const uint8_t lcdSlideMaxWidth = 10;  // widest field the slider handles
// Milliseconds per column. With lcdSlideFromRight the frame count is the field
// width, not the label length, so every label takes the same time: at 100 ms a
// 10-wide input name takes 1.0 s and the 4-wide MUTE takes 400 ms. Lower it to
// speed the slide up. ***
static const uint16_t lcdSlideStepMs = 150;
char lcdSlideText[lcdSlideMaxWidth + 1];
uint8_t lcdSlideCol = 0;
uint8_t lcdSlideRow = 0;
uint8_t lcdSlideWidth = 0;
uint8_t lcdSlideLen = 0;
uint8_t lcdSlideOffset = 0;  // columns still to travel; 0 == final position
uint8_t lcdSlideDir = lcdSlideFromLeft;
uint32_t lcdSlideMillis = 0;
bool lcdSlideActive = false;

// --- popup -----------------------------------------------------------------
// Temporary full-width banner over the bottom row; the row is rebuilt from
// live state when it expires.
static const uint16_t lcdPopupHoldMs = 2000;  // dwell time ***
uint32_t lcdPopupMillis = 0;
bool lcdPopupActive = false;

///////////////////////////////////////////////////////////////////////////////
// forward declarations
///////////////////////////////////////////////////////////////////////////////

void internalFunctions(uint16_t command);
bool checkPreampState();
void setInput(uint8_t command);
void writeVolumeStatus();
void writeTwoStateSerialMessage(int16_t state1, int16_t state2);
void writeSerialMessage(int16_t value);
long l_map(long x, long in_min, long in_max, long out_min, long out_max);
void resetRxTxBuffers();
void disableFwrdMode();
void serialProcess();
void readSerialData(char rc, char startInd, char endInd);
void writeSerialData(bool InOut);
void decodeMessage();
void processCmdData(uint8_t messageStart);
void routeQueued();
void routeMessage(bool origin, uint8_t reg, uint16_t ctldata);
bool setRelays(uint8_t pcf_a, uint8_t pcf_b, uint8_t new_state,
               uint8_t previous_state, uint8_t installed_relay_count,
               uint8_t forced_update_flag);
void resetPCF(uint8_t pcf_a, uint8_t pcf_b);
void PCFexpanderWrite(uint8_t address, uint8_t _data);
int readPotWithSmoothing(uint8_t analog_port_num, uint8_t reread_count);
int readPotWithClipping(int sensed_pot_value);
void potValueChanges(void);
void motorControlLoop(void);
void motorPot(void);
void volIncrement(uint8_t dir_flag, uint8_t speed_flag);
uint8_t volToPercent(uint8_t _level);
bool volRelayReady();
void volUpdate(uint8_t _vol, uint8_t _force, bool _redraw);
void muteSystem();
void inputUpdate(uint8_t _input);
void setInputRelays(uint8_t state);
void updateInputDisplay(uint8_t input);
void hpfControl(uint8_t _state);
void showMessage(const __FlashStringHelper *message);
void cancelInputMessage();
void showHeartMessage();
void lcdClear();
void lcdSpinnerStart(uint8_t _col, uint8_t _row);
void lcdSpinnerStep();
void lcdSpinnerStop(bool _clearCell);
void lcdSlideStart(uint8_t _col, uint8_t _row, uint8_t _width,
                   const __FlashStringHelper *_text, uint8_t _dir);
void lcdSlideDraw(uint8_t _offset);
bool lcdSlideStep();
void lcdSlideFinish();
void lcdSlideAbort();
void lcdPopupStart(const __FlashStringHelper *_msg);
void lcdPopupStep();
void lcdDrawBottomRow();
bool serialResponseValid();
void lcdVolume(uint8_t _pct);
void lcdBar(uint8_t row, int16_t var, int16_t minVal, int16_t maxVal);
void lcdTimedBarStart(uint8_t _sec, uint8_t _rows);
bool lcdTimedBarStep();
void lcdTimedBarBlocking(uint8_t _sec, uint8_t _rows);
void setPowerState();
void powerSequenceStep();
void startupBegin();
void shutdownBegin();
void lcdStandbyDraw();
void initSerial();
void initTimers();
void initGPIO();
void initDisplay();

///////////////////////////////////////////////////////////////////////////////
// command dispatch
///////////////////////////////////////////////////////////////////////////////

// Executes commands addressed to the preamp register. The reply for every
// command is left in serialMessageOut; serialProcess() transmits it.
void internalFunctions(uint16_t command) {
  switch (command) {
    // input select: 1=PC 2=aux 3=phono 4=coaxial 5=optical 6=auto
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
      setInput(command);
      break;
    // current input
    case 7:
      if (powerState == 0) {
        writeSerialMessage(offCode);
        break;
      }
      writeSerialMessage(inputSelected);
      break;
    // volume down fast (VOL-)
    case 8:
      if (checkPreampState() == false) { break; }
      volIncrement(volControlDown, volControlFast);
      writeVolumeStatus();
      break;
    // volume up fast (VOL+)
    case 9:
      if (checkPreampState() == false) { break; }
      volIncrement(volControlUp, volControlFast);
      writeVolumeStatus();
      break;
    // volume down slow
    case 10:
      if (checkPreampState() == false) { break; }
      volIncrement(volControlDown, volControlSlow);
      writeVolumeStatus();
      break;
    // volume up slow
    case 11:
      if (checkPreampState() == false) { break; }
      volIncrement(volControlUp, volControlSlow);
      writeVolumeStatus();
      break;
    // mute toggle (MUTE)
    case 12:
      if (powerState == 0) {
        writeSerialMessage(offCode);
        break;
      }
      muteSystem();
      if (volMute == 1) {
        writeSerialMessage(0);
      } else {
        writeVolumeStatus();
      }
      break;
    // current volume level
    case 13:
      if (checkPreampState() == false) { break; }
      writeVolumeStatus();
      break;
    // toggle high-pass filter
    case 14:
      if (checkPreampState() == false) { break; }
      hpfControl(2);
      writeSerialMessage(hpfState);
      break;
    // toggle volume limiter then power-off
    case 15:
      if (checkPreampState() == false) { break; }
      volLimitState = !volLimitState;
      if (powerState == 1) {
        powerState = 0;
        powerCycle = 1;
      }
      writeSerialMessage(volLimitState);
      break;
    // power toggle
    case 16:
      powerState = !powerState;
      powerCycle = 1;
      writeSerialMessage(powerState);
      break;
    // power on
    case 17:
      if (powerState == 0) {
        powerState = 1;
        powerCycle = 1;
      }
      writeSerialMessage(powerState);
      break;
    // power off
    case 18:
      if (powerState == 1) {
        powerState = 0;
        powerCycle = 1;
      }
      writeSerialMessage(powerState);
      break;
    // power status
    case 19:
      writeSerialMessage(powerState);
      break;
    // volume limit and high-pass filter status
    case 20:
      if (checkPreampState() == false) { break; }
      writeTwoStateSerialMessage(volLimitState, hpfState);
      break;
    case 21:
      showHeartMessage();
      writeSerialMessage(0);
      break;
    default:
      writeSerialMessage(0);
      break;
  }
}

// True only when the preamp is powered and un-muted; otherwise queues the
// appropriate status reply and returns false.
bool checkPreampState() {
  if (powerState == 0) {  // offline
    writeSerialMessage(offCode);
    return false;
  }
  if (volMute == 1) {  // muted
    writeSerialMessage(0);
    return false;
  }
  return true;
}

void setInput(uint8_t command) {
  if (checkPreampState() == false) { return; }
  inputUpdate(command);
  writeSerialMessage(inputSelected);
}

void writeVolumeStatus() {
  // no re-conversion here: report exactly the number the panel is showing
  writeSerialMessage(volPercent);
}

// itoa() is used instead of snprintf() throughout: the values are all plain
// integers, and keeping printf out of the image saves ~1.4 kB of flash on this
// part. Output is byte-for-byte identical to the previous "%d" / "%d~%d".
void writeTwoStateSerialMessage(int16_t state1, int16_t state2) {
  char *_p = serialMessageOut;      // worst case "-32768~-32768" = 13 chars
  itoa(state1, _p, 10);
  _p += strlen(_p);
  *_p++ = '~';
  itoa(state2, _p, 10);
}

void writeSerialMessage(int16_t value) {
  itoa(value, serialMessageOut, 10);
}

// inclusive-range mapping helper (map() truncates the top of the range)
long l_map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min + 1) / (in_max - in_min + 1) + out_min;
}

///////////////////////////////////////////////////////////////////////////////
// RS-232 logic
///////////////////////////////////////////////////////////////////////////////

// Clears both serial message buffers and the end-of-message flag,
// preparing the state machine for the next message.
void resetRxTxBuffers() {
  serialMessageOut[0] = nullTrm;
  serialMessageIn[0] = nullTrm;
  serialMsgEnd = 0;
}

// Leaves external-response forwarding mode and stops/clears the
// response timeout timer.
void disableFwrdMode() {
  serialFwrdMode = 0;
  maxFwrdRead.reset();
  maxFwrdRead.stop();
}

// Serial state machine: reads commands from the main serial port, decodes and
// routes them, and while in forwarding mode relays the response from the
// selected external port back to the main port. Enforces the response timeout.
// Command execution is held off while a power sequence is running; incoming
// characters keep being buffered, exactly as they were during the old blocking
// startup()/shutdown() routines.
void serialProcess() {
  // read main serial port
  if (serialFwrdMode == 0) {
    if (serialMsgEnd == 0) {
      uint8_t _guard = 0;
      while (Serial.available() && serialMsgEnd == 0 && _guard < serialReadBurst) {
        readSerialData(Serial.read(), serialDataStart, serialDataEnd);
        _guard++;
      }
    }
    if (serialMsgEnd == 1 && powerSeqState == pwrSeqIdle) {
      // process serial data
      decodeMessage();
      // send response to main serial
      if (serialFwrdMode == 0) {
        writeSerialData(0);
      }
      resetRxTxBuffers();
    }
  }
  // forwarding mode enabled
  if (serialFwrdMode == 1) {
    // listen to the selected external serial port when in forwarding mode
    if (serialMsgEnd == 0) {
      uint8_t _guard = 0;
      while (activeExtSerial->available() && serialMsgEnd == 0 && _guard < serialReadBurst) {
        readSerialData(activeExtSerial->read(), respDelimiter, respDelimiter);
        _guard++;
      }
    }
    if (serialMsgEnd == 1) {
      // a framed reply arrived, but an empty or digit-less one is not a
      // status value -- treat it the same as no reply at all
      if (serialResponseValid() == false) {
        lcdPopupStart(F("COM Error!"));
      }
      // forward external serial reponse to main serial
      writeSerialData(1);  // write serial in buffer
      disableFwrdMode();
      resetRxTxBuffers();
    }
  }
  // external serial reponse timeout
  if (maxFwrdRead.done()) {
    Serial.println(F("*EXT-232 MAX WAIT EXCEEDED!*"));
    lcdPopupStart(F("COM Error!"));
    // reset the character reader as well, otherwise a timeout that lands
    // mid-response leaves serialReading/serialCurPos dirty and corrupts the
    // parse of the next main-serial message
    serialReading = 0;
    serialCurPos = 0;
    // the reply we were suppressing never arrived; do not swallow the next one
    disableResponse = false;
    disableFwrdMode();
    resetRxTxBuffers();
  }
}

// A forwarded reply only counts as valid if it carries at least one digit.
// Every status value these devices return is numeric, so an empty frame or a
// frame of punctuation means the link misbehaved. Tighten this if the DAC
// gains non-numeric replies.
bool serialResponseValid() {
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    const char _vchr = serialMessageIn[_idx];
    if (_vchr == nullTrm) {
      break;
    }
    if (isDigit(_vchr)) {
      return true;
    }
  }
  return false;
}

// Character-by-character framed reader: waits for startInd, buffers characters
// into serialMessageIn (overflow-safe), and sets serialMsgEnd when endInd is
// received. Used for both main-serial commands ('<'/'>') and external-serial
// responses ('|'/'|').
void readSerialData(char rc, char startInd, char endInd) {
  if (serialReading == 1) {
    // end-of-reading
    if (rc == endInd) {
      // terminate the string
      serialMessageIn[serialCurPos] = nullTrm;
      serialReading = 0;
      serialCurPos = 0;
      serialMsgEnd = 1;
    } else {
      // store characters in buffer
      if (rc != startInd) {
        if (serialCurPos < maxMessage) {
          serialMessageIn[serialCurPos] = rc;
          serialCurPos++;
        }
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

// Writes a delimiter-framed ("|...|\n") response to the main serial port while
// blinking the activity LED. InOut selects the source: 1 = relay
// serialMessageIn (external response), 0 = serialMessageOut (local response).
void writeSerialData(bool InOut) {
  if (disableResponse == true) {
    disableResponse = false;
    return;
  }
  const char *_src = (InOut == 1) ? serialMessageIn : serialMessageOut;
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.print(respDelimiter);
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    char _fwrdChr = _src[_idx];
    if (_fwrdChr == nullTrm) {
      break;
    }
    Serial.write(_fwrdChr);
  }
  Serial.print(respDelimiter);
  Serial.print('\n');
  digitalWrite(LED_BUILTIN, LOW);
}

// Validates and decodes a received main-serial message of the form "9,9,RRDDD":
// verifies the delimiter count and the leading "9" prefix, then hands the
// position of the command payload to processCmdData().
void decodeMessage() {
  const char _delimiter = ',';
  uint8_t _delims = 0;
  uint8_t _linepos = 0;
  uint8_t _cmd2pos = 0;
  bool _firstFound = false;
  // single pass: count delimiters and record the first two positions
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    char _vchr = serialMessageIn[_idx];
    if (_vchr == nullTrm) {
      break;
    }
    if (_vchr == _delimiter) {
      if (_firstFound == false) {
        _linepos = _idx;
        _firstFound = true;
      } else if (_delims == 1) {
        _cmd2pos = _idx;
      }
      _delims++;
    }
  }
  // exit when delimiters incorrect
  if (_delims < 2) {
    Serial.println(F("*PREAMP: INVALID DELIMITERS!*"));
    return;
  }
  // copy the line-prefix field and convert it
  char _linebuffer[maxLineChars + 1];
  uint8_t _linecount = 0;
  for (uint8_t _idx = 0; _idx < _linepos && _linecount < maxLineChars; _idx++) {
    _linebuffer[_linecount] = serialMessageIn[_idx];
    _linecount++;
  }  // terminate string
  _linebuffer[_linecount] = nullTrm;
  uint8_t controlData = atoi(_linebuffer);
  // process command data
  if (controlData == 9) {
    processCmdData(_cmd2pos + 1);
  } else {
    Serial.println(F("*PREAMP: INVALID PREFIX!*"));
  }
}

// Extracts the fixed-length numeric control code from the message starting at
// messageStart, splits it into the 2-digit register select and 3-digit command
// data, then routes the command via routeMessage().
void processCmdData(uint8_t messageStart) {
  uint8_t _numcnt = 0;
  char _cmdarr[cmdTotalLen + 1];
  // extract control code from message
  for (uint8_t _idx = messageStart; _idx < maxMessage; _idx++) {
    char _curchar = serialMessageIn[_idx];
    if (_curchar == nullTrm) {
      break;
    }
    if (isDigit(_curchar) && _numcnt < cmdTotalLen) {
      _cmdarr[_numcnt] = _curchar;
      _numcnt++;
    }
  }
  _cmdarr[_numcnt] = nullTrm;
  // validate control code length
  if (_numcnt != cmdTotalLen) {
    Serial.println(F("*PREAMP: INVALID LENGTH!*"));
    return;
  }
  // extract register select
  char _regarr[cmdRegLen + 1];
  for (uint8_t _idx = 0; _idx < cmdRegLen; _idx++) {
    _regarr[_idx] = _cmdarr[_idx];
  }
  _regarr[cmdRegLen] = nullTrm;
  uint8_t _register = atoi(_regarr);
  // extract control data
  // FIX: start at cmdRegLen (the character AFTER the register field). The old
  // start index (cmdDatLen) dropped the leading data digit; it only produced
  // the right answer because every command in use has a leading zero.
  char _datarr[cmdDatLen + 1];
  uint8_t _dataidx = 0;
  for (uint8_t _idx = cmdRegLen; _idx < cmdTotalLen; _idx++) {
    _datarr[_dataidx] = _cmdarr[_idx];
    _dataidx++;
  }
  _datarr[_dataidx] = nullTrm;
  uint16_t _ctldata = atoi(_datarr);
  // route message (from main serial)
  routeMessage(false, _register, _ctldata);
}

// Sends a locally originated command once the serial layer is idle, so a
// device-initiated message can never interleave with a host transaction.
void routeQueued() {
  // Only proceed if there is actually a command waiting to be sent
  if (writeCommand > 0) {
    // 1. Not waiting for an external response (serialFwrdMode)
    // 2. Not currently processing a finished message (serialMsgEnd)
    // 3. Not currently mid-character-read (serialReading)
    if (serialFwrdMode == 0 && serialMsgEnd == 0 && serialReading == 0) {
      // All clear! Route the message.
      routeMessage(true, writeRegister, writeCommand);
      // Clear the queue so we don't send it twice
      writeRegister = 0;
      writeCommand = 0;
    }
  }
}

// Routes a decoded command by its register select value:
//   preampRegister -> internalFunctions() on this device
//   ampRegister    -> transmit-only port, no response expected
//   anything else  -> DAC port, enters forwarding mode and awaits a response
// origin: false = message from main serial (forward serialMessageIn verbatim),
//         true  = message from a local device (build "RRDDD" in extMsgOut and
//                 prepend the "9,9," prefix).
void routeMessage(bool origin, uint8_t reg, uint16_t ctldata) {
  /// route data by register ///
  if (reg == preampRegister) {
    internalFunctions(ctldata);
    return;
  }
  const bool _txOnly = (reg == ampRegister);
  SoftwareSerial *_port = _txOnly ? &ExtTxOnly : &ExtTxRx;
  const char *_payload;
  uint8_t _payloadMax;
  if (origin == true) {
    // combine fixed-length register and command data ("RRDDD"), identical to
    // the previous snprintf("%02d%03d") for the 2-digit / 3-digit field widths
    extMsgOut[0] = (char)('0' + ((reg / 10) % 10));
    extMsgOut[1] = (char)('0' + (reg % 10));
    extMsgOut[2] = (char)('0' + ((ctldata / 100) % 10));
    extMsgOut[3] = (char)('0' + ((ctldata / 10) % 10));
    extMsgOut[4] = (char)('0' + (ctldata % 10));
    extMsgOut[5] = nullTrm;
    _payload = extMsgOut;
    _payloadMax = sizeof(extMsgOut);
  } else {
    _payload = serialMessageIn;
    _payloadMax = maxMessage;
  }
  // receive on the selected port (SoftwareSerial listens to one port at a time)
  if (_txOnly == false) {
    activeExtSerial = _port;
    activeExtSerial->listen();
  }
  /// send message to external serial port ///
  _port->print(serialDataStart);
  if (origin == true) {
    _port->print(F("9,9,"));  // static characters
  }
  // write characters
  for (uint8_t _idx = 0; _idx < _payloadMax; _idx++) {
    char _vchr = _payload[_idx];
    if (_vchr == nullTrm) {
      break;
    }
    _port->write(_vchr);
  }
  _port->print(serialDataEnd);
  _port->print('\n');
  if (_txOnly == true) {
    writeSerialMessage(0);
  } else {
    // FIX: only suppress the host reply when a reply is actually coming back.
    // The old code also set this on the transmit-only path, where nothing ever
    // cleared it, so the next genuine host response was swallowed.
    if (origin == true) {
      // discard response for calls from local
      disableResponse = true;
    }
    // listen for external serial reponse
    serialFwrdMode = 1;
    maxFwrdRead.set(maxFwrdWait);
    maxFwrdRead.start();
  }
}

///////////////////////////////////////////////////////////////////////////////
// relay / expander I/O
///////////////////////////////////////////////////////////////////////////////

// Drives one latching-relay board (volume or inputs). Walks the installed bits,
// builds the set/reset coil masks for the bits that changed, then pulses them.
// forced_update_flag == 1 rewrites every bit regardless of previous_state.
// Returns true when coils were actually pulsed, so the caller can start a
// settling timer only for a real switch.
bool setRelays(uint8_t pcf_a, uint8_t pcf_b,        // first pair of i2c addr's
               uint8_t new_state,                   // the 0..255 value to write
               uint8_t previous_state,              // state the board is in now
               uint8_t installed_relay_count,       // how many bits are installed
               uint8_t forced_update_flag)          // forced or relative mode (1=forced)
{
  uint8_t mask_left = 0;
  uint8_t mask_right = 0;
  // this loop walks ALL bits, even the 'mute bit'
  for (int8_t bitnum = (int8_t)(installed_relay_count - 1); bitnum >= 0; bitnum--) {
    // optimize: calc this ONLY once per loop
    const uint8_t shifted_one_bit = (uint8_t)(1 << bitnum);
    // the new value, and just the bit we are walking
    const uint8_t just_the_current_bit = (new_state & shifted_one_bit);
    // logical AND to extract just the bit we are interested in
    const uint8_t just_the_previous_bit = (previous_state & shifted_one_bit);
    // examine our current bit and see if it changed from the last run
    if (just_the_previous_bit != just_the_current_bit || forced_update_flag == 1) {
      // latch the '1' on the left or right side of the relays
      if (just_the_current_bit != 0) {
        mask_left |= shifted_one_bit;
      } else {
        mask_right |= shifted_one_bit;
      }
    }  // the 2 bits were different
  }    // for each of the installed bits
  // nothing changed: the old code still ran the full unlatch sequence, which
  // wrote 0x00 to both expanders (idempotent) and burned ~15 ms. Skipping it
  // leaves the hardware in exactly the same state.
  if (mask_left == 0 && mask_right == 0) {
    return false;
  }
  // set upper relays
  PCFexpanderWrite(pcf_b, mask_right);
  PCFexpanderWrite(pcf_a, 0x00);
  resetPCF(pcf_a, pcf_b);
  // set lower relays
  PCFexpanderWrite(pcf_a, mask_left);
  PCFexpanderWrite(pcf_b, 0x00);
  resetPCF(pcf_a, pcf_b);
  return true;
}

// Un-latch (relax) both coil sides of a relay board. The two waits are the
// coil set/hold times of the latching relays and are deliberately left as
// busy-waits: they are ~3.7 ms each and the caller cannot proceed until the
// coil has physically moved.
void resetPCF(uint8_t pcf_a, uint8_t pcf_b) {
  // let them settle before we unlatch them
  delayMicroseconds(3700);
  // do the unlatch (relax) stuff
  // left side of relay coil
  PCFexpanderWrite(pcf_a, B00000000);
  // right side of relay coil
  PCFexpanderWrite(pcf_b, B00000000);
  // let the relay hold for a while
  delayMicroseconds(3700);
}

// write a byte to PCF8574A I/O expander
void PCFexpanderWrite(uint8_t address, uint8_t _data) {
  Wire.beginTransmission(address);
  Wire.write(_data);
  Wire.endTransmission();
}

///////////////////////////////////////////////////////////////////////////////
// motor potentiometer
///////////////////////////////////////////////////////////////////////////////

// read the pot, smooth out the data
int readPotWithSmoothing(uint8_t analog_port_num, uint8_t reread_count) {
  int sensed_port_value = 0;
  for (uint8_t i = 0; i < reread_count; i++) {
    sensed_port_value += analogRead(analog_port_num);
  }
  if (reread_count > 1) {
    sensed_port_value /= reread_count;
  }
  return sensed_port_value;
}

// read the pot, and clip to keep any stray values in that range
int readPotWithClipping(int sensed_pot_value) {
  return l_map(sensed_pot_value,
               0,  // potMinRange,
               potMaxRange,
               volMin, volMax);
}

// runs when pot state changes
void potValueChanges(void) {
  // read pot smoothed output
  int sensed_pot_value = readPotWithSmoothing(potAnalogPin, potRereads);
  // read an average pot value
  if (abs(sensed_pot_value - potValueLast) > potThreshold) {
    // 1-5 is a good value to ignore noise
    // convert the pot raw value into our correct volume min..max range
    const uint8_t old_vol = volLevel;  // the setting before user touched the pot
    // read pot clipped output
    const uint8_t temp_volume = readPotWithClipping(sensed_pot_value);
    if (temp_volume == old_vol) {
      // don't update if there was no effective change
      return;
    }
    if (volRelayReady() == false) {
      // Relays still settling. Leave potValueLast untouched so this same knob
      // position is re-detected on a later pass: a fast sweep lands on its
      // final value instead of being lost.
      return;
    }
    // volume state changed
    volLevel = temp_volume;
    if (temp_volume > old_vol && volMute == 1) {
      // tell the system we are out of mute mode
      volMute = 0;
      volUpdate(volLevel, 1, true);
    } else {
      volUpdate(temp_volume, 0, true);
    }
    // real change, save the changed state
    volPotLast = volLevel;
    potValueLast = sensed_pot_value;
  }
}

// motor pot control loop
void motorControlLoop(void) {
  // given the 'IR' set volume level, find out what wiper value to compare
  const int target_pot_wiper_value = l_map(volLevel, volMin, volMax, potMinRange, potMaxRange);
  // this is the value of the pot, from the a/d converter
  const int pot_pid_value = readPotWithSmoothing(potAnalogPin, potRereads);
  if (abs(target_pot_wiper_value - pot_pid_value) <= 8) {
    // stop the motor!
    digitalWrite(motorPinCCW, LOW);  // stop turning left
    digitalWrite(motorPinCW, LOW);   // stop turning right
    potState = motorCoasting;
    // was delay(5) + delay(20); now scheduled, so the loop keeps running
    motorSampleMillis = millis();
    motorSampleWait = motorStopSettleMs;
    return;
  }
  // not at target volume yet
  if (pot_pid_value < target_pot_wiper_value) {
    // turn clockwise
    digitalWrite(motorPinCCW, LOW);
    digitalWrite(motorPinCW, HIGH);
  } else if (pot_pid_value > target_pot_wiper_value) {
    // turn counter-clockwise
    digitalWrite(motorPinCW, LOW);
    digitalWrite(motorPinCCW, HIGH);
  }
}

// motor pot state-driven dispatcher
void motorPot(void) {
  static uint8_t motor_stabilized = 0;
  if (powerState == 0) {
    return;
  }
  // If max_vol == min_vol then don't run the motor
  if (volSpan == 0) {
    return;
  }
  // action states
  switch (potState) {
    case motorInit:
      // initial state, just go to 'settled' from here
      potState = motorSettled;
      volPotLast = volLevel;
      break;
      //---------------//
    case motorSettled:
      /*
       * if we are 'settled' and the pot wiper changed, it was via a human.
       * this doesn't affect our motor-driven logic. if the volume changed via
       * the user's IR, this should trigger us to move to the next state
       */
      if (volLevel != volPotLast) {
        potState = motorInMotion;
        potValueLast = readPotWithSmoothing(potAnalogPin, potRereads);
      }
      volPotLast = volLevel;
      break;
      //---------------//
    case motorInMotion:
      /*
       * if the motor is moving, we are looking for our target
       * so we can let go of the motor and let it 'coast' to a stop
       */
      motor_stabilized = 0;
      motorControlLoop();
      break;
      //---------------//
    case motorCoasting:
      /*
       * we are waiting for the motor to stop
       * (which means the last value == this value)
       */
      if ((uint32_t)(millis() - motorSampleMillis) < motorSampleWait) {
        break;  // still settling; re-checked on a later pass
      }
      {
        const int pot_pid_value = readPotWithSmoothing(potAnalogPin, potRereads);
        if (pot_pid_value == potValueLast) {
          if (++motor_stabilized >= 5) {
            // yay! we reached our target
            potState = motorSettled;
          }
        } else {
          // we found a value that didn't match, so reset our 'sameness' counter
          motor_stabilized = 0;
        }
        // this is the operating value of the pot, from the a/d converter
        potValueLast = pot_pid_value;
      }
      motorSampleMillis = millis();
      motorSampleWait = motorCoastSampleMs;
      break;
      //
    default:
      break;
  }
}

///////////////////////////////////////////////////////////////////////////////
// volume, input and filter control
///////////////////////////////////////////////////////////////////////////////

// increment volume up/down slow/fast
void volIncrement(uint8_t dir_flag, uint8_t speed_flag) {
  if (dir_flag != volControlUp && dir_flag != volControlDown) {
    return;
  }
  if (speed_flag != volControlSlow && speed_flag != volControlFast) {
    return;
  }
  if (dir_flag == volControlUp && volLevel >= volMax) {
    return;
  }
  if (dir_flag == volControlDown && volLevel <= volMin) {
    return;
  }
  if (volRelayReady() == false) {
    return;  // relays still settling; drop this step rather than stack it up
  }
  if (volMute == 1) {
    volMute = 0;
    volUpdate(volLevel, 1, true);
    return;
  }
  const int16_t step = (speed_flag == volControlFast) ? volCoarseSteps : 1;
  if (dir_flag == volControlUp) {
    volLevel = ((int16_t)volLevel + step < (int16_t)volMax)
                 ? (uint8_t)(volLevel + step)
                 : volMax;
  } else {
    volLevel = ((int16_t)volLevel - step > (int16_t)volMin)
                 ? (uint8_t)(volLevel - step)
                 : volMin;
  }
  volUpdate(volLevel, 0, true);
}

// Converts a raw attenuator level to the 0-100 user scale. This is the ONLY
// place the conversion is performed, so the panel and the host reply cannot
// drift apart. Clamped, and safe before the limits have been calculated.
uint8_t volToPercent(uint8_t _level) {
  if (volMax <= volMin) {
    return 0;
  }
  long _pct = l_map(_level, volMin, volMax, 0, 100);
  if (_pct < 0) {
    _pct = 0;
  } else if (_pct > 100) {
    _pct = 100;
  }
  return (uint8_t)_pct;
}

// True when the attenuator relays have finished their settling gap. Self-
// clearing: once the window has elapsed the flag is dropped, so the check is
// a single comparison on the hot path.
bool volRelayReady() {
  if (volRelayResting == false) {
    return true;
  }
  if ((uint32_t)(millis() - volRelayRestMillis) < volRelayRestMs) {
    return false;
  }
  volRelayResting = false;
  return true;
}

// Set a specific volume level. _redraw == false drives the relays and updates
// the stored percentage but leaves the panel alone, for callers that are about
// to paint something else over it (the mute screen).
void volUpdate(uint8_t _vol, uint8_t _force, bool _redraw) {
  // set volume relays
  if (volMute == 0) {
    if (setRelays(volSetAddr, volResetAddr, _vol, volLastLevel, volRelayCount, _force)) {
      // coils moved: hold everything off them until they have settled
      volRelayRestMillis = millis();
      volRelayResting = true;
    }
    volLastLevel = volLevel;
    // one conversion, reused by the panel below and by writeVolumeStatus()
    volPercent = volToPercent(volLastLevel);
    if (_redraw == false) {
      return;
    }
    // update display
    lcdVolume(volPercent);  // redraw progress bar and percentage
    if (lcdPopupActive == false) {
      lcd.setCursor(0, 1);  // music icon
      lcd.write(glyphSound);
      lcd.write(' ');
    }
    // update input
    updateInputDisplay(inputSelected);
  }
}

// mute system (toggle)
void muteSystem() {
  if (volMute == 0) {  // mute
    if (volSpan == 0) {
      return;
    }  // stop motor turning
    digitalWrite(motorPinCW, LOW);
    digitalWrite(motorPinCCW, LOW);
    // set volume to min (relays and volPercent only; the mute screen below
    // overwrites every cell volUpdate would have drawn)
    volUpdate(volMin, 1, false);
    // mute status on display
    lcd.setCursor(0, 0);
    lcd.print(F("----------------"));
    lcdBarLastBlock = 0xFF;  // volume bar row was overwritten
    lcdBarLastTip = 0xFF;
    lcdInputLastDrawn = 0xFF;  // input name was overwritten
    lcdVolLastPct = -1;
    if (lcdPopupActive == false) {
      lcdSlideAbort();  // drop any label mid-flight before repainting the row
      lcd.setCursor(2, 1);
      lcd.print(F("          "));
      lcd.setCursor(0, 1);
      lcd.write(glyphSpeaker);
      lcdSlideStart(12, 1, 4, F("MUTE"), lcdSlideFromRight);
    }
    // set mute flag
    volMute = 1;
  } else {
    volMute = 0;
    lcdSlideAbort();      // MUTE label may still be in flight
    lcd.setCursor(0, 0);  // clear top of screen
    lcd.print(F("                "));
    lcdBarLastBlock = 0xFF;
    lcdBarLastTip = 0xFF;
    volUpdate(volLevel, 1, true);
  }
}

// set a specific audio input
void inputUpdate(uint8_t _input) {
  if (_input > inputCount) {
    return;
  }
  // set last input selected
  if (_input == 0) {
    _input = inputSelected;
  } else {
    // save input state
    inputSelected = _input;
  }
  uint8_t _relayMask = 0;
  uint16_t _dacCommand = 0;
  switch (_input) {
    case 1:  // USB
      _dacCommand = 1;
      _relayMask = B00000001;
      break;
    case 2:
      _relayMask = B00000010;
      break;
    case 3:
      _relayMask = B00000100;
      break;
    case 4:  // Coax
      _dacCommand = 3;
      _relayMask = B00000001;
      break;
    case 5:  // Opt
      _dacCommand = 2;
      _relayMask = B00000001;
      break;
    case 6:  // Auto
      _dacCommand = 4;
      _relayMask = B00000001;
      break;
    default:
      break;
  }
  // set decoder input (queued, sent by routeQueued() once serial is idle)
  if (_dacCommand > 0) {
    writeRegister = dacRegister;
    writeCommand = _dacCommand;
  }
  // select input relay
  if (_relayMask > 0) {
    setInputRelays(_relayMask);
  }
  updateInputDisplay(_input);
}

void setInputRelays(uint8_t state) {
  // forced mode, so the previous-state argument is not consulted
  setRelays(inputSetAddr, inputResetAddr, state, 0, inputRelayCount, 1);
}

void updateInputDisplay(uint8_t input) {
  if (input == 0 || input > inputCount) {
    return;  // guard: inputNames is 1-based on the caller side
  }
  if (lcdPopupActive == true) {
    lcdInputLastDrawn = 0xFF;  // popup owns the row; redrawn when it expires
    return;
  }
  if (input == lcdInputLastDrawn) {
    return;  // already on screen, skip 10 characters of I2C traffic
  }
  lcdInputLastDrawn = input;
  lcdSlideStart(2, 1, inputNameLen,
                reinterpret_cast<const __FlashStringHelper *>(inputNames[input - 1]),
                lcdSlideFromRight);
}

// high-pass filter control
void hpfControl(uint8_t _state) {
  if (volMute != 0) {
    return;
  }
  if (_state >= 3) {  // invalid
    return;
  }
  if (_state == 0) {  // HPF on
    hpfState = 0;
  } else if (_state == 1) {  // HPF off
    hpfState = 1;
  } else {  // toggle state
    hpfState = !hpfState;
  }
  if (hpfState == 0) {
    showMessage(F("HPF Off   "));
    digitalWrite(hpfRelayPin, HIGH);
  } else {
    showMessage(F("HPF On    "));
    digitalWrite(hpfRelayPin, LOW);
  }
}

// Drops any pending temporary-message timeout. Called on every power
// transition so a message armed while the unit was on cannot repaint the
// input name over the shutdown/standby screen once the sequence finishes.
void cancelInputMessage() {
  delayDrawInputs.reset();
  delayDrawInputs.stop();
}

// temporary status text in the input-name field
void showMessage(const __FlashStringHelper *message) {
  lcdInputLastDrawn = 0xFF;  // input name was overwritten
  if (lcdPopupActive == false) {
    lcdSlideStart(2, 1, inputNameLen, message, lcdSlideFromRight);
  }
  delayDrawInputs.start();
}

void showHeartMessage() {
  if (powerState == 0 && lcdPopupActive == false) {
    lcdSlideAbort();
    analogWrite(lcdBacklightPin, lcdOnBrightness);
    lcd.setCursor(0, 1);
    lcd.print(F("I         "));
    lcd.setCursor(2, 1);
    lcd.write(glyphHeart);
    lcd.setCursor(4, 1);
    lcd.print(F("Nicky"));
    lcdInputLastDrawn = 0xFF;
  }
}

///////////////////////////////////////////////////////////////////////////////
// display
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// reusable animation primitives
///////////////////////////////////////////////////////////////////////////////

// Begin spinning a single cell. Reusable anywhere a cell is free. Borrows
// lcdSpinnerGlyphSlot for the backslash frame until lcdSpinnerStop().
void lcdSpinnerStart(uint8_t _col, uint8_t _row) {
  uint8_t _glyph[8];
  memcpy_P(_glyph, lcdSpinnerBackslash, sizeof(_glyph));
  lcd.createChar(lcdSpinnerGlyphSlot, _glyph);
  lcdSpinnerCol = _col;
  lcdSpinnerRow = _row;
  lcdSpinnerFrame = 0;
  lcdSpinnerActive = true;
  lcdSpinnerMillis = millis() - lcdSpinnerStepMs;  // draw the first frame now
}

// One character of traffic per frame.
void lcdSpinnerStep() {
  if (lcdSpinnerActive == false) {
    return;
  }
  if ((uint32_t)(millis() - lcdSpinnerMillis) < lcdSpinnerStepMs) {
    return;
  }
  lcdSpinnerMillis = millis();
  lcd.setCursor(lcdSpinnerCol, lcdSpinnerRow);
  lcd.write(pgm_read_byte(&lcdSpinnerFrames[lcdSpinnerFrame]));
  lcdSpinnerFrame++;
  if (lcdSpinnerFrame >= lcdSpinnerFrameCount) {
    lcdSpinnerFrame = 0;
  }
}

// _clearCell blanks the cell on the way out; skip it when the caller is about
// to clear or repaint the whole area anyway.
void lcdSpinnerStop(bool _clearCell) {
  if (lcdSpinnerActive == true) {
    if (_clearCell == true) {
      // blank the cell before handing the slot back, so the restored glyph
      // cannot flash in the spinner's place
      lcd.setCursor(lcdSpinnerCol, lcdSpinnerRow);
      lcd.write(' ');
    }
    uint8_t _glyph[8];
    memcpy_P(_glyph, customChars[lcdSpinnerGlyphSlot], sizeof(_glyph));
    lcd.createChar(lcdSpinnerGlyphSlot, _glyph);
  }
  lcdSpinnerActive = false;
}

// Slide a flash string into a fixed-width field, one column per frame.
// Trailing padding spaces are trimmed so the motion is visible from frame one.
// _dir selects which edge the text enters from; lcdSlideFromLeft means the
// text travels rightwards, so its trailing edge is what appears first.
void lcdSlideStart(uint8_t _col, uint8_t _row, uint8_t _width,
                   const __FlashStringHelper *_text, uint8_t _dir) {
  // a field already in motion is snapped to its final frame, never abandoned
  lcdSlideFinish();
  if (_width > lcdSlideMaxWidth) {
    _width = lcdSlideMaxWidth;
  }
  strncpy_P(lcdSlideText, (const char *)_text, lcdSlideMaxWidth);
  lcdSlideText[lcdSlideMaxWidth] = nullTrm;
  uint8_t _len = strlen(lcdSlideText);
  while (_len > 0 && lcdSlideText[_len - 1] == ' ') {
    _len--;  // trim the padding the label carries
  }
  lcdSlideText[_len] = nullTrm;
  lcdSlideCol = _col;
  lcdSlideRow = _row;
  lcdSlideWidth = _width;
  lcdSlideLen = _len;
  lcdSlideDir = _dir;
  if (_dir == lcdSlideFromRight) {
    // travels the full width of the field, so timing is label-independent
    lcdSlideOffset = (uint8_t)(_width - 1);
  } else {
    lcdSlideOffset = (_len > 0) ? (uint8_t)(_len - 1) : 0;
  }
  lcdSlideActive = true;
  lcdSlideMillis = millis() - lcdSlideStepMs;  // draw the first frame now
}

// Paints the whole field with the text shifted by _offset columns.
void lcdSlideDraw(uint8_t _offset) {
  lcd.setCursor(lcdSlideCol, lcdSlideRow);
  for (uint8_t _x = 0; _x < lcdSlideWidth; _x++) {
    int16_t _src;
    if (lcdSlideDir == lcdSlideReveal) {
      // characters stay at their landed column and are uncovered left to right
      _src = (((int16_t)_x + (int16_t)_offset) < (int16_t)lcdSlideLen)
               ? (int16_t)_x
               : -1;
    } else if (lcdSlideDir == lcdSlideFromLeft) {
      _src = (int16_t)_x + (int16_t)_offset;
    } else {
      _src = (int16_t)_x - (int16_t)_offset;
    }
    if (_src >= 0 && _src < (int16_t)lcdSlideLen) {
      lcd.write(lcdSlideText[_src]);
    } else {
      lcd.write(' ');
    }
  }
}

// Returns true once the label has landed.
bool lcdSlideStep() {
  if (lcdSlideActive == false) {
    return true;
  }
  if ((uint32_t)(millis() - lcdSlideMillis) < lcdSlideStepMs) {
    return false;
  }
  lcdSlideMillis = millis();
  lcdSlideDraw(lcdSlideOffset);
  if (lcdSlideOffset == 0) {
    lcdSlideActive = false;
    return true;
  }
  lcdSlideOffset--;
  return false;
}

// Jump straight to the landed frame.
void lcdSlideFinish() {
  if (lcdSlideActive == false) {
    return;
  }
  lcdSlideDraw(0);
  lcdSlideActive = false;
}

// Drop the animation without drawing (the caller is repainting the area).
void lcdSlideAbort() {
  lcdSlideActive = false;
}

// Banner across the whole bottom row for lcdPopupHoldMs. While it is up every
// other bottom-row painter stands down; the row is rebuilt from live state
// when it expires, so state may change freely underneath it.
void lcdPopupStart(const __FlashStringHelper *_msg) {
  lcdSlideAbort();
  char _line[lcdCols + 1];
  strncpy_P(_line, (const char *)_msg, lcdCols);
  _line[lcdCols] = nullTrm;
  uint8_t _len = strlen(_line);
  while (_len < lcdCols) {
    _line[_len] = ' ';  // pad so the banner covers the row
    _len++;
  }
  _line[lcdCols] = nullTrm;
  lcd.setCursor(0, 1);
  lcd.print(_line);
  lcdInputLastDrawn = 0xFF;
  lcdVolLastPct = -1;
  lcdPopupMillis = millis();
  lcdPopupActive = true;
}

void lcdPopupStep() {
  if (lcdPopupActive == false) {
    return;
  }
  if ((uint32_t)(millis() - lcdPopupMillis) < lcdPopupHoldMs) {
    return;
  }
  lcdPopupActive = false;
  lcdDrawBottomRow();
}

// Rebuilds the bottom row from current state. Used to restore the display
// after a popup; also a single place that documents the row layout.
void lcdDrawBottomRow() {
  lcdSlideAbort();
  lcdInputLastDrawn = 0xFF;
  lcdVolLastPct = -1;
  if (powerState == 0) {
    lcd.setCursor(0, 1);
    lcd.print(F("Stereo Preamp  "));  // cols 0-14
    lcd.write(glyphSpeaker);          // col 15
    return;
  }
  lcd.setCursor(0, 1);
  if (volMute == 1) {
    lcd.write(glyphSpeaker);
    lcd.write(' ');
    lcd.print(F("          "));  // cols 2-11
    lcd.print(F("MUTE"));        // cols 12-15
    return;
  }
  lcd.write(glyphSound);
  lcd.write(' ');
  updateInputDisplay(inputSelected);  // cols 2-11, slides back in
  lcdVolume(volPercent);              // cols 12-15 (bar row is cached)
}

// clear the panel and drop every cached "what is currently drawn" value
void lcdClear() {
  lcdSlideAbort();
  lcdPopupActive = false;
  lcd.clear();
  // after the panel is blank, so restoring the borrowed glyph is invisible
  lcdSpinnerStop(false);
  lcdBarLastBlock = 0xFF;
  lcdBarLastTip = 0xFF;
  lcdInputLastDrawn = 0xFF;
  lcdVolLastPct = -1;
}

// Volume status on display. Takes the already-converted 0-100 value so both
// the number and the bar are driven by exactly what writeVolumeStatus() sends.
void lcdVolume(uint8_t _pct) {
  if (lcdPopupActive == true) {
    lcdVolLastPct = -1;  // repainted when the popup expires
  } else if ((int16_t)_pct != lcdVolLastPct) {
    lcdVolLastPct = _pct;
    // columns 12..15 in one write: " MAX", " 42%", " 5 %"
    char _pctText[5];
    _pctText[0] = ' ';
    if (_pct >= 100) {
      _pctText[1] = 'M';
      _pctText[2] = 'A';
      _pctText[3] = 'X';
    } else if (_pct >= 10) {
      _pctText[1] = (char)('0' + (_pct / 10));
      _pctText[2] = (char)('0' + (_pct % 10));
      _pctText[3] = '%';
    } else {
      _pctText[1] = (char)('0' + _pct);
      _pctText[2] = ' ';
      _pctText[3] = '%';
    }
    _pctText[4] = nullTrm;
    lcd.setCursor(12, 1);
    lcd.print(_pctText);
  }
  // draw the progress bar from the same percentage
  lcdBar(0, _pct, 0, 100);
}

// horizontal bar graph across one whole row
void lcdBar(uint8_t row, int16_t var, int16_t minVal, int16_t maxVal) {
  const int16_t constrainedVar = constrain(var, minVal, maxVal);
  // Scale to 16 blocks and 80 "sub-steps" (16 blocks * 5 segments)
  const uint8_t block = map(constrainedVar, minVal, maxVal, 0, lcdCols);
  const uint8_t line = map(constrainedVar, minVal, maxVal, 0, lcdCols * 5);
  const uint8_t bar = (uint8_t)(line - (block * 5));
  if (block == lcdBarLastBlock && bar == lcdBarLastTip) {
    return;  // identical to what is already on the panel
  }
  lcdBarLastBlock = block;
  lcdBarLastTip = bar;
  // one setCursor, then let the controller auto-increment across the row
  lcd.setCursor(0, row);
  for (uint8_t x = 0; x < lcdCols; x++) {
    if (x < block) {
      lcd.write(glyphBarFull);  // solid block
    } else if (x == block && bar > 0) {
      lcd.write(bar);  // partial "tip": bar1..bar4
    } else {
      lcd.write(' ');
    }
  }
}

// Arms the timed progress bar. It advances one segment per lcdTimedBarStep()
// call, spending (_sec * 6) ms per segment, exactly like the old blocking
// version -- but the superloop keeps running while it plays.
void lcdTimedBarStart(uint8_t _sec, uint8_t _rows) {
  barStepMs = (uint16_t)_sec * 6;
  barRowsLeft = _rows;
  barLine = 1;
  barCol = 0;
  barSeg = 0;
  barStepMillis = millis() - barStepMs;  // draw the first segment immediately
}

// Draws at most one bar segment; returns true once the animation is finished.
bool lcdTimedBarStep() {
  if (barRowsLeft == 0) {
    return true;
  }
  if ((uint32_t)(millis() - barStepMillis) < barStepMs) {
    return false;
  }
  lcd.setCursor(barCol, barLine);
  // draw custom segment
  lcd.write(barSeg + 1);
  // also reset PCF expanders
  resetPCF(volSetAddr, volResetAddr);
  resetPCF(inputSetAddr, inputResetAddr);
  barStepMillis = millis();
  // advance segment -> column -> row
  barSeg++;
  if (barSeg >= 5) {
    barSeg = 0;
    barCol++;
    if (barCol >= lcdCols) {
      barCol = 0;
      if (barLine > 0) {
        barLine--;  // decrement to next row
      }
      barRowsLeft--;
      if (barRowsLeft == 0) {
        lcdBarLastBlock = 0xFF;  // the bar row was overwritten
        lcdBarLastTip = 0xFF;
        return true;
      }
    }
  }
  return false;
}

// Blocking wrapper, used only from setup() where there is nothing else to do.
void lcdTimedBarBlocking(uint8_t _sec, uint8_t _rows) {
  lcdTimedBarStart(_sec, _rows);
  while (lcdTimedBarStep() == false) {
    // spin: setup() has no other work pending
  }
}

// display in standby mode (no wait; the sequencer holds the splash)
void lcdStandbyDraw() {
  analogWrite(lcdBacklightPin, lcdOffBrightness);
  lcdClear();
  lcd.setCursor(0, 0);
  lcd.print(F("ProDesigns"));
  lcd.setCursor(0, 1);
  lcd.print(F("Stereo Preamp"));
  lcd.setCursor(15, 1);
  lcd.write(glyphSpeaker);
}

///////////////////////////////////////////////////////////////////////////////
// power sequencing
///////////////////////////////////////////////////////////////////////////////

// Debounces the front-panel button and kicks off a power sequence when the
// power state changes. Skipped while a sequence is already running, which is
// how the old blocking startup()/shutdown() behaved.
void setPowerState() {
  if (powerSeqState != pwrSeqIdle) {
    return;
  }
  // the button lives behind the MCP23008, so rate-limit the I2C poll
  if ((uint32_t)(millis() - powerButtonPollMillis) >= buttonPollMs) {
    powerButtonPollMillis = millis();
    // read pin state from MCP23008
    const uint8_t reading = lcd.readPin(powerButtonPin);
    // if switch changed
    if (reading != lastPowerButton) {
      // reset the debouncing timer
      powerButtonMillis = millis();
    }
    if ((millis() - powerButtonMillis) > debounceDelay) {
      // if the button state has changed:
      if (reading != powerButton) {
        powerButton = reading;
        // power state has changed!
        if (powerButton == 1) {
          powerState = !powerState;
          powerCycle = 1;
        }
      }
    }
    lastPowerButton = reading;
  }
  // power state actions
  if (powerCycle == 1) {
    // reset display
    lcdClear();
    // one-shot triggers
    if (powerState == 1) {
      /// runs once on boot ///
      startupBegin();
    } else {
      /// runs once on shutdown ///
      shutdownBegin();
    }
    powerCycle = 0;
  }
}

// Advances the startup/shutdown state machine. Each branch performs exactly
// the work the old blocking routines did, at the same points in time.
void powerSequenceStep() {
  switch (powerSeqState) {
    case pwrSeqIdle:
      return;
    //---------------//
    case pwrSeqStartLimitMsg:
      if ((uint32_t)(millis() - powerSeqMillis) < startLimitMsgMs) {
        return;
      }
      lcdClear();
      lcd.setCursor(0, 0);
      lcd.print(F("Starting Up"));
      lcdSpinnerStart(lcdCols - 1, 0);  // top right
      lcdTimedBarStart(startDelay, 1);
      powerSeqState = pwrSeqStartBar;
      return;
    //---------------//
    case pwrSeqStartBar:
      if (lcdTimedBarStep() == false) {
        return;
      }
      lcdClear();
      {
        // calculate volume min/max
        const uint8_t max_byte_size = (uint8_t)((1 << volRelayCount) - 1);
        volMin = (uint8_t)(((uint16_t)max_byte_size * volMinLimit) / 100);
        // volume limiter
        if (volLimitState == 1) {
          volMax = (uint8_t)(((uint16_t)max_byte_size * volMaxLimit) / 100);
        } else {
          volMax = max_byte_size;
        }
      }
      // calculate volume span
      volSpan = (uint8_t)abs((int16_t)volMax - (int16_t)volMin);
      // read volume level from pot
      potState = motorInit;
      // initialize and unmute
      muteSystem();
      // allow relays to settle
      powerSeqMillis = millis();
      powerSeqState = pwrSeqStartSettle;
      return;
    //---------------//
    case pwrSeqStartSettle:
      if ((uint32_t)(millis() - powerSeqMillis) < startSettleMs) {
        return;
      }
      // set last selected input
      inputUpdate(0);
      powerSeqState = pwrSeqIdle;
      return;
    //---------------//
    case pwrSeqShutBar:
      if (lcdTimedBarStep() == false) {
        return;
      }
      lcdStandbyDraw();
      powerSeqMillis = millis();
      powerSeqState = pwrSeqShutStandby;
      return;
    //---------------//
    case pwrSeqShutStandby:
      if ((uint32_t)(millis() - powerSeqMillis) < standbyHoldMs) {
        return;
      }
      digitalWrite(LED_BUILTIN, LOW);
      // turn off analog stages
      digitalWrite(powerRelayPin, LOW);
      powerSeqState = pwrSeqIdle;
      return;
    //---------------//
    default:
      powerSeqState = pwrSeqIdle;
      return;
  }
}

// startup routines (first stage; the rest runs from powerSequenceStep)
void startupBegin() {
  // no stale temporary message may survive the transition
  cancelInputMessage();
  // set display brightness
  analogWrite(lcdBacklightPin, lcdOnBrightness);
  // turn on analog stages
  digitalWrite(powerRelayPin, HIGH);
  // mute when not muted
  if (volMute == 0) {
    muteSystem();
  }
  // loading bar
  lcdClear();
  lcd.setCursor(0, 0);
  if (volLimitState == 1) {
    lcd.print(F("Volume Limit On"));
  } else {
    lcd.print(F("Volume Limit Off"));
  }
  powerSeqMillis = millis();
  powerSeqState = pwrSeqStartLimitMsg;
}

// shutdown routines (first stage; the rest runs from powerSequenceStep)
void shutdownBegin() {
  // no stale temporary message may survive the transition
  cancelInputMessage();
  // mute when not muted
  if (volMute == 0) {
    muteSystem();
  }
  // shutdown external amplifier
  routeMessage(true, ampRegister, 1);
  // shutdown animation
  lcdClear();
  lcd.setCursor(0, 0);
  lcd.print(F("Shutting Down"));
  lcdSpinnerStart(lcdCols - 1, 0);  // top right
  lcdTimedBarStart(shutdownTime, 1);
  powerSeqState = pwrSeqShutBar;
}

///////////////////////////////////////////////////////////////////////////////
// initialization
///////////////////////////////////////////////////////////////////////////////

void initSerial() {
  Serial.begin(serialBaudRate);
  pinMode(AMP_RX_PIN, INPUT);
  pinMode(AMP_TX_PIN, OUTPUT);
  ExtTxOnly.begin(serialBaudRate);
  pinMode(DAC_RX_PIN, INPUT);
  pinMode(DAC_TX_PIN, OUTPUT);
  ExtTxRx.begin(serialBaudRate);
  // SoftwareSerial receives on one port at a time; ExtTxOnly never receives
  activeExtSerial = &ExtTxRx;
  activeExtSerial->listen();
  serialMessageIn[0] = nullTrm;
  serialMessageOut[0] = nullTrm;
  extMsgOut[0] = nullTrm;
}

void initTimers() {
  maxFwrdRead.reset();
  maxFwrdRead.stop();
  delayDrawInputs.reset();
  delayDrawInputs.stop();
}

void initGPIO() {
  // LED status LED
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  // motor pot
  pinMode(potAnalogPin, INPUT);
  pinMode(motorPinCW, OUTPUT);
  pinMode(motorPinCCW, OUTPUT);
  digitalWrite(motorPinCW, LOW);
  digitalWrite(motorPinCCW, LOW);
  // display backlight
  pinMode(lcdBacklightPin, OUTPUT);
  analogWrite(lcdBacklightPin, lcdOffBrightness);
  // preamp power control
  pinMode(powerRelayPin, OUTPUT);
  digitalWrite(powerRelayPin, LOW);
  // HPF control
  pinMode(hpfRelayPin, OUTPUT);
  digitalWrite(hpfRelayPin, HIGH);
}

void initDisplay() {
  // icons and bar segments, copied one at a time out of flash
  uint8_t _glyph[8];
  for (uint8_t _slot = 0; _slot < 8; _slot++) {
    memcpy_P(_glyph, customChars[_slot], sizeof(_glyph));
    lcd.createChar(_slot, _glyph);
  }
}

void setup() {
  // timers
  initTimers();
  // 16x2 display (calls Wire.begin)
  lcd.begin(lcdCols, lcdRows);
  // reset expanders
  resetPCF(volSetAddr, volResetAddr);
  resetPCF(inputSetAddr, inputResetAddr);
  // pins
  initGPIO();
  // icons
  initDisplay();
  // initial boot display
  lcdTimedBarBlocking(initStartDelay, 2);
  lcdClear();
  delay(100);
  // serial support
  initSerial();
  // startup complete
  lcdStandbyDraw();
  digitalWrite(LED_BUILTIN, LOW);
}

///////////////////////////////////////////////////////////////////////////////
// superloop
///////////////////////////////////////////////////////////////////////////////

void loop() {
  // power on/off animation and sequencing
  powerSequenceStep();
  // reusable display animations (each a no-op while idle)
  lcdSpinnerStep();
  lcdSlideStep();
  lcdPopupStep();
  // power management
  setPowerState();
  if (powerSeqState == pwrSeqIdle && powerState == 1) {
    // motor potentiometer
    if (potState == motorSettled || potState == motorInit) {
      potValueChanges();
    }
    motorPot();
  }
  // communication
  serialProcess();
  routeQueued();
  // update inputs on display (never over the standby screen)
  if (powerState == 1 && powerSeqState == pwrSeqIdle && delayDrawInputs.done()) {
    updateInputDisplay(inputSelected);
    cancelInputMessage();
  }
}
