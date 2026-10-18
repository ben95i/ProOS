// Automate Website - message popup, error popups and error logging
//
// The one message popup at the top of the page, and every report of a
// failure. A failure is one of two kinds:
//
//   Not connected           a request never got an answer, or the web server
//                           could not reach the API behind it (wifi-slash icon)
//   Exception has occurred  anything else: an error answer, an answer that
//                           does not read, an error thrown in the page (alert
//                           icon)
//
// Both are logged to the console in full and shown in the message popup:
// the icon and title, a sentence saying what went wrong, then what the page
// was doing, the error itself and the server's answer when there was one.
// Styles are in styles/error.css.
//
// This module imports nothing, so every other module can use it, utils.js
// included.

const POPUP_ID = 'msg-popup';
const POPUP_TEXT_ID = 'msg-text';
const POPUP_CLASS = 'popup-alert';
const POPUP_FADE_CLASS = 'fade-out';
const MESSAGE_MS = 4000;  // a message's time on screen
const ERROR_MS = 7000;    // an error's, which has more to read
const LOG_PREFIX = '[Automate]';

// longest server answer or command quoted in a popup; the console has it all
const ANSWER_MAX_CHARS = 160;
const COMMAND_MAX_CHARS = 60;


// the two kinds of failure
const ERROR_KINDS = {
  connection: {
    icon: 'fad fa-wifi-slash',
    title: 'Not connected',
    popupClass: 'popup-error popup-error--connection'
  },
  exception: {
    icon: 'fad fa-exclamation-triangle',
    title: 'Exception has occurred',
    popupClass: 'popup-error popup-error--exception'
  }
};

// What browsers say when a request got no answer at all: Safari "Load
// failed", Chrome "Failed to fetch", Firefox "NetworkError when attempting to
// fetch resource.", older Safari "The network connection was lost."
const NETWORK_ERROR_TEXT =
  /load failed|failed to fetch|networkerror|network connection was lost|network request failed/i;

// lighttpd answers these itself while it cannot reach the node API
const PROXY_DOWN_STATUS = [502, 503, 504];

const HTTP_STATUS_TEXT = {
  400: 'Bad Request',
  403: 'Forbidden',
  404: 'Not Found',
  408: 'Request Timeout',
  413: 'Payload Too Large',
  429: 'Too Many Requests',
  500: 'Internal Server Error',
  502: 'Bad Gateway',
  503: 'Service Unavailable',
  504: 'Gateway Timeout'
};

//// message popup ////

let messagePopupTimer = null;

// lets the page teardown drop a pending fade
export function clearPopupTimer() {
  if (messagePopupTimer) {
    clearTimeout(messagePopupTimer);
    messagePopupTimer = null;
  }
}

// Shows a message in the popup at the top of the page for four seconds. The
// message is markup, or a node the caller built. options.popupClass styles
// one kind of popup and goes with its message, and options.duration is its
// time on screen in ms.
// showPopup("[lists] saved.")
export function showPopup(message, options = {}) {
  let popupWindow = document.getElementById(POPUP_ID);
  if (!popupWindow) {
    popupWindow = document.createElement("div");
    popupWindow.id = POPUP_ID;
    const textBox = document.createElement("div");
    textBox.id = POPUP_TEXT_ID;
    popupWindow.appendChild(textBox);
    // Attached once, for the life of the window. A message arriving mid-fade
    // takes the class off again, which cancels the animation before it ends:
    // a {once:true} listener per fade would never fire and never unregister.
    popupWindow.addEventListener("animationend", () => {
      if (popupWindow.classList.contains(POPUP_FADE_CLASS) &&
          popupWindow.isConnected) {
        popupWindow.remove();
      }
    });
    document.body.appendChild(popupWindow);
  }
  // the class list is set whole, which also drops the fade
  let popupClass = POPUP_CLASS;
  if (options.popupClass) {
    popupClass = `${POPUP_CLASS} ${options.popupClass}`;
  }
  popupWindow.className = popupClass;
  const textBox = document.getElementById(POPUP_TEXT_ID);
  if (message instanceof Node) {
    textBox.replaceChildren(message);
  } else {
    textBox.innerHTML = message;
  }
  let duration = MESSAGE_MS;
  if (options.duration) {
    duration = options.duration;
  }
  clearPopupTimer();
  messagePopupTimer = setTimeout(() => {
    messagePopupTimer = null;
    // the listener above removes the window once the fade ends
    popupWindow.classList.add(POPUP_FADE_CLASS);
  }, duration);
}

//// errors ////

// An answer the server sent with an error status. body is the answer's text,
// quoted in the popup: throw new HttpError(response.status, text)
export class HttpError extends Error {
  constructor(status, body = '') {
    super(httpStatusLine(status));
    this.name = 'HttpError';
    this.status = status;
    this.body = String(body);
  }
}

// 502 -> 'HTTP 502 Bad Gateway'
function httpStatusLine(status) {
  let line = `HTTP ${status}`;
  if (Object.prototype.hasOwnProperty.call(HTTP_STATUS_TEXT, status)) {
    line = `${line} ${HTTP_STATUS_TEXT[status]}`;
  }
  return line;
}

// Whether an error means the connection to the server is gone, rather than
// the server or the page failing. A request with no answer rejects with a
// TypeError, worded per browser; while the device is offline any TypeError
// from a request counts. isConnectionError(new TypeError('Load failed'))
export function isConnectionError(error) {
  if (error instanceof HttpError) {
    return PROXY_DOWN_STATUS.includes(error.status);
  }
  if (!(error instanceof Error) || error.name !== 'TypeError') {
    return false;
  }
  if (navigator.onLine === false) {
    return true;
  }
  return NETWORK_ERROR_TEXT.test(error.message);
}

// Logs a failure to the console with everything known about it, and nothing
// else. logError('reading the weather', error)
export function logError(what, error) {
  console.error(`${LOG_PREFIX} ${what}:`, error);
}

// Reports a failure caught around a request or any other step, as Not
// connected or as an exception, whichever the error is.
// reportError('sending lrxmit wl1_dual', error)
export function reportError(what, error) {
  if (isConnectionError(error)) {
    reportNotConnected(what, connectionSentence(error), error);
  } else {
    reportException(what, error);
  }
}

// Reports a lost connection. reason is the sentence under the title, error
// the failure itself when there is one.
// reportNotConnected('sending the LED command stop', 'No answer from LEDgrid')
export function reportNotConnected(what, reason = '', error = null) {
  let sentence = reason;
  if (!sentence) {
    sentence = connectionSentence(error);
  }
  presentError(ERROR_KINDS.connection, what, sentence, error);
}

// Reports any other failure. reason is an Error, which explains itself, or
// the sentence to show: reportException('uploading files', '2 files failed.')
export function reportException(what, reason) {
  if (reason instanceof Error) {
    presentError(ERROR_KINDS.exception, what, exceptionSentence(reason), reason);
    return;
  }
  presentError(ERROR_KINDS.exception, what, String(reason), null);
}

// The sentence under Not connected, from what is known about the failure.
function connectionSentence(error) {
  const host = location.hostname;
  if (navigator.onLine === false) {
    return 'This device has no network connection.';
  }
  if (error instanceof HttpError) {
    return `${host} is up, but the Automate API behind it did not answer.`;
  }
  return `No answer from ${host}. The connection dropped or the server is down.`;
}

// The sentence under Exception has occurred, from the error's own kind.
function exceptionSentence(error) {
  if (error instanceof HttpError) {
    const clientErrorTop = 500;
    if (error.status < clientErrorTop) {
      return 'The server refused the request.';
    }
    return 'The server failed while answering.';
  }
  if (error.name === 'SyntaxError') {
    return 'The server\'s answer could not be read.';
  }
  return 'Part of the page failed.';
}

// Logs a failure and shows it in place of whatever is on screen, so the
// popup always names the newest failure.
function presentError(kind, what, sentence, error) {
  const logLine = `${LOG_PREFIX} ${kind.title} while ${what}: ${sentence}`;
  if (error === null) {
    console.error(logLine);
  } else {
    console.error(logLine, error);
  }
  const content = errorPopupContent(kind, sentence, errorRows(what, error));
  showPopup(content, {
    popupClass: kind.popupClass,
    duration: ERROR_MS
  });
}

// The labelled rows under the sentence: what the page was doing, the error,
// the server's answer to a request it refused, and where in the page an
// error was thrown.
// [{ label: 'While', value: 'sending lrxmit wl1_dual' },
//  { label: 'Error', value: 'TypeError: Load failed' }]
function errorRows(what, error) {
  const rows = [{ label: 'While', value: what }];
  if (!(error instanceof Error)) {
    return rows;
  }
  if (error instanceof HttpError) {
    rows.push({ label: 'Error', value: error.message });
    const answer = answerExcerpt(error.body);
    if (answer) {
      rows.push({ label: 'Answer', value: answer });
    }
    return rows;
  }
  rows.push({ label: 'Error', value: `${error.name}: ${error.message}` });
  if (isConnectionError(error)) {
    return rows; // the stack only points at the fetch
  }
  const origin = errorOrigin(error);
  if (origin) {
    rows.push({ label: 'At', value: origin });
  }
  return rows;
}

// A server answer cut down to fit a popup: tags dropped (a proxy's error
// page is HTML), runs of white space made one space, then shortened.
function answerExcerpt(body) {
  const withoutTags = String(body).replace(/<[^>]*>/g, ' ');
  const oneLine = withoutTags.replace(/\s+/g, ' ').trim();
  return shortenText(oneLine, ANSWER_MAX_CHARS);
}

// 'abcdef', 4 -> 'abc…'
function shortenText(text, maxChars) {
  if (text.length <= maxChars) {
    return text;
  }
  const keptChars = maxChars - 1;
  return `${text.slice(0, keptChars)}…`;
}

// The file and line an error was thrown from, from the first frame of its
// stack that names one: 'bedrm.js:212'. Empty when there is none.
function errorOrigin(error) {
  if (typeof error.stack !== 'string') {
    return '';
  }
  const frame = error.stack.match(/([^/\s(@]+\.js):(\d+)(?::\d+)?/);
  if (!frame) {
    return '';
  }
  return `${frame[1]}:${frame[2]}`;
}

// The popup's content as nodes, so no server text is ever read as markup.
function errorPopupContent(kind, sentence, rows) {
  const content = popupElement('div', 'error-popup');
  const head = popupElement('div', 'error-popup__head');
  head.append(
    popupElement('i', `error-popup__icon ${kind.icon}`),
    popupElement('span', 'error-popup__title', kind.title)
  );
  content.append(head, popupElement('div', 'error-popup__text', sentence));
  const details = popupElement('div', 'error-popup__rows');
  rows.forEach((row) => {
    const line = popupElement('div', 'error-popup__row');
    line.append(
      popupElement('span', 'error-popup__label', row.label),
      popupElement('span', 'error-popup__value', row.value)
    );
    details.appendChild(line);
  });
  content.appendChild(details);
  return content;
}

// An element with a class and plain text: popupElement('span', 'x', 'hi')
function popupElement(tag, className, text = '') {
  const element = document.createElement(tag);
  element.className = className;
  if (text) {
    element.textContent = text;
  }
  return element;
}

//// commands ////

// A command as the popup names it, the default 'main' action left out and a
// long value (base64 text) shortened: ('main', 'brxmit', 'brtvon') ->
// 'brxmit brtvon'
export function describeCommand(action, arg, value) {
  const parts = [];
  if (action !== 'main') {
    parts.push(action);
  }
  parts.push(arg);
  if (value) {
    parts.push(shortenText(String(value), COMMAND_MAX_CHARS));
  }
  return parts.join(' ');
}

//// uncaught errors ////

let errorHandlersInstalled = false;

// Reports anything thrown or rejected that no code caught. Installed once,
// as the page loads: installErrorHandlers()
export function installErrorHandlers() {
  if (errorHandlersInstalled) {
    return;
  }
  errorHandlersInstalled = true;
  window.addEventListener('error', (event) => {
    // a resource that failed to load (an <img>) fires here too, with no error
    if (!(event.error instanceof Error)) {
      return;
    }
    reportError('running the page', event.error);
  });
  window.addEventListener('unhandledrejection', (event) => {
    const reason = event.reason;
    // a request the page cancelled on purpose is not a failure
    if (reason instanceof Error && reason.name === 'AbortError') {
      return;
    }
    if (reason instanceof Error) {
      reportError('running the page', reason);
      return;
    }
    reportException('running the page', `A task failed: ${String(reason)}`);
  });
}
