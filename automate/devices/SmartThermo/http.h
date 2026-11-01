// =====================================================================================
// http.h
// SmartThermo: the HTTP server on port 4000 and the command decoder. Every request carries one
// command framed as "?<command>,<unused>,<value>~" in its URL; the decoder hands the command and
// value to the handler given to startHttpServer() (the API routes in SmartThermo.ino), which
// builds the reply with the functions below. Taken from BathroomLight.ino. Runs on core 0.
// =====================================================================================
#pragma once

#include "common.h"

const uint16_t HTTP_SERVER_PORT = 4000;
const uint16_t HTTP_REQUEST_DEADLINE_MS = 2000; // longest one request may take, however slowly it trickles in
const uint16_t HTTP_WRITE_TIMEOUT_MS = 500;     // longest one write of a reply may wait for the client
const uint16_t HTTP_REPLY_WRITES = 13;          // the writes in one reply (see sendHttpResponse)
const uint16_t HTTP_REPLY_FLUSH_MS = 600;       // the flush and client.stop(), which wait 300 ms each
const uint16_t HTTP_REPLY_WRITES_MS = HTTP_WRITE_TIMEOUT_MS * HTTP_REPLY_WRITES;
const uint16_t HTTP_REPLY_DEADLINE_MS = HTTP_REPLY_WRITES_MS + HTTP_REPLY_FLUSH_MS; // longest one reply may take

// runs one decoded command, e.g. ?4,0,1~ -> handler(4, 1)
typedef void (*HttpCommandHandler)(uint16_t command, uint16_t value);

void startHttpServer(HttpCommandHandler handler);
void serviceHttpClient();

// reply building, for the command handler
void replyWithNumber(int16_t value);
void replyWithText(const char *text);
void replyWithFormat(const char *format, ...);
void startBodyReply();
void appendBody(const char *format, ...);
void appendBodyField(const char *label, const char *format, ...);
