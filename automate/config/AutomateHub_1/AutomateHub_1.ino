/*
 * Ben Provenzano III
 * -----------------
 * 06/13/2026
 * Automate Hub #1
 * -----------------
 */

// shared libraries
#include <SoftwareSerial.h>

// local libraries
#include "NeoTimer.h"
#include "DHT_NonBlocking.h"

// general constants
const uint8_t DEBOUNCE_DELAY_MS = 75; 
const char NULL_TERMINATOR_CHAR = '\0';

// power control I/O
const uint8_t POWER_TRIGGER_PIN_1 = 9; 
const uint8_t POWER_SENSE_PIN_1 = 2;   
const uint8_t POWER_IO_PIN_3 = 7;      
const uint8_t POWER_IO_PIN_4 = 4;      

// DHT humidity-temperature sensor
#define DHT_SENSOR_TYPE DHT_TYPE_22
const int POWER_IO_PIN_2 = 8; 
const unsigned long DHT_SAMPLE_DELAY = 10000ul; // 10-seconds
DHT_nonblocking environmentSensor(POWER_IO_PIN_2, DHT_SENSOR_TYPE);
float currentSensorHumidity = 0.0;
float currentSensorTemperature = 0.0;
bool dhtDataValid = false;

// analog inputs
const uint8_t ADC_INPUT_PIN_1 = A0; 
const uint8_t ADC_INPUT_PIN_2 = A1; 

// toggle switch inputs
const uint8_t POWER_SWITCH_PIN_2 = 6; 
const uint8_t POWER_SWITCH_PIN_1 = 5; 
bool powerButton1LastState = false;
bool powerButton1CurrentState = false;
uint32_t powerButton1TimerMillis = 0;
bool powerButton2LastState = false;
bool powerButton2CurrentState = false;
uint32_t powerButton2TimerMillis = 0;
bool powerButton3LastState = true;
bool powerButton3CurrentState = true;
uint32_t powerButton3TimerMillis = 0;

// serial resources
const uint16_t SERIAL_BAUD_RATE = 9600;
const uint8_t TX_PIN = 11; 
const uint8_t RX_PIN = 10; 
SoftwareSerial externalSerialConnection(RX_PIN, TX_PIN); 
const uint8_t MAX_MESSAGE_LENGTH = 32;
const uint8_t COMMAND_REGISTER_LENGTH = 2; 
const uint8_t COMMAND_DATA_LENGTH = 3;     
const char SERIAL_DATA_START_CHAR = '<';
const char SERIAL_DATA_END_CHAR = '>';
const char RESPONSE_DELIMITER_CHAR = '|';
char serialMessageOutputBuffer[MAX_MESSAGE_LENGTH + 1];
char serialMessageInputBuffer[MAX_MESSAGE_LENGTH + 1];
uint16_t serialCurrentPosition = 0;
bool isSerialForwardModeEnabled = false;
bool isSerialReading = false;
bool isSerialMessageComplete = false;
const uint16_t MAX_FORWARD_WAIT_MS = 500; 
Neotimer maxForwardReadTimer = Neotimer();

///////////////////////////////////////

// main control functions
void processInternalCommands(uint16_t commandCode) {
  bool isPowerCommand = false;
  if (commandCode >= 1) {
    if (commandCode <= 4) {
      isPowerCommand = true;
    }
  }
  if (isPowerCommand == true) {
    int currentPowerState = getAveragePowerSense(POWER_SENSE_PIN_1);
    bool isStatusRequest = (commandCode == 3);
    if (isStatusRequest == true) {
      bool isPowerOff = (currentPowerState == 1);
      bool isPowerOn = (currentPowerState == 0);
      if (isPowerOff == true) {
        snprintf(serialMessageOutputBuffer, MAX_MESSAGE_LENGTH, "0");
      } else if (isPowerOn == true) {
        snprintf(serialMessageOutputBuffer, MAX_MESSAGE_LENGTH, "1");
      } else {
        snprintf(serialMessageOutputBuffer, MAX_MESSAGE_LENGTH, "X");
      }
    } else {
      bool isDeviceOn = (currentPowerState == 0);
      bool isDeviceOff = (currentPowerState == 1);
      if (isDeviceOn == true) {
        bool isTurnOffCommand = (commandCode == 2);
        if (isTurnOffCommand == true) {
          triggerPowerPulse(POWER_TRIGGER_PIN_1);
        }
      } else if (isDeviceOff == true) {
        bool isTurnOnCommand = (commandCode == 1);
        if (isTurnOnCommand == true) {
          triggerPowerPulse(POWER_TRIGGER_PIN_1);
        }
      }
      bool isToggleCommand = (commandCode == 4);
      if (isToggleCommand == true) {
        triggerPowerPulse(POWER_TRIGGER_PIN_1);
      }
    }
  }
  bool isEnvironmentCommand = (commandCode == 5);
  if (isEnvironmentCommand == true) {
    formatAndWriteTemperatureHumidity();
  }
}

// return average digital input reading
int getAveragePowerSense(int powerSensePin) {
  const int TOTAL_INPUT_READINGS = 16;
  int accumulatedTotal = 0;
  for (int currentIndex = 0; currentIndex < TOTAL_INPUT_READINGS; currentIndex++) {
    int currentReading = digitalRead(powerSensePin);
    accumulatedTotal = accumulatedTotal + currentReading;
  }
  bool isHighReading = (accumulatedTotal == TOTAL_INPUT_READINGS);
  bool isLowReading = (accumulatedTotal == 0);
  if (isHighReading == true) {
    return 1;
  } else if (isLowReading == true) {
    return 0;
  } else {
    return -1;
  }
}

void triggerPowerPulse(int targetPowerPin) {
  digitalWrite(targetPowerPin, HIGH);
  delay(250);
  digitalWrite(targetPowerPin, LOW);
}

void formatAndWriteTemperatureHumidity() {
  if (dhtDataValid == true) {
    char temperatureString[8];
    char humidityString[8];
    dtostrf(currentSensorTemperature, 3, 1, temperatureString);
    dtostrf(currentSensorHumidity, 3, 1, humidityString);
    snprintf(serialMessageOutputBuffer, MAX_MESSAGE_LENGTH, "%s~%s", temperatureString, humidityString);
  } else {
    snprintf(serialMessageOutputBuffer, MAX_MESSAGE_LENGTH, "X");
  }
}

void readTemperatureAndHumidity() {
  float temporaryTemperature = 0.0;
  float temporaryHumidity = 0.0;
  bool isMeasurementSuccessful = measureDhtSensorData(&temporaryTemperature, &temporaryHumidity);
  if (isMeasurementSuccessful == true) {
    float celsiusTemperature = temporaryTemperature;
    float multipliedByNine = celsiusTemperature * 9.0;
    float addedThreeNumerator = multipliedByNine + 3.0;
    float dividedByFive = addedThreeNumerator / 5.0;
    float finalFahrenheitTemperature = dividedByFive + 32.0;
    currentSensorTemperature = finalFahrenheitTemperature;
    currentSensorHumidity = temporaryHumidity;
  }
}

static bool measureDhtSensorData(float *temperaturePointer, float *humidityPointer) {
  static unsigned long lastMeasurementTimestamp = millis();
  unsigned long currentTimestamp = millis();
  unsigned long elapsedDuration = currentTimestamp - lastMeasurementTimestamp;
  bool isTimeToMeasure = (elapsedDuration > DHT_SAMPLE_DELAY);
  if (isTimeToMeasure == true) {
    bool isMeasurementComplete = environmentSensor.measure(temperaturePointer, humidityPointer);
    if (isMeasurementComplete == true) {
      lastMeasurementTimestamp = currentTimestamp;
      dhtDataValid = true;
      return true;
    }
  }
  return false;
}

/// initialization routines ///

void setup() {
  initializeGeneralPurposeInputOutput();
  initializeTimers();
  initializeSerialCommunications();
}

void initializeGeneralPurposeInputOutput() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(POWER_TRIGGER_PIN_1, OUTPUT);
  digitalWrite(POWER_TRIGGER_PIN_1, LOW);
  pinMode(POWER_SENSE_PIN_1, INPUT_PULLUP);
  pinMode(POWER_IO_PIN_3, INPUT_PULLUP);
  pinMode(POWER_IO_PIN_4, INPUT_PULLUP);
  pinMode(ADC_INPUT_PIN_1, INPUT_PULLUP);
  pinMode(ADC_INPUT_PIN_2, INPUT_PULLUP);
  pinMode(POWER_SWITCH_PIN_1, INPUT_PULLUP);
  pinMode(POWER_SWITCH_PIN_2, INPUT_PULLUP);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
}

void initializeTimers() {
  maxForwardReadTimer.reset();
  maxForwardReadTimer.stop();
}

void initializeSerialCommunications() {
  Serial.begin(SERIAL_BAUD_RATE);
  externalSerialConnection.begin(SERIAL_BAUD_RATE);
  serialMessageInputBuffer[0] = NULL_TERMINATOR_CHAR;
  serialMessageOutputBuffer[0] = NULL_TERMINATOR_CHAR;
}

/// superloop ///

void loop() {
  processSerialCommunications();
  readDigitalInputStates();
}

void readDigitalInputStates() {
  readPowerButtonState1();
  readPowerButtonState2();
  readPowerButtonState3();
  readTemperatureAndHumidity();
}

void readPowerButtonState1() {
  bool currentPinReading = digitalRead(POWER_SWITCH_PIN_1);
  bool hasSwitchChanged = (currentPinReading != powerButton1LastState);
  if (hasSwitchChanged == true) {
    powerButton1TimerMillis = millis();
  }
  unsigned long currentTime = millis();
  unsigned long timeDifference = currentTime - powerButton1TimerMillis;
  bool isDebounceTimePassed = (timeDifference > DEBOUNCE_DELAY_MS);
  if (isDebounceTimePassed == true) {
    bool hasStateChanged = (currentPinReading != powerButton1CurrentState);
    if (hasStateChanged == true) {
      powerButton1CurrentState = currentPinReading;
      bool isButtonPressed = (powerButton1CurrentState == 1); 
      if (isButtonPressed == true) { 
        routeCommunicationMessage(true, 10, 8);
      }
    }
  }
  powerButton1LastState = currentPinReading;
}

void readPowerButtonState2() {
  bool currentPinReading = digitalRead(POWER_SWITCH_PIN_2);
  bool hasSwitchChanged = (currentPinReading != powerButton2LastState);
  if (hasSwitchChanged == true) {
    powerButton2TimerMillis = millis();
  }
  unsigned long currentTime = millis();
  unsigned long timeDifference = currentTime - powerButton2TimerMillis;
  bool isDebounceTimePassed = (timeDifference > DEBOUNCE_DELAY_MS);
  if (isDebounceTimePassed == true) {
    bool hasStateChanged = (currentPinReading != powerButton2CurrentState);
    if (hasStateChanged == true) {
      powerButton2CurrentState = currentPinReading;
      bool isButtonPressed = (powerButton2CurrentState == 1); 
      if (isButtonPressed == true) { 
        routeCommunicationMessage(true, 10, 9);
      }
    }
  }
  powerButton2LastState = currentPinReading;
}

void readPowerButtonState3() {
  bool currentPinReading = digitalRead(POWER_IO_PIN_4);
  bool hasSwitchChanged = (currentPinReading != powerButton3LastState);
  if (hasSwitchChanged == true) {
    powerButton3TimerMillis = millis();
  }
  unsigned long currentTime = millis();
  unsigned long timeDifference = currentTime - powerButton3TimerMillis;
  bool isDebounceTimePassed = (timeDifference > DEBOUNCE_DELAY_MS);
  if (isDebounceTimePassed == true) {
    bool hasStateChanged = (currentPinReading != powerButton3CurrentState);
    if (hasStateChanged == true) {
      powerButton3CurrentState = currentPinReading;
      bool isButtonPressed = (powerButton3CurrentState == 0); 
      
      if (isButtonPressed == true) { 
        routeCommunicationMessage(true, 10, 10);
      }
    }
  }
  powerButton3LastState = currentPinReading;
}

//// RS-232 logic ////

void clearSerialBuffers() {
  serialMessageOutputBuffer[0] = NULL_TERMINATOR_CHAR;
  serialMessageInputBuffer[0] = NULL_TERMINATOR_CHAR;
  isSerialMessageComplete = false;
}

void disableForwardMode() {
  isSerialForwardModeEnabled = false;
  maxForwardReadTimer.reset();
  maxForwardReadTimer.stop();
}

void processSerialCommunications() {
  bool isForwardModeOff = (isSerialForwardModeEnabled == false);
  if (isForwardModeOff == true) {
    bool isMessageNotComplete = (isSerialMessageComplete == false);
    if (isMessageNotComplete == true) {
      int availableBytes = Serial.available();
      bool hasAvailableBytes = (availableBytes > 0);
      if (hasAvailableBytes == true) {
        char receivedCharacter = Serial.read();
        readIncomingSerialData(receivedCharacter, SERIAL_DATA_START_CHAR, SERIAL_DATA_END_CHAR);
      }
    } else {
      decodeIncomingMessage();
      bool isStillForwardModeOff = (isSerialForwardModeEnabled == false);
      if (isStillForwardModeOff == true) { 
        writeOutgoingSerialData(false); 
      }
      clearSerialBuffers();
    }
  }
  bool isForwardModeOn = (isSerialForwardModeEnabled == true);
  if (isForwardModeOn == true) {
    bool isExternalMessageNotComplete = (isSerialMessageComplete == false);
    if (isExternalMessageNotComplete == true) {
      int externalAvailableBytes = externalSerialConnection.available();
      bool hasExternalAvailableBytes = (externalAvailableBytes > 0);
      if (hasExternalAvailableBytes == true) {
        char externalReceivedCharacter = externalSerialConnection.read();
        readIncomingSerialData(externalReceivedCharacter, RESPONSE_DELIMITER_CHAR, RESPONSE_DELIMITER_CHAR);
      }
    } else {
      writeOutgoingSerialData(true);
      disableForwardMode();
      clearSerialBuffers();
    }
  }
  bool hasWaitExceeded = maxForwardReadTimer.done();
  if (hasWaitExceeded == true) {
    Serial.print("*EXT-232 MAX WAIT EXCEEDED!*\n");
    Serial.flush();
    disableForwardMode();
    clearSerialBuffers();
  }  
}

void readIncomingSerialData(char receivedCharacter, char startIndicator, char endIndicator) {
  bool isCurrentlyReading = (isSerialReading == true);
  if (isCurrentlyReading == true) {
    bool isEndIndicatorMatched = (receivedCharacter == endIndicator);
    if (isEndIndicatorMatched == true) {
      serialMessageInputBuffer[serialCurrentPosition] = NULL_TERMINATOR_CHAR;
      isSerialReading = false;
      serialCurrentPosition = 0;
      isSerialMessageComplete = true;
    } else {
      bool isNotStartIndicator = (receivedCharacter != startIndicator);
      if (isNotStartIndicator == true) {
        serialMessageInputBuffer[serialCurrentPosition] = receivedCharacter;
        serialCurrentPosition = serialCurrentPosition + 1;
        uint16_t maximumAllowedPosition = MAX_MESSAGE_LENGTH - 1;
        bool isPositionOverflowed = (serialCurrentPosition >= maximumAllowedPosition);
        if (isPositionOverflowed == true) {
          serialCurrentPosition = maximumAllowedPosition;
        }
      }
    }
  } else {
    bool isStartIndicatorMatched = (receivedCharacter == startIndicator);
    if (isStartIndicatorMatched == true) {
      isSerialReading = true;
      serialCurrentPosition = 0;
      isSerialMessageComplete = false;
    }
  }
}

void writeOutgoingSerialData(bool isInputBufferSource) {
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.print(RESPONSE_DELIMITER_CHAR);
  for (uint8_t bufferIndex = 0; bufferIndex < MAX_MESSAGE_LENGTH; bufferIndex++) {
    char characterToForward;
    bool useInputBuffer = (isInputBufferSource == true);
    if (useInputBuffer == true){
      characterToForward = serialMessageInputBuffer[bufferIndex];
    } else {
      characterToForward = serialMessageOutputBuffer[bufferIndex];
    }
    bool isNotNullTerminator = (characterToForward != NULL_TERMINATOR_CHAR);
    if (isNotNullTerminator == true) {
      Serial.print(characterToForward); 
    } else {
      break;
    }
  }
  Serial.print(RESPONSE_DELIMITER_CHAR);
  Serial.print('\n');
  Serial.flush();
  digitalWrite(LED_BUILTIN, LOW);
}

void decodeIncomingMessage() {
  uint8_t delimiterCount = 0;
  uint8_t maximumLineCharacters = 10;
  char delimiterCharacter = ',';
  for (uint8_t bufferIndex = 0; bufferIndex < MAX_MESSAGE_LENGTH; bufferIndex++) {
    char currentCharacter = serialMessageInputBuffer[bufferIndex];  
    bool isNullTerminated = (currentCharacter == NULL_TERMINATOR_CHAR);
    if (isNullTerminated == true) {
      break;
    }
    bool isDelimiterMatched = (currentCharacter == delimiterCharacter);
    if (isDelimiterMatched == true) {
      delimiterCount = delimiterCount + 1;
    }
  }
  bool isDelimiterCountInvalid = (delimiterCount < 2);
  if (isDelimiterCountInvalid == true) {
    Serial.print("*INVALID DELIMITERS!*");
    return;
  }
  uint8_t lineDelimiterPosition = 0;
  for (uint8_t bufferIndex = 0; bufferIndex < MAX_MESSAGE_LENGTH; bufferIndex++) {  
    char currentCharacter = serialMessageInputBuffer[bufferIndex];  
    bool isNullTerminated = (currentCharacter == NULL_TERMINATOR_CHAR);
    if (isNullTerminated == true) {
      break;
    }
    bool isDelimiterMatched = (currentCharacter == delimiterCharacter);
    if (isDelimiterMatched == true) {
      lineDelimiterPosition = bufferIndex;
      break;
    }
  }
  char lineCharacterBuffer[maximumLineCharacters + 1];
  uint8_t lineCharacterCount = 0;
  for (uint8_t bufferIndex = 0; bufferIndex < lineDelimiterPosition; bufferIndex++) {
    bool hasReachedMaximumCharacters = (lineCharacterCount >= maximumLineCharacters);
    if (hasReachedMaximumCharacters == true) break;
    lineCharacterBuffer[lineCharacterCount] = serialMessageInputBuffer[bufferIndex];
    lineCharacterCount = lineCharacterCount + 1;
  } 
  lineCharacterBuffer[lineCharacterCount] = NULL_TERMINATOR_CHAR;
  uint8_t extractedControlData = atoi(lineCharacterBuffer); 
  uint8_t matchingCount = 0;
  uint8_t secondCommandPosition = 0; 
  for (uint8_t bufferIndex = 0; bufferIndex < MAX_MESSAGE_LENGTH; bufferIndex++) {
    char currentCharacter = serialMessageInputBuffer[bufferIndex];
    bool isNullTerminated = (currentCharacter == NULL_TERMINATOR_CHAR);
    if (isNullTerminated == true) break;
    bool isDelimiterMatched = (currentCharacter == delimiterCharacter);
    if (isDelimiterMatched == true) {
      bool isSecondDelimiter = (matchingCount == 1);
      if (isSecondDelimiter == true) {
        secondCommandPosition = bufferIndex;
        break;
      }
      matchingCount = matchingCount + 1;
    }
  }
  bool isValidControlData = (extractedControlData == 9);
  if (isValidControlData == true) {
    uint8_t nextPositionToRead = secondCommandPosition + 1;
    processCommandData(nextPositionToRead);
  } else {
    Serial.print("*INVALID PREFIX!*");
    Serial.flush();
  }
}

void processCommandData(uint8_t messageStartPosition) {
  uint8_t extractedNumberCount = 0;
  uint8_t expectedCommandLength = COMMAND_REGISTER_LENGTH + COMMAND_DATA_LENGTH;
  char commandArray[expectedCommandLength + 1];
  for (uint8_t bufferIndex = messageStartPosition; bufferIndex < MAX_MESSAGE_LENGTH; bufferIndex++) {
    char currentCharacter = serialMessageInputBuffer[bufferIndex];
    bool isNullTerminated = (currentCharacter == NULL_TERMINATOR_CHAR);
    if (isNullTerminated == true) {
      break;
    }
    bool isCharacterDigit = isDigit(currentCharacter);
    bool isUnderCommandLength = (extractedNumberCount < expectedCommandLength);
    bool isValidToExtract = (isCharacterDigit == true) && (isUnderCommandLength == true);
    if (isValidToExtract == true) {
      commandArray[extractedNumberCount] = currentCharacter;
      extractedNumberCount = extractedNumberCount + 1;
    }
  }
  commandArray[extractedNumberCount] = NULL_TERMINATOR_CHAR;
  bool isInvalidCommandLength = (extractedNumberCount != expectedCommandLength);
  if (isInvalidCommandLength == true) {
    Serial.print("*INVALID LENGTH!*");
    Serial.flush();
    return;
  }
  char registerArray[COMMAND_REGISTER_LENGTH + 1];
  uint8_t registerIndex = 0;
  for (uint8_t bufferIndex = 0; bufferIndex < COMMAND_REGISTER_LENGTH; bufferIndex++) {
    registerArray[registerIndex] = commandArray[bufferIndex];
    registerIndex = registerIndex + 1;
  }
  registerArray[registerIndex] = NULL_TERMINATOR_CHAR;
  uint8_t extractedRegisterSelect = atoi(registerArray); 
  char dataArray[COMMAND_DATA_LENGTH + 1];
  uint8_t dataIndex = 0;
  uint8_t commandDataStartOffset = COMMAND_DATA_LENGTH - 1;
  for (uint8_t bufferIndex = commandDataStartOffset; bufferIndex < expectedCommandLength; bufferIndex++) {
    dataArray[dataIndex] = commandArray[bufferIndex];
    dataIndex = dataIndex + 1;
  }
  dataArray[dataIndex] = NULL_TERMINATOR_CHAR;
  uint16_t extractedControlData = atoi(dataArray);
  routeCommunicationMessage(false, extractedRegisterSelect, extractedControlData);
}

void routeCommunicationMessage(bool messageOrigin, uint8_t registerCode, uint16_t controlDataValue) {
  bool isLocalDeviceOrigin = (messageOrigin == true);
  if (isLocalDeviceOrigin == true) {
    bool isProcessingCompleteMessage = (isSerialMessageComplete == true);
    if (isProcessingCompleteMessage == true) {
      return;
    }
    bool isCurrentlyReadingSerial = (isSerialReading == true);
    if (isCurrentlyReadingSerial == true) {
      return;
    }
    bool isForwardingModeActive = (isSerialForwardModeEnabled == true);
    if (isForwardingModeActive == true) {
      return;
    }
  }
  bool isInternalRegister = (registerCode == 1);
  if (isInternalRegister == true) {
    processInternalCommands(controlDataValue);
  } else {
    if (isLocalDeviceOrigin == true) {
      serialMessageInputBuffer[0] = NULL_TERMINATOR_CHAR;
      uint8_t totalMessageLength = COMMAND_REGISTER_LENGTH + COMMAND_DATA_LENGTH + 1;
      snprintf(serialMessageInputBuffer, totalMessageLength, "%02d%03d", registerCode, controlDataValue);
    }
    externalSerialConnection.print(SERIAL_DATA_START_CHAR);
    if (isLocalDeviceOrigin == true) {
      externalSerialConnection.print("9,9,"); 
    }
    for (uint8_t bufferIndex = 0; bufferIndex < MAX_MESSAGE_LENGTH; bufferIndex++) {  
      char characterToSend;
      characterToSend = serialMessageInputBuffer[bufferIndex];
      bool isNullTerminator = (characterToSend == NULL_TERMINATOR_CHAR);
      if (isNullTerminator == true) {
        break;
      }
      externalSerialConnection.print(characterToSend);
    }
    externalSerialConnection.print(SERIAL_DATA_END_CHAR);
    externalSerialConnection.print('\n');
    Serial.flush();
    isSerialForwardModeEnabled = true;
    maxForwardReadTimer.set(MAX_FORWARD_WAIT_MS);
    maxForwardReadTimer.start();
  }
}
