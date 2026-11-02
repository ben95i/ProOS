// Automate - message popup and failure reports (styles/error.css). Imports nothing,
// so every module can use it. Failures show as one of two kinds:
//   Not connected           no answer, or the proxy could not reach the API
//   Exception has occurred  an error answer, an unreadable answer, a page error

const POPUP_ID = 'msg-popup';
const POPUP_TEXT_ID = 'msg-text';
const POPUP_CLASS = 'popup-alert';
const POPUP_FADE_CLASS = 'fade-out';
const MESSAGE_MS = 4000;
const ERROR_MS = 7000;
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

// What browsers say when a request got no answer (Safari, Chrome, Firefox, older Safari)
const NETWORK_ERROR_TEXT =
  /load failed|failed to fetch|networkerror|network connection was lost|network request failed/i;

// lighttpd answers these itself while the node API is down
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

// drops a pending fade, for the page teardown
export function clearPopupTimer() {
  if (messagePopupTimer) {
    clearTimeout(messagePopupTimer);
    messagePopupTimer = null;
  }
}

// Removes a popup when its fade ends. Attached once for the popup's life: a message
// arriving mid-fade cancels the animation, so a per-fade listener would never fire.
export function removeOnFade(popupWindow) {
  popupWindow.addEventListener("animationend", () => {
    if (popupWindow.classList.contains(POPUP_FADE_CLASS) &&
        popupWindow.isConnected) {
      popupWindow.remove();
    }
  });
}

// Shows a message at the top of the page. message is markup or a node; options:
// popupClass, duration in ms. showPopup("[lists] saved.")
export function showPopup(message, options = {}) {
  let popupWindow = document.getElementById(POPUP_ID);
  if (!popupWindow) {
    popupWindow = document.createElement("div");
    popupWindow.id = POPUP_ID;
    const textBox = document.createElement("div");
    textBox.id = POPUP_TEXT_ID;
    popupWindow.appendChild(textBox);
    removeOnFade(popupWindow);
    document.body.appendChild(popupWindow);
  }
  // set whole, which also drops the fade
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
    // removeOnFade() takes the window off once the fade ends
    popupWindow.classList.add(POPUP_FADE_CLASS);
  }, duration);
}

//// errors ////

// An error status answer; body is quoted in the popup: throw new HttpError(response.status, text)
export class HttpError extends Error {
  // status and the answer's text
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
  if (HTTP_STATUS_TEXT[status]) {
    line = `${line} ${HTTP_STATUS_TEXT[status]}`;
  }
  return line;
}

// Does this error mean the connection is gone? A request with no answer rejects with a
// TypeError, worded per browser; while offline any TypeError counts.
function isConnectionError(error) {
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

// Logs a failure to the console only: logError('reading the weather', error)
export function logError(what, error) {
  console.error(`${LOG_PREFIX} ${what}:`, error);
}

// Reports a caught failure as Not connected or an exception, whichever the error is.
// reportError('sending lrxmit wl1_dual', error)
export function reportError(what, error) {
  if (isConnectionError(error)) {
    reportNotConnected(what, connectionSentence(error), error);
  } else {
    reportException(what, error);
  }
}

// Reports a lost connection, with reason as the sentence shown.
// reportNotConnected('sending the LED command stop', 'No answer from LEDgrid')
export function reportNotConnected(what, reason = '', error = null) {
  let sentence = reason;
  if (!sentence) {
    sentence = connectionSentence(error);
  }
  presentError(ERROR_KINDS.connection, what, sentence, error);
}

// Reports any other failure; reason is an Error or the sentence to show.
// reportException('uploading files', '2 files failed.')
export function reportException(what, reason) {
  if (reason instanceof Error) {
    presentError(ERROR_KINDS.exception, what, exceptionSentence(reason), reason);
    return;
  }
  presentError(ERROR_KINDS.exception, what, String(reason), null);
}

// the sentence under Not connected
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

// the sentence under Exception has occurred
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

// Logs a failure and shows it in place of whatever is on screen.
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

// The rows under the sentence: what the page was doing, the error, the server's answer,
// and where it was thrown. [{ label: 'While', value: 'sending lrxmit wl1_dual' }]
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

// A server answer cut to fit a popup: tags dropped, white space collapsed, shortened.
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

// the file and line an error was thrown from: 'bedrm.js:212', or ''
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

// The popup's content as nodes, so no server text is read as markup.
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

// an element with a class and plain text: popupElement('span', 'x', 'hi')
function popupElement(tag, className, text = '') {
  const element = document.createElement(tag);
  element.className = className;
  if (text) {
    element.textContent = text;
  }
  return element;
}

//// commands ////

// A command as the popup names it, 'main' left out and a long value shortened.
// ('main', 'brxmit', 'brtvon') -> 'brxmit brtvon'
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

// Reports anything thrown or rejected that no code caught; called once at page load.
export function installErrorHandlers() {
  window.addEventListener('error', (event) => {
    // a resource that failed to load fires here too, with no error
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
