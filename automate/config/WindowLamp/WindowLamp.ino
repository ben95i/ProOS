
// shared libraries
#include <WiFi.h>
#include <neotimer.h>
#include <RotaryEncoder.h>

// local libraries
#include "creds.h"

// GPIO pins
#define powerBtnPin    3
#define lamp1PowerPin  21
#define lamp2PowerPin  4
#define inputEncPinA   14
#define inputEncPinB   15

// common resources
#define serialBaudRate 115200
const char nullTrm = '\0';
const char newLine = '\n';
const char carriageReturn = '\r';
WiFiServer WebServer(4000); // web server port
Neotimer WiFiCheck = Neotimer(30000); // in (ms)

// command decoder
const uint8_t maxMessage = 32;
char serialMessageIn[maxMessage + 1];
char serialMessageOut[maxMessage + 1];
const uint8_t cmdMaxChars = 3;
const char serialDataStart = '?';
const char serialDataEnd = '~';
uint16_t serialCurPos = 0;
bool serialReading = 0;

// HTTP buffer
const uint8_t HTTPBufferSize = 255;
const uint16_t HTTPClientTimeout = 1000; // client inactivity timeout in (ms)
char HTTPBuffer[HTTPBufferSize];
uint8_t HTTPReqIdx = 0;

// power button
const uint8_t btnDebounceDelay = 75; // button debounce in (ms)
uint8_t lastPowerButton = 1;
uint32_t powerButtonMillis;
uint8_t powerButton = 0;

// encoder resources
// LatchMode::FOUR3 reports one position count per physical detent
RotaryEncoder rotaryEncoder(inputEncPinB, inputEncPinA, RotaryEncoder::LatchMode::FOUR3);
const uint8_t encoderSensitivity = 8; // brightness steps applied per detent
volatile long oldEncPos = 0;

// LED lights
#define pwmFrequency 22000 // PWM frequency in (Hz), keep >20x above the 1.28kHz LC filter resonance
const uint8_t maxPWMrange = 255;
bool lamp1Status = 1; // 1=OFF, 0=ON
int16_t lamp2Status = 0; // 0=OFF, 255=100%
const uint8_t antiLogarithmicTable[256] = {
    0,    0,    0,    0,    0,    0,    0,    1,    1,    1,    1,    1,    1,    1,    1,    1,
    1,    1,    1,    2,    2,    2,    2,    2,    2,    2,    2,    3,    3,    3,    3,    3,
    3,    3,    4,    4,    4,    4,    4,    5,    5,    5,    5,    5,    6,    6,    6,    6,
    6,    7,    7,    7,    7,    8,    8,    8,    8,    9,    9,    9,   10,   10,   10,   11,
   11,   11,   12,   12,   12,   13,   13,   13,   14,   14,   14,   15,   15,   16,   16,   16,
   17,   17,   18,   18,   19,   19,   20,   20,   21,   21,   22,   22,   23,   23,   24,   24,
   25,   25,   26,   26,   27,   28,   28,   29,   29,   30,   31,   31,   32,   33,   33,   34,
   35,   35,   36,   37,   37,   38,   39,   40,   40,   41,   42,   43,   44,   44,   45,   46,
   47,   48,   49,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,   61,
   62,   63,   64,   65,   66,   67,   68,   69,   70,   71,   72,   73,   75,   76,   77,   78,
   79,   80,   82,   83,   84,   85,   87,   88,   89,   90,   92,   93,   94,   96,   97,   99,
  100,  101,  103,  104,  106,  107,  108,  110,  111,  113,  114,  116,  118,  119,  121,  122,
  124,  125,  127,  129,  130,  132,  134,  135,  137,  139,  141,  142,  144,  146,  148,  149,
  151,  153,  155,  157,  159,  161,  162,  164,  166,  168,  170,  172,  174,  176,  178,  180,
  182,  185,  187,  189,  191,  193,  195,  197,  200,  202,  204,  206,  208,  211,  213,  215,
  218,  220,  222,  225,  227,  230,  232,  234,  237,  239,  242,  244,  247,  249,  252,  255,
};

// main entry point
void setup() {
  serialMessageOut[0] = nullTrm;
  // initalize PWM
  pinMode(lamp2PowerPin, OUTPUT);
  analogWriteFreq(pwmFrequency);
  analogWriteRange(maxPWMrange);
  digitalWrite(lamp2PowerPin, LOW);
  // initalize GPIO
  pinMode(powerBtnPin, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(lamp1PowerPin, OUTPUT);
  digitalWrite(lamp1PowerPin, lamp1Status);
  // initalize encoder interrupts (all RP2040 GPIOs are interrupt capable)
  attachInterrupt(digitalPinToInterrupt(inputEncPinA), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(inputEncPinB), encoderISR, CHANGE);
  // initalize serial
  Serial.begin(serialBaudRate);
  // start networking
  connectWiFi();
  delay(1000);
  WebServer.begin();
  digitalWrite(LED_BUILTIN, LOW);
}

// superloop
void loop() {
  checkWiFi();
  readPowerButton();
  readEncoder();
  webServer();
}

// interrupt service routine: update encoder state machine on any pin edge
void encoderISR() {
  rotaryEncoder.tick();
}

// set Wi-Fi client mode and connect to the configured network
void connectWiFi() {
  Serial.println("connecting to Wi-Fi...");
  // set client mode
  WiFi.mode(WIFI_STA);
  // disable power management
  cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM & ~0xf);
  // connect to network
  WiFi.begin(ssid, ssid_pwd);
}

// periodically verify the Wi-Fi link and reconnect when dropped
void checkWiFi() {
  if (WiFiCheck.repeat()) {
    if (WiFi.status() != WL_CONNECTED) {
      // disconnect from the current network
      WiFi.disconnect();
      delay(500);
      connectWiFi();
    } else {
      Serial.println("connected to Wi-Fi");
    }
  }
}

// read power button state
void readPowerButton() {
  // read pin state
  uint8_t reading = digitalRead(powerBtnPin);
  // switch changed
  if (reading != lastPowerButton) {
    // reset the debouncing timer
    powerButtonMillis = millis();
  }
  if ((millis() - powerButtonMillis) > btnDebounceDelay) {
    if (reading != powerButton) {
      powerButton = reading;
      // power button was pressed
      if (powerButton == 0) {
        // toggle lamp #1 power
        lamp1Status = !lamp1Status;
        digitalWrite(lamp1PowerPin, lamp1Status);
      }
    }
  }
  lastPowerButton = reading;
}

// apply accumulated encoder movement to lamp #2 brightness
void readEncoder() {
  long encoderPosition = rotaryEncoder.getPosition();
  long positionDelta = encoderPosition - oldEncPos;
  if (positionDelta != 0) {
    // apply every detent turned since the last pass
    // (positive delta = encoder left = decrease, matching original direction)
    setLamp2(lamp2Status - (int16_t)(positionDelta * encoderSensitivity));
    oldEncPos = encoderPosition;
  }
}

// dispatch decoded remote commands to lamp functions
void remoteFunctions(uint16_t _ctldata, uint16_t _state) {
  switch (_ctldata) {
  case 1:
    // lamp #1 fixed-controls
    switch (_state) {
    case 0: // lamp status
      writeSerialMessage(!lamp1Status);
      break;
    case 1: // toggle lamp
      toggleLamp1();
      writeSerialMessage(!lamp1Status);
      break;
    case 2: // lamp power off
      if (lamp1Status == 0) { toggleLamp1(); }
      writeSerialMessage(!lamp1Status);
      break;
    case 3: // lamp power on
      if (lamp1Status == 1) { toggleLamp1(); }
      writeSerialMessage(!lamp1Status);
      break;
    default:
      writeSerialMessage(0);
    }
    break;
  case 2:
    // lamp #2 fixed-controls
    switch (_state) {
    case 0: // lamp power off
      for (int16_t fadeoff = lamp2Status; fadeoff >= 0; fadeoff--) {
        setLamp2(fadeoff);
        delay(8);
      }
      writeSerialMessage(lamp2Status);
      break;
    case 1: // lamp on
      for (int16_t fadeon = lamp2Status; fadeon <= maxPWMrange; fadeon++) {
        setLamp2(fadeon);
        delay(8);
      }
      writeSerialMessage(lamp2Status);
      break;
    case 2: // lamp #2 status
      writeSerialMessage(lamp2Status);
      break;
    default:
      writeSerialMessage(0);
    }
    break;
  case 3:
    // lamp #2 direct control
    setLamp2(_state);
    writeSerialMessage(lamp2Status);
    break;
  case 4:
    // write all lamps status
    writeAllLampsStatus();
    break;
  default:
    writeSerialMessage(0);
  }
}

// format a single integer value into the output message buffer
void writeSerialMessage(int16_t value) {
  snprintf(serialMessageOut, sizeof(serialMessageOut), "%d", value);
}

// format both lamp states into the output message buffer
void writeAllLampsStatus() {
  snprintf(serialMessageOut, sizeof(serialMessageOut), "%d~%d",
           (int16_t)!lamp1Status, lamp2Status);
}

// set lamp #2 brightness with range clamping and anti-log correction
void setLamp2(int16_t brightness) {
  // limit max range
  if (brightness >= maxPWMrange) { brightness = maxPWMrange; }
  if (brightness <= 0) { brightness = 0; }
  // convert range to anti-logarithmic using LUT
  uint8_t pwm_dt = antiLogarithmicTable[brightness];
  switch (pwm_dt) {
    case maxPWMrange: // full-on
      digitalWrite(lamp2PowerPin, HIGH);
      break;
    case 0: // off
      digitalWrite(lamp2PowerPin, LOW);
      break;
    default:
      // set PWM duty cycle
      analogWrite(lamp2PowerPin, pwm_dt);
      break;
  }
  // store brightness
  lamp2Status = brightness;
}

// toggle lamp #1 output state
void toggleLamp1() {
  lamp1Status = !lamp1Status;
  digitalWrite(lamp1PowerPin, lamp1Status);
}

// decode serial message
void decodeMessage() {
  const char _delimiter = ',';
  uint8_t _delims = 0;
  uint8_t _charscount = 0;
   // count delimiters
  for (uint8_t _idx = 0; _idx < maxMessage; _idx++) {
    char _vchr = serialMessageIn[_idx];
    if (_vchr == nullTrm) {
      break;
    }
    if (_vchr == _delimiter) {
      _delims++;
    }
    _charscount++;
  }
  // exit when delimiters incorrect
  if (_delims < 2) {
    Serial.println("*INVALID DELIMITERS!*");
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
  // loop through first argument characters
  char _linebuffer[cmdMaxChars + 1];
  uint8_t _linecount = 0;
  for (uint8_t _idx = 0; _idx < _linepos; _idx++) {
    if (_linecount >= cmdMaxChars) {
      break;
    }
    // store in new array
    _linebuffer[_linecount] = serialMessageIn[_idx];
    _linecount++;
  } // terminate string
  _linebuffer[_linecount] = nullTrm;
  // convert to integer, store line value
  uint16_t controlData = atoi(_linebuffer);
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
  // loop through second argument characters
  char _line2buffer[cmdMaxChars + 1];
  uint8_t _line2count = 0;
  uint8_t _cmd2idx = _cmd2pos + 1;
  for (uint8_t _idx = 0; _idx < cmdMaxChars; _idx++) {
    // stop reading at end of line character
    char _line2char = serialMessageIn[_cmd2idx];
    if (_line2char == _delimiter || _line2char == nullTrm) {
      break;
    }
    // store in new array
    _line2buffer[_line2count] = _line2char;
    _line2count++;
    _cmd2idx++;
  } // terminate string
  _line2buffer[_line2count] = nullTrm;
  // convert to integer, store line value
  uint16_t stateData = atoi(_line2buffer);
  // run function
  remoteFunctions(controlData, stateData);
}

// accumulate framed command characters and trigger decoding at frame end
void processSerialData(char rc, char startInd ,char endInd) {
  if (serialReading == 1) {
    // end-of-reading
    if (rc == endInd) {
      // terminate the string
      serialMessageIn[serialCurPos] = nullTrm;
      serialReading = 0;
      serialCurPos = 0;
      decodeMessage();
    } else {
      // store characters in buffer
      if (rc != startInd) {
        if (serialCurPos < (maxMessage - 1)) {
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
    }
  }
}

// accept HTTP clients, extract framed commands, and reply with the result
void webServer() {
  WiFiClient client = WebServer.accept();
  if (!client) return;
  // incoming client connection
  client.setNoDelay(true); // disable nagle's algorithm
  HTTPReqIdx = 0; // reset buffer index for new request
  bool lineblank = 1;
  uint32_t clientActivityMillis = millis(); // reset the client timeout
  while (client.connected()) {
    // drop stalled or malformed clients so the superloop never hangs
    if ((millis() - clientActivityMillis) > HTTPClientTimeout) {
      break;
    }
    if (client.available()) {
      char http_byte = client.read(); // read byte (character)
      clientActivityMillis = millis(); // reset the client timeout
      // store character in the buffer if space is available
      if (HTTPReqIdx < (HTTPBufferSize - 1)) {
        HTTPBuffer[HTTPReqIdx] = http_byte;
        HTTPReqIdx++;
      }
      // the end of the client request is typically a blank line
      // (CR/LF, which is \r\n, followed by another \r\n)
      if (http_byte == newLine && lineblank) {
        // end of HTTP request, process the data
        HTTPBuffer[HTTPReqIdx] = nullTrm; // terminate the string
        // process the incoming request array
        for (size_t _idx = 0; _idx < (HTTPBufferSize - 1); _idx++) {
          char _chr = HTTPBuffer[_idx];
          if (_chr == nullTrm) { break; }
          processSerialData(_chr, serialDataStart, serialDataEnd);
        }
        // send response to client
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: text/plain");
        client.println("Connection: close");
        client.println();
        client.print(serialMessageOut);
        client.print(newLine);
        break;
      }
      // track blank lines for end-of-request detection
      if (http_byte == newLine) {
        lineblank = 1;
      } else if (http_byte != carriageReturn) {
        lineblank = 0;
      }
    }
  }
  client.stop();
}
