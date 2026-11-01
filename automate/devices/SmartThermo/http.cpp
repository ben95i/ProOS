// =====================================================================================
// http.cpp
// SmartThermo: the HTTP server, the command decoder and the reply buffers. See http.h.
// =====================================================================================
#include <WiFi.h>
#include <stdarg.h>

#include "http.h"

// ---- command decoder: requests carry "?<command>,<unused>,<value>~" in the URL ----
const uint8_t COMMAND_MESSAGE_LENGTH = 32;
const uint8_t COMMAND_REPLY_LENGTH = 48;    // fits the one-line status of command 11
const uint8_t COMMAND_FIELD_MAX_DIGITS = 4;
const uint16_t COMMAND_FIELD_BASE = 10;
const char COMMAND_FRAME_START = '?';
const char COMMAND_FRAME_END = '~';
const char COMMAND_FIELD_DELIMITER = ',';
const uint8_t COMMAND_FIRST_FIELD_START = 0;
const uint8_t COMMAND_VALUE_DELIMITER_OCCURRENCE = 2; // the value follows the second delimiter
const uint8_t COMMAND_DELIMITER_NOT_FOUND = 0;        // the second delimiter can never be at position 0
char commandMessageIn[COMMAND_MESSAGE_LENGTH + 1];
char commandReplyOut[COMMAND_REPLY_LENGTH + 1];
uint16_t commandFramePosition = 0;
bool commandFrameReading = false;
HttpCommandHandler commandHandler = nullptr; // the API routes, set by startHttpServer()

// ---- HTTP server and buffers ----
const uint8_t HTTP_REQUEST_BUFFER_SIZE = 255;
const uint16_t HTTP_CLIENT_TIMEOUT_MS = 1000;   // client inactivity timeout
const uint16_t HTTP_BODY_BUFFER_SIZE = 6144;    // multi-line replies (log, status)
const uint8_t HTTP_LINE_ENDING_LENGTH = 1;      // the newline sent after every reply body
WiFiServer httpServer(HTTP_SERVER_PORT);
char httpRequestBuffer[HTTP_REQUEST_BUFFER_SIZE];
char httpBodyBuffer[HTTP_BODY_BUFFER_SIZE];
uint16_t httpBodyLength = 0;
bool httpReplyUsesBody = false; // true when the reply is in httpBodyBuffer instead of commandReplyOut

// ==== reply text ====

// Starts a multi-line reply in httpBodyBuffer, replacing the single-value reply.
void startBodyReply() {
  httpBodyLength = 0;
  httpBodyBuffer[0] = NULL_TERMINATOR;
  httpReplyUsesBody = true;
}

// Appends printf-style text, given as a va_list, to the multi-line reply; text that does
// not fit is cut off. The length is summed in 32 bits so it can never wrap around.
void appendBodyFormatted(const char *format, va_list arguments) {
  uint16_t lastUsableIndex = HTTP_BODY_BUFFER_SIZE - 1;
  if (httpBodyLength >= lastUsableIndex) {
    return;
  }
  char *writePosition = httpBodyBuffer + httpBodyLength;
  size_t spaceRemaining = HTTP_BODY_BUFFER_SIZE - httpBodyLength;
  int charactersWritten = vsnprintf(writePosition, spaceRemaining, format, arguments);
  if (charactersWritten > 0) {
    uint32_t extendedLength = (uint32_t)httpBodyLength + (uint32_t)charactersWritten;
    if (extendedLength > lastUsableIndex) {
      extendedLength = lastUsableIndex;
    }
    httpBodyLength = (uint16_t)extendedLength;
  }
}

// Appends printf-style text to the multi-line reply.
void appendBody(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  appendBodyFormatted(format, arguments);
  va_end(arguments);
}

// Appends a status line: the label padded to 21 characters, then the value. Indented lines
// put their two spaces at the start of the label.
void appendBodyField(const char *label, const char *format, ...) {
  appendBody("%-21s", label);
  va_list arguments;
  va_start(arguments, format);
  appendBodyFormatted(format, arguments);
  va_end(arguments);
}

// Sets a single number as the reply.
void replyWithNumber(int16_t value) {
  snprintf(commandReplyOut, sizeof(commandReplyOut), "%d", value);
}

// Sets a short text as the reply.
void replyWithText(const char *text) {
  snprintf(commandReplyOut, sizeof(commandReplyOut), "%s", text);
}

// Sets printf-style text as the reply, e.g. replyWithFormat("%d~%d", 1, 70) -> "1~70"; text
// beyond the reply buffer is cut off.
void replyWithFormat(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(commandReplyOut, sizeof(commandReplyOut), format, arguments);
  va_end(arguments);
}

// ==== command decoder ====

// Returns the position of the given delimiter occurrence (1 = first) in the command message,
// or COMMAND_DELIMITER_NOT_FOUND when the message has fewer delimiters.
uint8_t findCommandDelimiter(uint8_t occurrence) {
  uint8_t delimitersSeen = 0;
  for (uint8_t position = 0; position < COMMAND_MESSAGE_LENGTH; position++) {
    char character = commandMessageIn[position];
    if (character == NULL_TERMINATOR) {
      break;
    }
    if (character == COMMAND_FIELD_DELIMITER) {
      delimitersSeen = delimitersSeen + 1;
      if (delimitersSeen == occurrence) {
        return position;
      }
    }
  }
  return COMMAND_DELIMITER_NOT_FOUND;
}

// Parses the whole number in the field starting at startPosition into fieldValue. The field ends
// at a delimiter or the end of the message. Returns false unless it holds 1 to
// COMMAND_FIELD_MAX_DIGITS digits, so "x", an empty field or "00001" can never be read as another number.
bool parseCommandField(uint8_t startPosition, uint16_t &fieldValue) {
  uint16_t parsedNumber = 0;
  uint8_t fieldLength = 0;
  uint8_t readPosition = startPosition;
  // bounds guard: the message is always terminated before its buffer ends
  while (readPosition < COMMAND_MESSAGE_LENGTH) {
    char character = commandMessageIn[readPosition];
    bool reachedDelimiter = (character == COMMAND_FIELD_DELIMITER);
    bool reachedEnd = (character == NULL_TERMINATOR);
    if (reachedDelimiter || reachedEnd) {
      break;
    }
    bool isDigit = (character >= '0') && (character <= '9');
    bool fieldTooLong = (fieldLength >= COMMAND_FIELD_MAX_DIGITS);
    if (!isDigit || fieldTooLong) {
      return false;
    }
    uint16_t digitValue = character - '0';
    parsedNumber = (parsedNumber * COMMAND_FIELD_BASE) + digitValue;
    fieldLength = fieldLength + 1;
    readPosition = readPosition + 1;
  }
  bool fieldEmpty = (fieldLength == 0);
  if (fieldEmpty) {
    return false;
  }
  fieldValue = parsedNumber;
  return true;
}

// Decodes "<command>,<unused>,<value>" and runs it through the command handler; messages with
// fewer than two delimiters or a command or value that isn't a whole number are logged and
// ignored, which replies 400 ERR.
void decodeCommandMessage() {
  uint8_t valueDelimiterPosition = findCommandDelimiter(COMMAND_VALUE_DELIMITER_OCCURRENCE);
  bool enoughDelimiters = (valueDelimiterPosition != COMMAND_DELIMITER_NOT_FOUND);
  if (!enoughDelimiters) {
    logMessage("invalid command frame: %s", commandMessageIn);
    return;
  }
  uint16_t command = 0;
  uint16_t value = 0;
  uint8_t valueStartPosition = valueDelimiterPosition + 1;
  bool commandValid = parseCommandField(COMMAND_FIRST_FIELD_START, command);
  bool valueValid = parseCommandField(valueStartPosition, value);
  if (!commandValid || !valueValid) {
    logMessage("invalid command frame: %s", commandMessageIn);
    return;
  }
  bool handlerSet = (commandHandler != nullptr);
  if (handlerSet) {
    commandHandler(command, value);
  }
}

// Collects the characters between '?' and '~' and decodes the message at the closing '~'.
// Characters beyond the message buffer are dropped.
void processCommandCharacter(char receivedCharacter) {
  if (!commandFrameReading) {
    bool frameStarts = (receivedCharacter == COMMAND_FRAME_START);
    if (frameStarts) {
      commandFrameReading = true;
      commandFramePosition = 0;
    }
    return;
  }
  bool frameEnds = (receivedCharacter == COMMAND_FRAME_END);
  if (frameEnds) {
    commandMessageIn[commandFramePosition] = NULL_TERMINATOR;
    commandFrameReading = false;
    decodeCommandMessage();
    return;
  }
  bool isFrameStart = (receivedCharacter == COMMAND_FRAME_START);
  uint16_t lastStorablePosition = COMMAND_MESSAGE_LENGTH - 1;
  bool hasSpace = (commandFramePosition < lastStorablePosition);
  if (!isFrameStart && hasSpace) {
    commandMessageIn[commandFramePosition] = receivedCharacter;
    commandFramePosition = commandFramePosition + 1;
  }
}

// ==== HTTP server ====

// Writes a complete HTTP response with a plain-text body followed by a newline, in
// HTTP_REPLY_WRITES writes (each print is one, each println two).
void sendHttpResponse(WiFiClient &client, const char *status, const char *body) {
  size_t bodyLength = strlen(body);
  size_t contentLength = bodyLength + HTTP_LINE_ENDING_LENGTH;
  client.print("HTTP/1.1 ");
  client.println(status);
  client.println("Content-Type: text/plain");
  client.print("Content-Length: ");
  client.println(contentLength);
  client.println("Connection: close");
  client.println();
  client.print(body);
  client.print(NEW_LINE);
  client.flush(); // wait for the response to leave before the connection closes
}

// Decodes the command in the request line and replies. Every request starts with a clean
// decoder and an empty reply, so a half-received frame or an old result never leaks into it.
// Only the request line is parsed: headers such as Referer may also contain '?'.
void handleHttpRequest(WiFiClient &client, uint8_t requestLength) {
  commandFrameReading = false;
  commandReplyOut[0] = NULL_TERMINATOR;
  httpReplyUsesBody = false;
  for (uint8_t position = 0; position < requestLength; position++) {
    char character = httpRequestBuffer[position];
    bool endOfRequestLine = (character == CARRIAGE_RETURN) || (character == NEW_LINE) || (character == NULL_TERMINATOR);
    if (endOfRequestLine) {
      break;
    }
    processCommandCharacter(character);
  }
  bool noValidCommand = (commandReplyOut[0] == NULL_TERMINATOR);
  if (httpReplyUsesBody) {
    sendHttpResponse(client, "200 OK", httpBodyBuffer);
  } else if (noValidCommand) {
    sendHttpResponse(client, "400 Bad Request", "ERR");
  } else {
    sendHttpResponse(client, "200 OK", commandReplyOut);
  }
}

// Reads the request headers into httpRequestBuffer until the blank line that ends them;
// characters beyond the buffer are dropped. Returns true when a complete request arrived,
// false when the client disconnected, stalled for HTTP_CLIENT_TIMEOUT_MS or took longer than
// HTTP_REQUEST_DEADLINE_MS in all, so no client can hold up the thermostat.
bool readHttpRequest(WiFiClient &client, uint8_t &requestLength) {
  requestLength = 0;
  bool lineIsBlank = true;
  uint32_t requestStartMs = millis();
  uint32_t lastActivityMs = requestStartMs;
  while (client.connected()) {
    uint32_t currentMs = millis();
    uint32_t msSinceActivity = currentMs - lastActivityMs;
    uint32_t msSinceStart = currentMs - requestStartMs;
    bool timedOut = (msSinceActivity > HTTP_CLIENT_TIMEOUT_MS);
    bool pastDeadline = (msSinceStart > HTTP_REQUEST_DEADLINE_MS);
    if (timedOut || pastDeadline) {
      return false;
    }
    if (!client.available()) {
      continue;
    }
    int receivedValue = client.read();
    char receivedCharacter = (char)receivedValue;
    lastActivityMs = millis();
    uint8_t lastStorablePosition = HTTP_REQUEST_BUFFER_SIZE - 1;
    bool bufferHasSpace = (requestLength < lastStorablePosition);
    if (bufferHasSpace) {
      httpRequestBuffer[requestLength] = receivedCharacter;
      requestLength = requestLength + 1;
    }
    // the headers end with a blank line (\r\n followed by another \r\n)
    bool isNewLine = (receivedCharacter == NEW_LINE);
    if (isNewLine && lineIsBlank) {
      return true;
    }
    if (isNewLine) {
      lineIsBlank = true;
    } else if (receivedCharacter != CARRIAGE_RETURN) {
      lineIsBlank = false;
    }
  }
  return false;
}

// Serves one waiting HTTP client, if any: reads its request, replies, and closes it.
void serviceHttpClient() {
  WiFiClient client = httpServer.accept();
  if (!client) {
    return;
  }
  client.setNoDelay(true); // disable Nagle's algorithm
  client.setTimeout(HTTP_WRITE_TIMEOUT_MS);
  uint8_t requestLength = 0;
  bool requestComplete = readHttpRequest(client, requestLength);
  if (requestComplete) {
    handleHttpRequest(client, requestLength);
  }
  client.stop();
}

// Sets the function that runs each decoded command (the API routes) and starts listening on
// HTTP_SERVER_PORT; called once from setup(), after Wi-Fi has been started.
void startHttpServer(HttpCommandHandler handler) {
  commandHandler = handler;
  httpServer.begin();
}
