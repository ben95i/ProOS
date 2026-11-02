// Automate - shared utilities: API calls, file read / write, DOM helpers, button
// templates and delegated listeners, colour maths, search row, drag re-order, edit lock.

import { HttpError, reportError, reportNotConnected, logError,
  describeCommand } from './error.js';

export const REFRESH_INTERVAL = 10; // seconds, room status refresh
export const TIMEOUT_INTERVAL = 50; // seconds, inactivity before services pause

//// API calls ////

// Only characters that would end a query value or read as an escape are encoded.
const QUERY_UNSAFE = /[%&#+=?\s]/g;

// a query value, encoded
function encodeArg(value) {
  const text = (value === null || value === undefined) ? '' : String(value);
  return text.replace(QUERY_UNSAFE, (ch) =>
    '%' + ch.charCodeAt(0).toString(16).toUpperCase().padStart(2, '0'));
}

// Fetches a URL and answers its text; an error status throws HttpError.
export async function fetchAnswer(url) {
  const response = await fetch(url, { method: 'GET' });
  const answerText = await response.text();
  if (!response.ok) {
    throw new HttpError(response.status, answerText);
  }
  return answerText;
}

// Sends a command and answers the parsed JSON, or null with the failure reported.
// options.quiet logs it to the console only.
// sendCmd('main', 'brxmit', 'brtvon')
export async function sendCmd(act, arg1, arg2 = '', options = {}) {
  const url = location.protocol + "//" + location.hostname + "/api?var=" +
    encodeArg(arg2) + "&arg=" + encodeArg(arg1) + "&action=" + encodeArg(act);
  try {
    const answerText = await fetchAnswer(url);
    return JSON.parse(answerText);
  } catch (error) {
    const what = `sending ${describeCommand(act, arg1, arg2)}`;
    if (options.quiet === true) {
      logError(what, error);
    } else {
      reportError(what, error);
    }
    return null;
  }
}

// Reads a menu file at a full URL as text; undefined with the failure reported.
// loadMenu(fileURL('read', 'ledeffectmenu'))
export async function loadMenu(url) {
  const fileName = fileNameOf(url);
  try {
    return await fetchAnswer(url);
  } catch (error) {
    reportError(`reading the ${fileName} menu`, error);
    return undefined;
  }
}

//// Pico controllers ////
// A Pico serves one request at a time; errors arrive as text under HTTP 200.

// picoReplyError()'s answers; the offline text is the caller's own
export const PICO_REFUSED = 'Value out of range';
export const PICO_NO_REPLY = 'No reply from server';
export const PICO_CMD_ERROR = 'Command error';

// checked in this order: the first two are both "commonAPIRequest" replies
const PICO_OFFLINE_PREFIX = 'commonAPIRequest exception';
const PICO_REFUSED_PREFIX = 'commonAPIRequest: invalid response';
const PICO_ERROR_PREFIXES = ['commonAPIRequest', 'executeMain:'];

// '' for a good reply, or a short line saying what went wrong.
// picoReplyError('commonAPIRequest exception: …', 'bthrXmit:', 'Light offline') -> 'Light offline'
export function picoReplyError(reply, xmitPrefix, offlineText) {
  if (reply === null || reply === undefined) {
    return PICO_NO_REPLY;
  }
  const text = String(reply);
  if (text.startsWith(PICO_OFFLINE_PREFIX)) {
    return offlineText;
  }
  if (text.startsWith(PICO_REFUSED_PREFIX)) {
    return PICO_REFUSED;
  }
  const errorPrefixes = [xmitPrefix].concat(PICO_ERROR_PREFIXES);
  for (const prefix of errorPrefixes) {
    if (text.startsWith(prefix)) {
      return PICO_CMD_ERROR;
    }
  }
  return '';
}

// One queue per Pico, so commands reach it one after another. Answers the send function.
// const bthrRequest = picoRequestQueue('bthrxmit'); bthrRequest('bthr-light1_config')
export function picoRequestQueue(target) {
  let queue = Promise.resolve();
  return (command) => {
    const running = queue.then(() => sendCmd('main', target, command));
    queue = running;
    return running;
  };
}

// A reply that should be one number: "71" -> { answer: '71', error: '' }, or
// { answer: '', error } with what went wrong.
export function parsePicoNumber(reply, replyError) {
  const error = replyError(reply);
  if (error !== '') {
    return { answer: '', error: error };
  }
  const answer = cleanString(String(reply));
  if (isNumeric(answer) !== true) {
    return { answer: '', error: PICO_CMD_ERROR };
  }
  return { answer: answer, error: '' };
}

// Reads a Pico's text read-out into win.logBox. A closed window or a newer read
// (win.closed, win.logSeq) drops the reply; scrollToEnd opens on the last line.
export async function readPicoLog(win, request, replyError, command, scrollToEnd) {
  win.logSeq += 1;
  const sequence = win.logSeq;
  const reply = await request(command);
  if (win.closed === true || sequence !== win.logSeq) {
    return;
  }
  const error = replyError(reply);
  if (error !== '') {
    win.logBox.value = error;
    return;
  }
  win.logBox.value = String(reply).replace(/\r\n/g, '\n');
  if (scrollToEnd === true) {
    win.logBox.scrollTop = win.logBox.scrollHeight;
  }
}

//// file transfer ////

// read / write URL for one file on the device: fileURL('read', 'lists.save')
export function fileURL(action, file) {
  return `${location.protocol}//${location.hostname}/api/${action}?file=${file}`;
}

// the file a fileURL() names: '…/api/read?file=lists.save' -> 'lists.save'
function fileNameOf(url) {
  return new URL(url, location.href).searchParams.get('file');
}

// A request lost on the way (no answer, or the proxy's 502-504) is retried after
// these pauses. An answer from the API stands; every write replaces the whole file.
const FILE_RETRY_MS = [250, 750];
const FILE_RETRY_STATUS = [502, 503, 504];

// Last failed file request for reportFileFailure(): { file, status, text, attempts,
// time }, status null when nothing answered.
const FILE_FAILURE_MAX_AGE_MS = 10000;
let lastFileFailure = null;

// { ok, status, text } once the server answered, or null when it never did. The body
// is read in the same try, so a connection dropped mid-answer counts as lost.
async function fileRequest(url, options) {
  let attempt = 0;
  while (true) {
    let answer = null;
    try {
      const response = await fetch(url, options);
      answer = {
        ok: response.ok,
        status: response.status,
        text: await response.text()
      };
    } catch (error) {
      answer = null;
    }
    let lost = true;
    if (answer !== null) {
      lost = FILE_RETRY_STATUS.includes(answer.status);
    }
    if (!lost || attempt >= FILE_RETRY_MS.length) {
      if (answer === null || answer.ok !== true) {
        recordFileFailure(fileNameOf(url), answer, attempt);
      }
      return answer;
    }
    await sleep(FILE_RETRY_MS[attempt]);
    attempt += 1;
  }
}

// keeps one failed file request for reportFileFailure()
function recordFileFailure(fileName, answer, attempt) {
  const attempts = attempt - -1;
  let status = null;
  let text = '';
  if (answer !== null) {
    status = answer.status;
    text = answer.text;
  }
  lastFileFailure = {
    file: fileName,
    status: status,
    text: text,
    attempts: attempts,
    time: Date.now()
  };
}

// Reports why a file request just failed: Not connected when nothing answered, or
// an exception with the server's answer. reportFileFailure('saving the bookmarks')
export function reportFileFailure(what) {
  const failure = lastFileFailure;
  let failureAge = Infinity;
  if (failure !== null) {
    failureAge = Date.now() - failure.time;
  }
  if (failureAge > FILE_FAILURE_MAX_AGE_MS) {
    reportNotConnected(what);
    return;
  }
  const doing = `${what} (${failure.file})`;
  if (failure.status === null) {
    let tries = `${failure.attempts} tries`;
    if (failure.attempts === 1) {
      tries = '1 try';
    }
    reportNotConnected(doing, `No answer from ${location.hostname} after ${tries}.`);
    return;
  }
  reportError(doing, new HttpError(failure.status, failure.text));
}

// the file text, or null when the server cannot be reached
export async function readFile(file) {
  const answer = await fileRequest(fileURL('read', file), { method: 'GET' });
  if (answer === null || answer.ok !== true) {
    return null;
  }
  return answer.text;
}

// true only when the server confirms the write
export async function writeFile(file, text) {
  const answer = await fileRequest(fileURL('write', file), {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: text
  });
  return answer !== null && answer.ok === true;
}

// Writes a file and calls onSuccess(file) once taken; a failure is reported.
// savePOST('pwd', 'secret', afterPOSTActions)
export function savePOST(file, data, onSuccess) {
  writeFile(file, data).then((sent) => {
    if (sent !== true) {
      reportFileFailure('saving the file');
      return;
    }
    onSuccess(file);
  });
}

// Hands a file to the browser to send as the page goes away, when ordinary requests
// get cancelled. Answers only whether the browser accepted it.
export function sendFileBeacon(file, text) {
  const blob = new Blob([text], { type: 'text/plain' });
  return navigator.sendBeacon(fileURL('write', file), blob) === true;
}

//// text ////

// file text -> one entry per line, with the API's surrounding quotes dropped
export function splitMenuData(data) {
  const start = data.charCodeAt(0) === 34 ? 1 : 0; // 34 is '"'
  const end = data.charCodeAt(data.length - 1) === 34 ? data.length - 1 : data.length;
  return data.slice(start, end).split(/\\n|\n/);
}

// strips line endings; null in, null out
export function cleanString(data) {
  if (data === null || data === undefined) {
    return null;
  }
  return data.replace(/(\r\n|\n|\r)/gm, '');
}

// file text -> its non-empty lines, line endings stripped
export function fileRows(text) {
  return splitMenuData(text)
    .map(row => cleanString(row))
    .filter(row => row);
}

// 'a-b-c' -> ['a', 'b-c']; only the first delimiter splits
export function splitByDelimiter(str, delimiter = '-') {
  if (!str) return ['', ''];
  const parts = str.split(delimiter);
  const before = parts[0] || '';
  let after = '';
  if (parts.length > 1) {
    after = parts.slice(1).join(delimiter);
  }
  return [before, after];
}

// true for a string holding a whole number ("" and " " are not)
export function isNumeric(value) {
  return typeof value === 'string' && value.trim() !== '' &&
    Number.isInteger(Number(value));
}

// Palette file (`hex|name` per line) -> [{ color, name }]. A line with no colour is
// dropped; a value without its "#" still reads.
export function parsePalette(fileText) {
  const colors = [];
  if (!fileText || typeof fileText !== 'string') {
    return colors;
  }
  for (const line of splitMenuData(fileText)) {
    const row = line.trim();
    if (row === '') {
      continue;
    }
    const field = row.split('|');
    let color = field[0].trim();
    if (color === '') {
      continue;
    }
    if (!color.startsWith('#')) {
      color = `#${color}`;
    }
    colors.push({ color: color, name: (field[1] || '').trim() });
  }
  return colors;
}

// cuts a name to length, marking that it was cut
export function boundedText(name, limit) {
  if (name.length >= limit) {
    return `${name.substring(0, limit)}...`;
  }
  return name;
}

// one dynamic menu row: menuRow('cmd~lrxmit~usb', 'grnind', 'USB')
export function menuRow(col0, col1, col2) {
  return `${col0}|${col1}|${col2}\n`;
}

// the indicator column for an on / off row
export function indicator(on) {
  if (on === true) {
    return 'grnind';
  }
  return 'blkind';
}

//// base64 ////

// binary string -> URL-safe base64
export function base64URLSafeEncode(buffer) {
  return btoa(buffer)
    .replace(/\+/g, '-')  // '+' -> '-'
    .replace(/\//g, '_')  // '/' -> '_'
    .replace(/\=/g, '@'); // '=' -> '@'
}

// URL-safe base64 -> standard alphabet, for a data: URL. Does not decode.
export function base64URLSafeToStandard(data) {
  if (!data) {
    return '';
  }
  return data
    .replace(/-/g, '+')
    .replace(/_/g, '/')
    .replace(/@/g, '=');
}

//// DOM helpers ////

// Tag for DOM markup; strips whitespace between tags, which would render as gaps
// between inline-block elements. html`<div id="${id}"></div>`
export function html(strings, ...values) {
  return strings
    .reduce((out, str, i) => out + str + (i < values.length ? values[i] : ""), "")
    .replace(/>\s+</g, "><")
    .trim();
}

// new element with its class and, optionally, its markup
export function makeElement(tag, className = '', innerHTML = '') {
  const elem = document.createElement(tag);
  elem.className = className;
  if (innerHTML) {
    elem.innerHTML = innerHTML;
  }
  return elem;
}

// new SVG element: svgElement('circle', { cx: '25' })
export function svgElement(tag, attributes = {}) {
  const elem = document.createElementNS("http://www.w3.org/2000/svg", tag);
  Object.keys(attributes).forEach(name => {
    elem.setAttribute(name, attributes[name]);
  });
  return elem;
}

// sets the display style of every element carrying a class
export function classDisplay(className, state) {
  const elements = document.getElementsByClassName(className);
  for (let index = 0; index < elements.length; index++) {
    elements[index].style.display = state;
  }
}

// display style of one element by id, when it is on the page
export function setDisplayById(id, state) {
  const elem = document.getElementById(id);
  if (elem) {
    elem.style.display = state;
  }
}

// is this element on the page and shown as a block
export function isBlockDisplayed(id) {
  const elem = document.getElementById(id);
  return !!elem && elem.style.display === 'block';
}

// removes an element by id
export function removeWindow(id) {
  const win = document.getElementById(id);
  if (win) {
    document.body.removeChild(win);
  }
}

// Draws a centred prompt window: promptWindow('bed__prompt', '<div>Bed</div>')
export function promptWindow(id, markup, className = 'prompt_win') {
  const win = makeElement('div', className, markup);
  win.id = id;
  document.body.appendChild(win);
  return win;
}

// Opens a URL in a new tab: GoToExtPage('media.home')
export function GoToExtPage(_path) {
  window.open("https://" + _path, "_blank");
}

// Calls handler(button) for every button press inside a window.
export function onButtons(win, handler) {
  win.addEventListener("click", (click_event) => {
    const button = click_event.target.closest('button');
    if (button) {
      handler(button);
    }
  });
}

// Swaps a button's label for its spinner: setSpinner('hfinmenu-text', 'hfinmenu-spinner', true)
export function setSpinner(textId, spinnerId, on, spinnerClass = 'btn-spinner') {
  const text = document.getElementById(textId);
  const spinner = document.getElementById(spinnerId);
  if (!text || !spinner) {
    return;
  }
  if (on === true) {
    text.style.visibility = 'hidden';
    spinner.classList.add(spinnerClass);
  } else {
    // cleared rather than set to visible, so a hidden panel around it keeps it hidden
    text.style.visibility = '';
    spinner.classList.remove(spinnerClass);
  }
}

//// button templates ////

// A button's inside: its icon, then its label. buttonLabelMarkup('fad fa-lamp-floor', 'Lamp')
function buttonLabelMarkup(iconClass, labelText, iconId) {
  let labelMarkup = '';
  if (iconClass) {
    let iconIdAttribute = '';
    if (iconId) {
      iconIdAttribute = ` id="${iconId}"`;
    }
    labelMarkup = `<i class="${iconClass}"${iconIdAttribute}></i>`;
  }
  if (labelText) {
    if (labelMarkup !== '') {
      labelMarkup += ' ';
    }
    labelMarkup += labelText;
  }
  return labelMarkup;
}

// buttonClassNames('svropt_button') -> "button svropt_button"
function buttonClassNames(extraClass) {
  if (extraClass) {
    return `button ${extraClass}`;
  }
  return 'button';
}

// A plain button: a grid button or one of a window's own.
// config: buttonId, optional icon, iconId, label, buttonClass
export function gridButtonTemplate(config) {
  const labelMarkup = buttonLabelMarkup(config.icon, config.label, config.iconId);
  const buttonClass = buttonClassNames(config.buttonClass);
  return `
  <button type="button" class="${buttonClass}" id="${config.buttonId}">
    <span class="button__text">${labelMarkup}</span>
  </button>`;
}

// A grid button with the dropdown under it.
// config: buttonId, menuId, optional icon, iconId, label, buttonClass,
// textId + spinnerId (the label and its spinner), menuClass ("ind-button" by default
// or "dd-button"), menuContent (rows held from the start)
export function dropdownButtonTemplate(config) {
  const labelMarkup = buttonLabelMarkup(config.icon, config.label, config.iconId);
  const buttonClass = buttonClassNames(config.buttonClass);
  let menuClass = 'ind-button';
  if (config.menuClass) {
    menuClass = config.menuClass;
  }
  let menuContent = '';
  if (config.menuContent) {
    menuContent = config.menuContent;
  }
  // a button with no spinner has a label with no id
  let buttonContent = `
      <span class="button__text">${labelMarkup}</span>`;
  if (config.spinnerId) {
    buttonContent = `
      <span class="button__text" id="${config.textId}">${labelMarkup}</span>
      <span class="button__text" id="${config.spinnerId}"></span>`;
  }
  return `
  <div class="dropdown">
    <button type="button" class="${buttonClass}" id="${config.buttonId}">${buttonContent}
    </button>
    <div class="dd-content ${menuClass}" id="${config.menuId}">${menuContent}</div>
  </div>`;
}

//// delegated button listeners ////

// { elementId: handler }, filled by each template's registerListeners() call and
// read by the page's one click listener, so markup drawn later needs no binding.
const DELEGATED_LISTENERS = {};

// Adds { elementId: handler } pairs; the first handler for an id stays.
export function registerListeners(listeners) {
  for (const [elementId, handler] of Object.entries(listeners)) {
    if (!(elementId in DELEGATED_LISTENERS)) {
      DELEGATED_LISTENERS[elementId] = handler;
    }
  }
}

// Runs the handler of the nearest element with one, walking up from the click.
// Answers true when one ran.
export function dispatchRegisteredListener(clickedElement) {
  let currentElement = clickedElement;
  while (currentElement && currentElement !== document) {
    if (currentElement.id && DELEGATED_LISTENERS[currentElement.id]) {
      DELEGATED_LISTENERS[currentElement.id]();
      return true;
    }
    currentElement = currentElement.parentNode;
  }
  return false;
}

//// timing ////

// a promise that resolves after ms
export function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

// Calls func at most once per limit ms, the last call of a burst on the trailing edge.
// cancel() drops a trailing call that has not fired.
export function requestThrottle(func, limit) {
  let lastFunc;
  let lastRan;
  const throttled = (...args) => {
    if (!lastRan) {
      func(...args);
      lastRan = Date.now();
    } else {
      clearTimeout(lastFunc);
      lastFunc = setTimeout(() => {
        if ((Date.now() - lastRan) >= limit) {
          func(...args);
          lastRan = Date.now();
        }
      }, limit - (Date.now() - lastRan));
    }
  };
  throttled.cancel = () => {
    clearTimeout(lastFunc);
    lastFunc = undefined;
  };
  return throttled;
}

//// in-flight promises ////

// Holds a started promise on owner[key] until it settles.
export function trackPromise(owner, key, running) {
  owner[key] = running;
  // the catch stops the .finally() promise surfacing a rejection the caller handles
  running.finally(() => {
    if (owner[key] === running) {
      owner[key] = null;
    }
  }).catch(() => {});
  return running;
}

// Returns the promise in flight on owner[key], or starts one with factory().
export function singleFlight(owner, key, factory) {
  if (owner[key]) {
    return owner[key];
  }
  return trackPromise(owner, key, factory());
}

// Waits for a tracked promise however it settles; its starter reports failures.
export async function awaitQuiet(promise) {
  if (!promise) {
    return;
  }
  try {
    await promise;
  } catch (error) {
    // reported by the caller that started it
  }
}

//// maths ////

// re-maps a number from one range onto another: mapNumber(96, 0, 192, 0, 100)
export function mapNumber(num, inMin, inMax, outMin, outMax) {
  const offsetFromMin = num - inMin;
  const inRange = inMax - inMin;
  const outRange = outMax - outMin;
  return offsetFromMin * outRange / inRange + outMin;
}

//// colour ////

// '#FF8000' -> { red: 255, green: 128, blue: 0 }
export function hexToRgb(hex_in) {
  const hex = hex_in.replace(/^#/, '');
  return {
    red: parseInt(hex.substring(0, 2), 16),
    green: parseInt(hex.substring(2, 4), 16),
    blue: parseInt(hex.substring(4, 6), 16)
  };
}

// (255, 128, 0) -> '#FF8000'
export function rgbToHex(red, green, blue) {
  const redHex = red.toString(16).padStart(2, '0');
  const greenHex = green.toString(16).padStart(2, '0');
  const blueHex = blue.toString(16).padStart(2, '0');
  return `#${redHex}${greenHex}${blueHex}`.toUpperCase();
}

// 'rgb(255, 0, 0)' -> '#FF0000', or null when it does not parse
export function rgbToHexFromStyle(rgbString) {
  const rgbValues = rgbString.match(/\d+/g);
  if (!rgbValues || rgbValues.length < 3) return null;
  const red = parseInt(rgbValues[0]);
  const green = parseInt(rgbValues[1]);
  const blue = parseInt(rgbValues[2]);
  return rgbToHex(red, green, blue);
}

// The hue wheel's six 60 degree faces, each naming how it builds red, green and blue.
const HUE_SEGMENTS = [
  ['chroma', 'ramp', 'zero'],
  ['ramp', 'chroma', 'zero'],
  ['zero', 'chroma', 'ramp'],
  ['zero', 'ramp', 'chroma'],
  ['ramp', 'zero', 'chroma'],
  ['chroma', 'zero', 'ramp']
];

// hue 0-360, saturation and value 0-1 -> { red, green, blue } 0-255
export function hsvToRgb(hue, saturation, value) {
  // wraps into [0, 360), so hue 360 (slider at max) maps back to red
  const wrappedHue = ((hue % 360) + 360) % 360;
  const chroma = value * saturation;
  const hueSegment = wrappedHue / 60;
  const ramp = chroma * (1 - Math.abs((hueSegment % 2) - 1));
  const valueMatch = value - chroma;
  const parts = { chroma: chroma, ramp: ramp, zero: 0 };
  const segment = HUE_SEGMENTS[Math.floor(hueSegment)];
  const redRaw = parts[segment[0]];
  const greenRaw = parts[segment[1]];
  const blueRaw = parts[segment[2]];
  return {
    red: Math.round((redRaw + valueMatch) * 255),
    green: Math.round((greenRaw + valueMatch) * 255),
    blue: Math.round((blueRaw + valueMatch) * 255)
  };
}

// (255, 128, 0) -> { hue: 30, saturation: 1, value: 1 }
export function rgbToHsv(red, green, blue) {
  const redNormalized = red / 255;
  const greenNormalized = green / 255;
  const blueNormalized = blue / 255;
  const maxColorComponent = Math.max(redNormalized, greenNormalized, blueNormalized);
  const minColorComponent = Math.min(redNormalized, greenNormalized, blueNormalized);
  const deltaColor = maxColorComponent - minColorComponent;
  let hue = 0;
  if (deltaColor !== 0) {
    if (maxColorComponent === redNormalized) {
      hue = 60 * (((greenNormalized - blueNormalized) / deltaColor) % 6);
    } else if (maxColorComponent === greenNormalized) {
      hue = 60 * (((blueNormalized - redNormalized) / deltaColor) + 2);
    } else if (maxColorComponent === blueNormalized) {
      hue = 60 * (((redNormalized - greenNormalized) / deltaColor) + 4);
    }
  }
  if (hue < 0) {
    hue = hue + 360;
  }
  let saturation = 0;
  if (maxColorComponent !== 0) {
    saturation = deltaColor / maxColorComponent;
  }
  return { hue: hue, saturation: saturation, value: maxColorComponent };
}

//// shared search box ////

const SEARCH_GLASS_ICON = 'fa-search';
const SEARCH_CLEAR_ICON = 'fa-times';

// The icon follows the box's content: a magnifier while empty, else a cross that clears it.
function setSearchIcon(icon, value) {
  const clear = value !== "";
  icon.classList.toggle(SEARCH_CLEAR_ICON, clear);
  icon.classList.toggle(SEARCH_GLASS_ICON, !clear);
  icon.classList.toggle('search-row__icon--clear', clear);
}

// what was typed, ready to match against
export function searchTerm(value) {
  return String(value).trim().toLowerCase();
}

// one name against a searchTerm(); an empty term matches everything
export function searchMatches(text, term) {
  if (term === "") {
    return true;
  }
  return String(text).toLowerCase().indexOf(term) !== -1;
}

// Builds the search row (styled in lists.css). options:
//   tag        element to build the row from ('div', or 'a' in a menu)
//   id, boxId  ids for the row and box
//   rowClass   extra class on the row alone
//   partClass  class on the row and both parts (the menu's "nohide")
//   maxLength  longest term the box takes
//   value      what the box opens with
//   onSearch   called with the box's value on every change, clearing included
export function searchRowElement(options) {
  const row = document.createElement(options.tag || 'div');
  if (options.id) {
    row.id = options.id;
  }
  row.className = 'search-row';
  const icon = document.createElement('i');
  icon.className = `fad ${SEARCH_GLASS_ICON} search-row__icon`;
  const box = document.createElement('input');
  box.type = "text";
  box.className = 'search-row__box';
  if (options.boxId) {
    box.id = options.boxId;
  }
  if (options.rowClass) {
    row.className += ` ${options.rowClass}`;
  }
  if (options.partClass) {
    // on the icon too, or clearing a search would close the menu it is in
    row.className += ` ${options.partClass}`;
    icon.className += ` ${options.partClass}`;
    box.className += ` ${options.partClass}`;
  }
  box.placeholder = 'Search here...';
  box.setAttribute("autocorrect", "off");
  box.setAttribute("autocapitalize", "none");
  box.setAttribute("spellcheck", "false");
  if (options.maxLength) {
    box.maxLength = options.maxLength;
  }
  box.value = options.value ? String(options.value) : "";
  const report = () => {
    setSearchIcon(icon, box.value);
    options.onSearch(box.value);
  };
  const clear = () => {
    if (box.value === "") {
      return false; // a magnifier does nothing
    }
    box.value = "";
    report();
    return true;
  };
  // the press must not blur the box, or the keyboard drops away under a clear
  icon.addEventListener('pointerdown', (event) => event.preventDefault());
  icon.addEventListener('click', () => {
    if (clear()) {
      box.focus();
    }
  });
  box.addEventListener('input', report);
  box.addEventListener('keydown', (key_event) => {
    if (key_event.key === 'Escape') {
      key_event.preventDefault();
      clear();
      return;
    }
    if (key_event.key === 'Enter') {
      // rows filter as typed, so this only puts a keyboard away
      key_event.preventDefault();
      box.blur();
    }
  });
  row.appendChild(icon);
  row.appendChild(box);
  setSearchIcon(icon, box.value);
  return row;
}

//// drag to re-order ////

const DRAG_GRIP_CLASS = 'drag-grip';
const DRAG_GRIP_OFF_CLASS = 'drag-grip--off';

// grip handle for a draggable row; a fixed row's grip cannot drag
export function dragGripElement(fixed = false) {
  let className = DRAG_GRIP_CLASS;
  if (fixed === true) {
    className += ` ${DRAG_GRIP_OFF_CLASS}`;
  }
  return makeElement('span', className, '<i class="fad fa-grip-lines"></i>');
}

// Pointer drag re-ordering: the row moves in the DOM while dragged, and the caller
// reads the order back when onDrop fires. options: rowClass, onDrop
export class DragSort {

  // no drag until start() takes one
  constructor(options) {
    this.rowClass = options.rowClass;
    this.onDrop = options.onDrop;
    this.drag = null;
  }

  // starts a drag from a pointerdown on a grip; true when it did
  start(event) {
    const grip = event.target.closest(`.${DRAG_GRIP_CLASS}`);
    if (!grip || grip.classList.contains(DRAG_GRIP_OFF_CLASS)) {
      return false;
    }
    const row = grip.closest(`.${this.rowClass}`);
    if (!row) {
      return false;
    }
    event.preventDefault();
    this.drag = {
      row: row,
      grip: grip,
      pointer: event.pointerId,
      startY: event.clientY,
      offset: 0
    };
    row.classList.add("drag-row--active");
    grip.setPointerCapture(event.pointerId);
    return true;
  }

  // the next or previous row the dragged row may swap with, or null
  neighbour(row, forward) {
    let next = row.previousElementSibling;
    if (forward === true) {
      next = row.nextElementSibling;
    }
    if (!next || !next.classList.contains(this.rowClass)) {
      return null;
    }
    // rows that cannot be dragged hold their place
    if (next.querySelector(`.${DRAG_GRIP_OFF_CLASS}`)) {
      return null;
    }
    return next;
  }

  // follows the pointer, swapping past a neighbour's mid-point
  move(event) {
    const drag = this.drag;
    if (!drag || event.pointerId !== drag.pointer) {
      return;
    }
    event.preventDefault();
    drag.offset = event.clientY - drag.startY;
    let stepped = true;
    while (stepped) {
      const forward = drag.offset > 0;
      const other = this.neighbour(drag.row, forward);
      const reach = other ? other.offsetHeight : 0;
      stepped = other !== null && reach > 0 && Math.abs(drag.offset) > (reach / 2);
      if (stepped) {
        if (forward) {
          drag.row.parentNode.insertBefore(other, drag.row);
          drag.startY += reach;
          drag.offset -= reach;
        } else {
          drag.row.parentNode.insertBefore(drag.row, other);
          drag.startY -= reach;
          drag.offset += reach;
        }
      }
    }
    drag.row.style.transform = `translateY(${drag.offset}px)`;
  }

  // ends the drag and reports the drop
  end(event) {
    const drag = this.drag;
    if (!drag || (event && event.pointerId !== drag.pointer)) {
      return;
    }
    this.cancel();
    this.onDrop();
  }

  // drops the drag without reporting it
  cancel() {
    const drag = this.drag;
    if (!drag) {
      return;
    }
    drag.row.style.transform = "";
    drag.row.classList.remove("drag-row--active");
    if (drag.grip.hasPointerCapture(drag.pointer)) {
      drag.grip.releasePointerCapture(drag.pointer);
    }
    this.drag = null;
  }

  // is a row being dragged
  isDragging() {
    return this.drag !== null;
  }
}

//// shared edit lock ////

// the leading row older data files still carry, dropped when parsed
export const LOCK_TAG = 'LOCK';

const LOCK_EXPIRE_MS = 75000; // a lock left behind is ignored after this
const LOCK_BEAT_MS = 15000;   // the owner re-stamps this often
const LOCK_SETTLE_MS = 600;   // lets a competing claim land before checking

// One editor at a time for a file several browsers open. The lock file holds one row,
// OWNER_TOKEN|TIMESTAMP; a blank token or an expired stamp means free.
// new FileLock({ file, onLost })

export class FileLock {

  // options: file (the lock file), onLost()
  constructor(options) {
    this.file = options.file;
    this.onLost = options.onLost;
    this.owned = false;
    this.beatTimer = null;
    this.confirming = null;
    this.writing = null; // lock row on the wire, so writes cannot overtake
    // identifies this window in the lock row
    this.token = Date.now().toString(36) + Math.random().toString(36).substring(2, 8);
  }

  // the whole file: one row, held by this window or free
  line(held) {
    if (held === true) {
      return `${this.token}|${Date.now()}\n`;
    }
    return "|0\n";
  }

  // lock file text -> { token, stamp }; anything unreadable counts as free
  parse(text) {
    const free = { token: "", stamp: 0 };
    if (typeof text !== 'string' || text === "") {
      return free;
    }
    const rows = fileRows(text);
    if (rows.length === 0) {
      return free;
    }
    const cols = rows[0].split('|');
    if (cols.length < 2 || !isNumeric(cols[1])) {
      return free;
    }
    return { token: cols[0], stamp: Number(cols[1]) };
  }

  // is someone else editing, recently enough that they have not gone away
  busy(text) {
    if (text === null) {
      return false; // nothing to go on; claim() refuses on its own
    }
    const row = this.parse(text);
    if (row.token === "" || row.token === this.token) {
      return false;
    }
    return (Date.now() - row.stamp) < LOCK_EXPIRE_MS;
  }

  // the lock file text, or null
  async read() {
    return await readFile(this.file);
  }

  // One write at a time, in order: a retried re-stamp landing after the free row
  // would leave the lock held by a window that has gone.
  write(text) {
    const previous = this.writing;
    return trackPromise(this, 'writing', this.runWrite(previous, text));
  }

  // waits for the previous write, then writes
  async runWrite(previous, text) {
    await awaitQuiet(previous);
    return await writeFile(this.file, text);
  }

  // Takes the lock, given the lock file's text. False when another window holds it, or on a failure.
  async claim(lockText) {
    if (lockText === null) {
      return false; // unreadable, so do not assume it is free
    }
    if (this.busy(lockText)) {
      return false;
    }
    this.owned = true;
    const sent = await this.write(this.line(true));
    if (sent !== true) {
      this.owned = false;
      return false;
    }
    this.startBeat();
    return true;
  }

  // Reads the row back after the settle pause: of two windows that claimed together,
  // the one whose token is not in the file stands down. Runs in the background;
  // settle() holds later writes until it lands.
  confirm() {
    return trackPromise(this, 'confirming', this.runConfirm());
  }

  // the confirm itself; true unless the lock was lost
  async runConfirm() {
    await sleep(LOCK_SETTLE_MS);
    if (this.owned !== true) {
      return true; // released or abandoned during the pause
    }
    const text = await this.read();
    if (text === null) {
      return true; // nothing to go on, so the claim stands
    }
    if (this.parse(text).token === this.token) {
      return true;
    }
    this.stopBeat();
    this.owned = false;
    this.onLost();
    return false;
  }

  // waits for a running confirm, so nothing is written while ownership is unknown
  async settle() {
    await awaitQuiet(this.confirming);
  }

  // starts re-stamping
  startBeat() {
    this.stopBeat();
    this.beatTimer = setInterval(() => this.refresh(), LOCK_BEAT_MS);
  }

  // stops re-stamping
  stopBeat() {
    if (this.beatTimer !== null) {
      clearInterval(this.beatTimer);
      this.beatTimer = null;
    }
  }

  // re-stamps the lock so it does not expire under an open window
  async refresh() {
    if (this.owned !== true) {
      return true;
    }
    return await this.write(this.line(true));
  }

  // writes the free row, after any confirm has settled
  async release() {
    this.stopBeat();
    await this.settle();
    if (this.owned !== true) {
      return true;
    }
    this.owned = false;
    return await this.write(this.line(false));
  }

  // drops the claim without writing anything
  abandon() {
    this.stopBeat();
    this.owned = false;
  }
}
