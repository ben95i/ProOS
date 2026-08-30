///////////////////////////////////////////////////////////////////////////////
// Preamp Controller
// by Ben Provenzano III
// v3.0 06/11/2023
// v4.3 03/29/2026
///////////////////////////////////////////////////////////////////////////////

// shared libraries
#include <SoftwareSerial.h>
#include <Arduino.h>
#include <Wire.h>

// local libraries
#include "NeoTimer.h"
#include "LiquidCrystal_I2C.h"  // custom for MCP23008-E/P, power button support

// I2C addresses
#define inputResetAddr 0x3A
#define inputSetAddr 0x3B
#define volResetAddr 0x3E
#define volSetAddr 0x3F
#define lcdAddr 0x27

// general constants
const char nullTrm = '\0';

// 16x2 Display
LiquidCrystal_I2C lcd(lcdAddr);
uint8_t lcdCols = 16;            // number of columns in the LCD
uint8_t lcdRows = 2;             // number of rows in the LCD
#define lcdBacklightPin 9        // display backlight pin
uint8_t lcdOffBrightness = 140;  // standby-off LCD brightness level ***
uint8_t lcdOnBrightness = 255;   // online LCD brightness level ***
// custom characters
uint8_t bar1[8] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10 };
uint8_t bar2[8] = { 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18 };
uint8_t bar3[8] = { 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C };
uint8_t bar4[8] = { 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E };
uint8_t bar5[8] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F };
uint8_t speaker[8] = { B00001, B00011, B01111, B01111, B01111, B00011, B00001, B00000 };
uint8_t sound[8] = { B00001, B00011, B00101, B01001, B01001, B01011, B11011, B11000 };
uint8_t heart[8] = { B00000, B01010, B11111, B11111, B01110, B00100, B00000, B00000 };
Neotimer delayDrawInputs = Neotimer(4000);  // pause for 4-seconds before showing

// serial resources
#define serialBaudRate 9600
const uint8_t preampRegister = 15;
const uint8_t dacRegister = 16;
const uint8_t ampRegister = 17;
#define DAC_RX_PIN 11  // 232-RX 3.5mm TRS Ring (DAC)
#define DAC_TX_PIN 12  // 232-TX 3.5mm TRS Tip (DAC)
SoftwareSerial ExtTxRx(DAC_RX_PIN, DAC_TX_PIN);
#define AMP_RX_PIN 6  // Not Used
#define AMP_TX_PIN 8  // 232-TX RCA Jack (Class-D Amp)
SoftwareSerial ExtTxOnly(AMP_RX_PIN, AMP_TX_PIN);
const int16_t offCode = -1;
const uint8_t maxMessage = 16;
const uint8_t cmdRegLen = 2;  // register length (99***)
const uint8_t cmdDatLen = 3;  // data length (**999)
const char serialDataStart = '<';
const char serialDataEnd = '>';
const char respDelimiter = '|';
char serialMessageOut[maxMessage + 1];
char serialMessageIn[maxMessage + 1];
uint16_t serialCurPos = 0;
bool serialFwrdMode = 0;
bool serialReading = 0;
bool serialMsgEnd = 0;
const uint16_t maxFwrdWait = 500;  // max wait in (ms) for external serial response
Neotimer maxFwrdRead = Neotimer();
uint8_t writeRegister = 0;
uint16_t writeCommand = 0;
bool disableResponse = 0;

// input selector
bool setDecoder = 0;
uint8_t inputSelected = 1;
uint8_t inputRelayCount = 3;
static const char *inputNames[] = {
  ("DIGI PC   "),
  ("AUX       "),
  ("PHONO     "),
  ("COAXIAL   "),
  ("OPTICAL   "),
  ("DIGI AUTO ")
};

// volume control
uint8_t volCoarseSteps = 6;  // volume steps to skip for coarse adjust ***
uint8_t volMaxLimit = 90;    // max percentage of volume range (100%) when in limit mode ***
uint8_t volMinLimit = 40;    // min percentage of volume range (0%)
uint8_t volRelayCount = 8;   // number of relays on volume attenuator board ***
bool volLimitState = 1;      // limit on by default
#define volControlDown 1
#define volControlUp 2
#define volControlSlow 1
#define volControlFast 2
uint8_t volLastLevel = 0;
uint8_t volLevel = 0;
uint8_t volSpan = 0;
uint8_t volMax = 0;
uint8_t volMin = 0;
bool volMute = 0;

// high-pass filter
#define hpfRelayPin 10  // HPF relay pin
bool hpfState = 0;

// motor pins
#define potAnalogPin 1  // pot reading pin A1
#define motorPinCW 16   // motor H-bridge CW pin
#define motorPinCCW 17  // motor H-bridge CCW pin
// motor resources
#define motorInit 1       // motor initialization
#define motorSettled 2    // pot at resting state
#define motorInMotion 3   // pot is moving right now
#define motorCoasting 4   // pot just passed read value
#define potThreshold 4    // pot value change threshold ***
#define potRereads 6      // amount of pot readings ***
#define potMinRange 30    // lowest pot reading
#define potMaxRange 1023  // highest pot reading
int potValueLast = 0;     // range from 0-1023
uint8_t volPotLast = 0;
uint8_t potState = 0;

// power pins
#define powerRelayPin 7
#define powerButtonPin 5
// power resources
int startDelay = 5;      // startup delay in seconds, unmutes after ***
int shutdownTime = 3;    // shutdown delay before turning off aux power ***
int initStartDelay = 2;  // delay on initial cold start ***
uint8_t debounceDelay = 50;  // button debounce delay in ms
uint32_t powerButtonMillis;
uint8_t lastPowerButton = 0;
uint8_t powerButton = 0;
bool powerCycle = 0;
bool powerState = 0;

///////////////////////////////////////////////////////////////////////////////

// main control functions
void internalFunctions(uint16_t command) {
  // write status back to serial
  switch (command) {
    // PC input
    case 1:
      setInput(command);
      break;
    // aux input
    case 2:
      setInput(command);
      break;
    // phono input
    case 3:
      setInput(command);
      break;
    // coaxial input
    case 4:
      setInput(command);
      break;
    // optical input
    case 5:
      setInput(command);
      break;
    // auto input mode
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
      volIncrement(1, 2);
      writeVolumeStatus();
      break;
      // volume up fast (VOL+)
    case 9:
      if (checkPreampState() == false) { break; } 
      volIncrement(2, 2);
      writeVolumeStatus();
      break;
    // volume down slow
    case 10:
      if (checkPreampState() == false) { break; } 
      volIncrement(1, 1);
      writeVolumeStatus();
      break;
    // volume up slow
    case 11:
      if (checkPreampState() == false) { break; } 
      volIncrement(2, 1);
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

bool checkPreampState() {
  if (powerState == 0) { // offline
    writeSerialMessage(offCode);
    return false;
  }
  if (volMute == 1) { // muted
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
  writeSerialMessage(l_map(volLastLevel, volMin, volMax, 0, 100));
}

void writeTwoStateSerialMessage(int16_t state1, int16_t state2) {
  serialMessageOut[0] = nullTrm;
  snprintf(serialMessageOut, maxMessage, "%d~%d", state1, state2);
}

void writeSerialMessage(int16_t value) {
  serialMessageOut[0] = nullTrm;
  snprintf(serialMessageOut, maxMessage, "%d", value);
}

// reverse byte function
int reverseByte(uint8_t _inByte) {
  uint8_t _revByte = 0;
  for (int _revBit = 0; _revBit < 8; _revBit++) {
    if (bitRead(_inByte, _revBit) == 1) {
      bitWrite(_revByte, (7 - _revBit), 1);
    } else {
      bitWrite(_revByte, (7 - _revBit), 0);
    }
  }
  return _revByte;
}

// # mapping function
long l_map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min + 1) / (in_max - in_min + 1) + out_min;
}

//// RS-232 logic ////

void resetRxTxBuffers() {
  serialMessageOut[0] = nullTrm;
  serialMessageIn[0] = nullTrm;
  serialMsgEnd = 0;
}

void disableFwrdMode() {
  serialFwrdMode = 0;
  maxFwrdRead.reset();
  maxFwrdRead.stop();
}

void serialProcess() {
  // read main serial port
  if (serialFwrdMode == 0) {
    if (serialMsgEnd == 0) {
      if (Serial.available()) {
        readSerialData(Serial.read(), serialDataStart, serialDataEnd);
      }
    } else {
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
    // listen to external serial port when in forwarding mode
    if (serialMsgEnd == 0) {
      if (ExtTxRx.available()) {
        readSerialData(ExtTxRx.read(), respDelimiter, respDelimiter);
      }
    } else {
      // forward external serial reponse to main serial
      writeSerialData(1);  // write serial in buffer
      disableFwrdMode();
      resetRxTxBuffers();
    }
  }
  // external serial reponse timeout
  if (maxFwrdRead.done()) {
    Serial.println("*EXT-232 MAX WAIT EXCEEDED!*");
    disableFwrdMode();
    resetRxTxBuffers();
  }
}

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

void writeSerialData(bool InOut) {
  if (disableResponse == true) {
    disableResponse = false;
    return;
  }
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.print(respDelimiter);
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    char _fwrdChr;
    if (InOut == 1) {
      _fwrdChr = serialMessageIn[_idx];
    } else {
      _fwrdChr = serialMessageOut[_idx];
    }
    if (_fwrdChr != nullTrm) {
      Serial.print(_fwrdChr);
    } else {
      break;
    }
  }
  Serial.print(respDelimiter);
  Serial.print('\n');
  digitalWrite(LED_BUILTIN, LOW);
}

// decode serial message
void decodeMessage() {
  // count delimiters
  uint8_t _delims = 0;
  uint8_t _maxchars = 10;
  char _delimiter = ',';
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    char _vchr = serialMessageIn[_idx];
    if (_vchr == nullTrm) {
      break;
    }
    if (_vchr == _delimiter) {
      _delims++;
    }
  }
  // exit when delimiters incorrect
  if (_delims < 2) {
    Serial.println("*PREAMP: INVALID DELIMITERS!*");
    return;
  }
  // find first delimiter position
  uint8_t _linepos = 0;
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    char _vchr = serialMessageIn[_idx];
    if (_vchr == nullTrm) {
      break;
    }
    if (_vchr == _delimiter) {
      // store index position
      _linepos = _idx;
      break;
    }
  }
  // loop through line characters
  char _linebuffer[_maxchars + 1];
  uint8_t _linecount = 0;
  for (uint8_t _idx = 0; _idx < _linepos; _idx++) {
    if (_linecount >= _maxchars) {
      break;
    }
    // store in new array
    _linebuffer[_linecount] = serialMessageIn[_idx];
    _linecount++;
  }  // terminate string
  _linebuffer[_linecount] = nullTrm;
  // convert to integer, store line value
  uint8_t controlData = atoi(_linebuffer);
  // find second delimiter position
  uint8_t _count = 0;
  uint8_t _cmd2pos = 0;
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    char _vchr = serialMessageIn[_idx];
    if (_vchr == nullTrm) {
      break;
    }
    if (_vchr == _delimiter) {
      if (_count == 1) {
        // store pointer position
        _cmd2pos = _idx;
        break;
      }
      _count++;
    }
  }
  // process command data
  if (controlData == 9) {
    processCmdData(_cmd2pos + 1);
  } else {
    Serial.println("*PREAMP: INVALID PREFIX!*");
  }
}

// process register and command data
void processCmdData(uint8_t messageStart) {
  uint8_t _numcnt = 0;
  uint8_t _cmdlen = cmdRegLen + cmdDatLen;
  char _cmdarr[_cmdlen + 1];
  // extract control code from message
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
    Serial.println("*PREAMP: INVALID LENGTH!*");
    return;
  }
  // extract register select
  char _regarr[cmdRegLen + 1];
  uint8_t _regidx = 0;
  for (uint8_t _idx = 0; _idx < cmdRegLen; _idx++) {
    _regarr[_regidx] = _cmdarr[_idx];
    _regidx++;
  }
  _regarr[_regidx] = nullTrm;
  uint8_t _register = atoi(_regarr);
  // extract control data
  char _datarr[cmdDatLen + 1];
  uint8_t _dataidx = 0;
  for (uint8_t _idx = cmdDatLen; _idx < _cmdlen; _idx++) {
    _datarr[_dataidx] = _cmdarr[_idx];
    _dataidx++;
  }
  _datarr[_dataidx] = nullTrm;
  uint16_t _ctldata = atoi(_datarr);
  // route message (from main serial)
  routeMessage(false, _register, _ctldata);
}

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

void routeMessage(bool origin, uint8_t reg, uint16_t ctldata) {
  /// route data by register ///
  if (reg == preampRegister) {
    internalFunctions(ctldata);
  } else {
    if (origin == true) {
      // combine fixed-length register and command data
      serialMessageIn[0] = nullTrm;
      uint8_t totalMsgLength = cmdRegLen + cmdDatLen + 1;
      snprintf(serialMessageIn, totalMsgLength, "%02d%03d", reg, ctldata);
    }
    /// send message to external serial port ///
    if (reg == ampRegister) {
      ExtTxOnly.print(serialDataStart);
    } else {
      ExtTxRx.print(serialDataStart);
    }
    // FALSE = message from main serial
    // TRUE = message from local device
    if (origin == true) {
      // static characters
      if (reg == ampRegister) {
        ExtTxOnly.print("9,9,");
      } else {
        ExtTxRx.print("9,9,");
      }
      // discard response for calls from local
      disableResponse = true;
    }
    // write characters
    for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
      char _vchr;
      _vchr = serialMessageIn[_idx];
      if (_vchr == nullTrm) {
        break;
      }
      if (reg == ampRegister) {
        ExtTxOnly.print(_vchr);
      } else {
        ExtTxRx.print(_vchr);
      }
    }
    if (reg == ampRegister) {
      ExtTxOnly.print(serialDataEnd);
      ExtTxOnly.print('\n');
      writeSerialMessage(0);
    } else {
      ExtTxRx.print(serialDataEnd);
      ExtTxRx.print('\n');
      // listen for external serial reponse
      serialFwrdMode = 1;
      maxFwrdRead.set(maxFwrdWait);
      maxFwrdRead.start();
    }
  }
}

// set a relay controller board (volume or inputs)
void setRelays(uint8_t pcf_a, uint8_t pcf_b,   // first pair of i2c addr's
               uint8_t vol_byte,               // the 0..255 value to write
               uint8_t installed_relay_count,  // how many bits are installed
               uint8_t forced_update_flag)     // forced or relative mode (1=forced)
{
  int bitnum;
  uint8_t mask_left;
  uint8_t mask_right;
  uint8_t just_the_current_bit;
  uint8_t just_the_previous_bit;
  uint8_t shifted_one_bit;
  // this must to be able to underflow to *negative* numbers
  // walk the bits and just count the bit-changes and save into left and right masks
  mask_left = mask_right = 0;
  //
  // this loop walks ALL bits, even the 'mute bit'
  for (bitnum = (installed_relay_count - 1); bitnum >= 0; bitnum--) {
    //
    // optimize: calc this ONLY once per loop
    shifted_one_bit = (1 << bitnum);
    //
    // this is the new volume value; and just the bit we are walking
    just_the_current_bit = (vol_byte & shifted_one_bit);
    //
    // logical AND to extra just the bit we are interested in
    just_the_previous_bit = (volLastLevel & shifted_one_bit);
    //
    // examine our current bit and see if it changed from the last run
    if (just_the_previous_bit != just_the_current_bit || forced_update_flag == 1) {
      //
      // latch the '1' on the left or right side of the relays
      if (just_the_current_bit != 0) {
        // a '1' in this bit pos
        // (1 << bitnum);
        mask_left |= ((uint8_t)shifted_one_bit);
      } else {  // (1 << bitnum);
        mask_right |= ((uint8_t)shifted_one_bit);
      }
    }  // the 2 bits were different
  }    // for each of the 8 bits
       // set upper relays
  PCFexpanderWrite(pcf_b, mask_right);
  PCFexpanderWrite(pcf_a, 0x00);
  resetPCF(pcf_a, pcf_b);
  // set lower relays
  PCFexpanderWrite(pcf_a, mask_left);
  PCFexpanderWrite(pcf_b, 0x00);
  resetPCF(pcf_a, pcf_b);
}

// reset all pins PCF8574A I/O expander
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

// read a byte from PCF8574A I/O expander
uint8_t PCFexpanderRead(int address) {
  uint8_t _data;
  Wire.requestFrom(address, 1);
  if (Wire.available()) {
    _data = Wire.read();
  }
  return _data;
}

// write a byte to PCF8574A I/O expander
void PCFexpanderWrite(uint8_t address, uint8_t _data) {
  Wire.beginTransmission(address);
  Wire.write(_data);
  Wire.endTransmission();
}

// read the pot, smooth out the data
int readPotWithSmoothing(uint8_t analog_port_num, uint8_t reread_count) {
  int sensed_port_value = 0;
  for (uint8_t i = 0; i < reread_count; i++) {
    sensed_port_value += analogRead(analog_port_num);
    // delayMicroseconds(200);
  }
  if (reread_count > 1) {
    sensed_port_value /= reread_count;
  }
  return sensed_port_value;
}

// read the pot, and clip to keep any stray values in that range
int readPotWithClipping(int sensed_pot_value) {
  int temp_volume;
  temp_volume = l_map(sensed_pot_value,
                      0,  // potMinRange,
                      potMaxRange,
                      volMin, volMax);
  return temp_volume;
}

// runs when pot state changes
void potValueChanges(void) {
  uint8_t old_vol;
  uint8_t temp_volume;
  int sensed_pot_value;
  // read pot smoothed output
  sensed_pot_value = readPotWithSmoothing(
    potAnalogPin, potRereads);  // to smooth it out
  // read an average pot value
  if (abs(sensed_pot_value - potValueLast) > potThreshold) {
    // 1-5 is a good value to ignore noise
    // convert the pot raw value into our correct volume min..max range
    old_vol = volLevel;  // the setting before user touched the pot
    // read pot clipped output
    temp_volume = readPotWithClipping(
      sensed_pot_value);
    if (temp_volume == old_vol) {
      // don't update if there was no effective change
      return;
    }
    // volume state changed
    volLevel = temp_volume;
    if (temp_volume > old_vol) {
      if (volMute == 1) {
        // tell the system we out of mute mode
        volMute = 0;
        volUpdate(volLevel, 1);
      } else {
        // not in mute mode, set volume up
        volUpdate(temp_volume, 0);
      }
    } else {
      // not in mute mode, set volume down
      volUpdate(temp_volume, 0);
    }
    // real change, save the changed state
    volPotLast = volLevel;
    potValueLast = sensed_pot_value;
  }
}

// motor pot PID control loop
void motorControlLoop(void) {
  int target_pot_wiper_value;
  int pot_pid_value;
  // given the 'IR' set volume level, find out what wiper value to compare
  target_pot_wiper_value = l_map(volLevel, volMin, volMax, potMinRange, potMaxRange);
  // this is the value of the pot, from the a/d converter
  pot_pid_value = readPotWithSmoothing(
    potAnalogPin, potRereads);
  // average out the values
  if (abs(target_pot_wiper_value - pot_pid_value) <= 8) {
    // stop the motor!
    digitalWrite(motorPinCCW, LOW);  // stop turning left
    digitalWrite(motorPinCW, LOW);   // stop turning right
    potState = motorCoasting;
    delay(5);
    return;
  } else {
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
}

// motor pot state-driven dispatcher
void motorPot(void) {
  int pot_pid_value = 0;
  static int motor_stabilized;
  if (powerState == 0)
    return;
  // If max_vol == min_vol then don't run the motor
  if (volSpan == 0)
    return;
// action states
#if 0
  if (volMute == 1)
    return; // don't spin the motor just muted
#endif
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
     * if we are 'settled' and the pot wiper changed,
     * it was via a human.  this doesn't affect our
     * motor-driven logic.
     */
      // if the volume changed via the user's IR, this should
      // trigger us to move to the next state
      if (volLevel != volPotLast) {
        potState = motorInMotion;
        potValueLast = readPotWithSmoothing(
          potAnalogPin, potRereads);
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
      delay(20);
      pot_pid_value = readPotWithSmoothing(
        potAnalogPin, potRereads);
      if (pot_pid_value == potValueLast) {
        if (++motor_stabilized >= 5) {
          // yay! we reached our target
          potState = motorSettled;
        }
      } else {
        // we found a value that didn't match,
        // so reset our 'sameness' counter
        motor_stabilized = 0;
      }
      //---------------//
      // this is the operating value of the pot,
      // from the a/d converter
      potValueLast = pot_pid_value;
      break;
      //
    default:
      break;
  }
}

// increment volume up/down slow/fast
void volIncrement(uint8_t dir_flag, uint8_t speed_flag) {
  if (dir_flag == volControlUp) {
    if (volLevel >= volMax)
      return;
  } else if (dir_flag == volControlDown) {
    if (volLevel <= volMin)
      return;
  }
  if (volMute == 1) {
    volMute = 0;
    volUpdate(volLevel, 1);
    return;
  }
  if (dir_flag == volControlUp) {
    if (speed_flag == volControlSlow) {
      // volume up-slow
      if (volLevel < volMax && (volLevel + 1) < volMax) {
        volLevel += 1;
      } else {
        volLevel = volMax;
      }
      volUpdate(volLevel, 0);
    } else if (speed_flag == volControlFast) {
      // volume up-fast
      if (volLevel < (volMax - volCoarseSteps)) {
        volLevel += volCoarseSteps;
      } else {
        volLevel = volMax;
      }
      volUpdate(volLevel, 0);
    }
  } else if (dir_flag == volControlDown) {
    if (speed_flag == volControlSlow) {
      // volume down-slow
      if (volLevel > volMin && (volLevel - 1) > volMin) {
        volLevel -= 1;
      } else {
        volLevel = volMin;
      }
      volUpdate(volLevel, 0);
    } else if (speed_flag == volControlFast) {
      // volume down-fast
      if (volLevel > (volMin + volCoarseSteps)) {
        volLevel -= volCoarseSteps;
      } else {
        volLevel = volMin;
      }
      volUpdate(volLevel, 0);
    }
  }
}

// set a specific volume level
void volUpdate(uint8_t _vol, uint8_t _force) {
  // set volume relays
  if (volMute == 0) {
    setRelays(volSetAddr, volResetAddr, _vol, volRelayCount, _force);
    volLastLevel = volLevel;
    // update display
    lcdVolume(_vol);      // redraw progress bar and percentage
    lcd.setCursor(0, 1);  // music icon
    lcd.print(char(0));
    lcd.setCursor(1, 1);
    lcd.print(" ");
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
    // set volume to min
    volUpdate(volMin, 1);
    // mute status on display
    lcd.setCursor(2, 1);
    lcd.print("          ");
    lcd.setCursor(0, 0);
    lcd.print("----------------");
    lcd.setCursor(12, 1);
    lcd.print("MUTE");
    lcd.setCursor(0, 1);
    lcd.print(char(6));
    // set mute flag
    volMute = 1;
  } else {
    volMute = 0;
    lcd.setCursor(0, 0);  // clear top of screen
    lcd.print("                ");
    volUpdate(volLevel, 1);
  }
}

// set a specific audio input
void inputUpdate(uint8_t _input) {
  if (_input > 6) {
    return;
  }
  // set last input selected
  if (_input == 0) {
    _input = inputSelected;
  } else {
    // save input state
    inputSelected = _input;
  }
  // set decoder input
  if (_input == 1) {
    writeRegister = dacRegister;
    writeCommand = 1; // USB
  }
  if (_input == 4) {
    writeRegister = dacRegister;
    writeCommand = 3; // Coax
  }
  if (_input == 5) {
    writeRegister = dacRegister;
    writeCommand = 2; // Opt
  }
  if (_input == 6) {
    writeRegister = dacRegister;
    writeCommand = 4; // Auto
  }  
  // select input relay
  if (_input == 1 || _input > 3) {  // input #1
    setInputRelays(B00000001);
  }
  if (_input == 2) {  // input #2
    setInputRelays(B00000010);
  }
  if (_input == 3) {  // input #3
    setInputRelays(B00000100);
  }
  updateInputDisplay(_input);
}

void setInputRelays(uint8_t state) {
   setRelays(inputSetAddr, inputResetAddr, state, inputRelayCount, 1);
}

void updateInputDisplay(uint8_t input) {
  // update display
  lcd.setCursor(2, 1);
  lcd.print(inputNames[input - 1]);
}

// high-pass filter control
void hpfControl(uint8_t _state) {
  if (volMute == 0) {
    if (_state == 0) {  // HPF on
      hpfState = 0;
    }
    if (_state == 1) {  // HPF off
      hpfState = 1;
    }
    if (_state == 2) {  // toggle state
      hpfState = !hpfState;
    }
    if (_state >= 3) {  // invalid
      return;
    }
    if (hpfState == 0) {
      showMessage("HPF Off   ");
      digitalWrite(hpfRelayPin, HIGH);
    } else {
      showMessage("HPF On    ");
      digitalWrite(hpfRelayPin, LOW);
    }
  }
}

void showMessage(char message[]) {
  lcd.setCursor(2, 1);
  lcd.print(message);
  delayDrawInputs.start();
}

void showHeartMessage() {
  if (powerState == 0) {
    analogWrite(lcdBacklightPin, lcdOnBrightness);
    lcd.setCursor(0, 2);
    lcd.print("I         ");
    lcd.setCursor(2, 2);
    lcd.print(char(7));
    lcd.setCursor(4, 2);
    lcd.print("Nicky");
  }
}

// volume status on display
void lcdVolume(int _level) {
  // update percentage
  int _lcdlevel = map(_level, volMin, volMax, 0, 100);
  if (_lcdlevel <= 0) {
    return;
  } else {
    if (_lcdlevel >= 100) {
      lcd.setCursor(12, 1);
      lcd.print("    ");
      lcd.setCursor(13, 1);
      lcd.print("MAX");
    } else {
      lcd.setCursor(12, 1);
      lcd.print("    ");
      lcd.setCursor(13, 1);
      lcd.print(_lcdlevel);
      lcd.setCursor(15, 1);
      lcd.print("%");
    }
    // draw volume progress bar if unmuted
    _lcdlevel = map(_level, volMin, volMax, 0, 1024);
    lcdBar(0, _lcdlevel, 0, 1024);
  }
}

void lcdBar(int row, int var, int minVal, int maxVal) {
  int constrainedVar = constrain(var, minVal, maxVal);
  // Scale to 16 blocks and 80 "sub-steps" (16 blocks * 5 segments)
  int block = map(constrainedVar, minVal, maxVal, 0, 16); 
  int line = map(constrainedVar, minVal, maxVal, 0, 80); 
  int bar = (line - (block * 5)); 
  // 1. Draw solid blocks
  for (int x = 0; x < block; x++) {
    lcd.setCursor(x, row);
    lcd.write(5); // bar5 is the 5-line solid block
  }
  // 2. Draw the single partial "tip" of the bar
  if (block < 16) {
    lcd.setCursor(block, row);
    if (bar > 0) {
      lcd.write(bar); // Writes bar1, bar2, bar3, or bar4
    } else {
      lcd.print(" "); // Clear the "tip" space if it's empty
    }
  }
  // 3. Wipe the rest of the row (Fixing the 16 index bug)
  for (int x = block + 1; x < 16; x++) {
    lcd.setCursor(x, row);
    lcd.print(" "); // Using a space is safer than custom char 1022
  }
}

// display progress bar for # of seconds
void lcdTimedBar(int _sec, uint8_t _single) {
  uint32_t _ms = _sec * 6;
  uint32_t _colcount;
  uint32_t _segcount;
  uint8_t _rowcount;
  uint8_t _line = 1;
  if (_single == 0) {
    return;
  }
  // draw bar on each row
  for (_rowcount = 0; _rowcount < _single; _rowcount++) {
    // draw bar on each collumn
    for (_colcount = 0; _colcount < lcdCols; _colcount++) {
      // draw bar segments
      for (_segcount = 0; _segcount < 5; _segcount++) {
        lcd.setCursor(_colcount, _line);
        // draw custom segment
        lcd.write(_segcount + 1);
        // also reset PCF expanders
        resetPCF(volSetAddr, volResetAddr);
        resetPCF(inputSetAddr, inputResetAddr);
        // pause
        delay(_ms);
      }
    }  // decrement to next row
    _line--;
  }
}

// power on/off
void setPowerState() {
  // read pin state from MCP23008
  int reading = lcd.readPin(powerButtonPin);
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
  // power state actions
  if (powerCycle == 1) {
    // reset display
    lcd.clear();
    // one-shot triggers
    if (powerState == 1) {
      /// runs once on boot ///
      startup();
    } else {
      /// runs once on shutdown ///
      shutdown();
    }
    powerCycle = 0;
  }
}

// shutdown routines
void shutdown() {
  // mute when not muted
  if (volMute == 0) {
    muteSystem();
  }
  // shutdown external amplifier
  routeMessage(true, ampRegister, 1);
  // shutdown animation
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Shutting Down...");
  lcdTimedBar(shutdownTime, 1);
  lcdStandby();
  // turn off analog stages
  digitalWrite(powerRelayPin, LOW);
}

// startup routines
void startup() {  // set display brightness
  analogWrite(lcdBacklightPin, lcdOnBrightness);
  // turn on analog stages
  digitalWrite(powerRelayPin, HIGH);
  // mute when not muted
  if (volMute == 0) {
    muteSystem();
  }
  // loading bar
  lcd.clear();
  lcd.setCursor(0, 0);
  if (volLimitState == 1) {
    lcd.print("Volume Limit On");
  } else {
    lcd.print("Volume Limit Off");
  }
  delay(650);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Starting Up...");
  lcdTimedBar(startDelay, 1);
  lcd.clear();
  // calculate volume min/max
  uint8_t max_byte_size = (1 << volRelayCount) - 1;
  volMin = (max_byte_size * volMinLimit) / 100;
  // volume limiter
  if (volLimitState == 1) {
    volMax = (max_byte_size * volMaxLimit) / 100;
  } else {
    volMax = max_byte_size;
  }
  // calculate volume span
  volSpan = abs(volMax - volMin);
  // read volume level from pot
  potState = motorInit;
  // initialize and unmute
  muteSystem();
  // allow relays to settle
  delay(350);
  // set last selected input
  inputUpdate(0);
}

// display in standby mode
void lcdStandby() {
  analogWrite(lcdBacklightPin, lcdOffBrightness);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ProDesigns");
  lcd.setCursor(0, 1);
  lcd.print("Preamp III");
  lcd.setCursor(15, 1);
  lcd.print(char(6));
  delay(500);
  // turn off preamp power
  digitalWrite(LED_BUILTIN, LOW);
}

void initSerial() {
  Serial.begin(serialBaudRate);
  pinMode(AMP_RX_PIN, INPUT);
  pinMode(AMP_TX_PIN, OUTPUT);
  ExtTxOnly.begin(serialBaudRate);  
  pinMode(DAC_RX_PIN, INPUT);
  pinMode(DAC_TX_PIN, OUTPUT);
  ExtTxRx.begin(serialBaudRate);
  ExtTxRx.listen();
  serialMessageIn[0] = nullTrm;
  serialMessageOut[0] = nullTrm;
}

void initTimers() {
  maxFwrdRead.reset();
  maxFwrdRead.stop();
  delayDrawInputs.reset();
  delayDrawInputs.stop();
}

///////////////////////////////////////////////////////////////////////////////
// initialization
void setup() {
  // timers
  initTimers();
  // 16x2 display (calls Wire.begin)
  lcd.begin(lcdCols, lcdRows);
  // reset expanders
  resetPCF(volSetAddr, volResetAddr);
  resetPCF(inputSetAddr, inputResetAddr);
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
  // icons
  lcd.createChar(0, sound);
  lcd.createChar(6, speaker);
  lcd.createChar(7, heart);
  // initial boot display
  lcd.createChar(1, bar1);
  lcd.createChar(2, bar2);
  lcd.createChar(3, bar3);
  lcd.createChar(4, bar4);
  lcd.createChar(5, bar5);
  lcdTimedBar(initStartDelay, 2);
  lcd.clear();
  delay(100);
  // serial support
  initSerial();
  // startup complete
  lcdStandby();
}

///////////////////////////////////////////////////////////////////////////////
// superloop
void loop() {
  // power management
  setPowerState();
  if (powerState == 1) {
    // motor potentiometer
    if (potState == motorSettled || potState == motorInit) {
      potValueChanges();
    }
    motorPot();
  }
  // communication
  serialProcess();
  routeQueued();
  // update inputs on display
  if (delayDrawInputs.done()) {
    updateInputDisplay(inputSelected);
    delayDrawInputs.reset();
    delayDrawInputs.stop();
  }
}

