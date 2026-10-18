// Automate Website - shared utilities
//
// Everything more than one module needs: API calls, the file read/write
// pair, DOM helpers, the delegated button listeners, the button templates
// the room grids share, colour maths, the search row, drag re-ordering and
// the edit lock. The volume controls are in volctrl.js and the browser-side
// encryption in encrypt.js, and the message popup and error reports in
// error.js.

import { HttpError, reportError, reportNotConnected, logError,
  describeCommand } from './error.js';

const REFRESH_INTERVAL = 10; // seconds, room status refresh
const TIMEOUT_INTERVAL = 50; // seconds, inactivity before services pause

export function refreshInterval() {
  return REFRESH_INTERVAL;
}

export function timeoutInterval() {
  return TIMEOUT_INTERVAL;
}

//// API calls ////

// Query values are pasted straight into the URL. Only characters that would
// end a value or read as an escape are encoded; everything the site sends
// (letters, digits, _ - ~ . @) goes out byte for byte.
const QUERY_UNSAFE = /[%&#+=?\s]/g;

function encodeArg(value) {
  const text = (value === null || value === undefined) ? '' : String(value);
  return text.replace(QUERY_UNSAFE, (ch) =>
    '%' + ch.charCodeAt(0).toString(16).toUpperCase().padStart(2, '0'));
}

// Sends one command to the device API and returns the parsed JSON answer, or
// null when the request failed, which is reported in the error popup. The
// body is read inside the same try, so a connection that drops part way
// through an answer is reported as lost too. options.quiet logs a failure to
// the console only, for a caller that shows failures itself.
// sendCmd('main', 'brxmit', 'brtvon')
// sendCmd('main', 'lrstatus', '', { quiet: true })
export async function sendCmd(act, arg1, arg2 = '', options = {}) {
  const url = location.protocol + "//" + location.hostname + "/api?var=" +
    encodeArg(arg2) + "&arg=" + encodeArg(arg1) + "&action=" + encodeArg(act);
  try {
    const response = await fetch(url, { method: 'GET' });
    const answerText = await response.text();
    if (!response.ok) {
      throw new HttpError(response.status, answerText);
    }
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

// Reads a menu / list file at a full URL, as text; undefined when it could
// not be read, which is reported in the error popup.
// loadMenu(fileURL('read', 'ledeffectmenu'))
export async function loadMenu(url) {
  const fileName = fileNameOf(url);
  try {
    const response = await fetch(url, { method: "GET" });
    const answerText = await response.text();
    if (!response.ok) {
      throw new HttpError(response.status, answerText);
    }
    return answerText;
  } catch (error) {
    reportError(`reading the ${fileName} menu`, error);
    return undefined;
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

// A file request lost on the way is sent again after a short pause: a
// keep-alive connection the far end had already closed fails the request
// without it ever reaching the API (Safari reports "Load failed"), and the
// proxy answers 502-504 while it cannot reach the API for a moment. Only
// those are retried -- an answer from the API itself stands. Every write
// replaces the whole file, so a write sent twice leaves the same file.
const FILE_RETRY_MS = [250, 750];      // pause before each further attempt
const FILE_RETRY_STATUS = [502, 503, 504];

// The last file request that failed, so the caller that gave up on it can say
// why: { file, status, text, attempts, time }, status null when nothing
// answered. Kept until the next failure; reportFileFailure() reads it while
// it is fresh.
const FILE_FAILURE_MAX_AGE_MS = 10000;
let lastFileFailure = null;

// { ok, status, text } once the server has answered, or null when it never
// did. The body is read inside the same try, so a connection that drops
// part way through an answer counts as lost too. A failure is kept in
// lastFileFailure.
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

// Reports why a file read or write just failed, in the error popup: Not
// connected when nothing answered (or only the proxy did), an exception with
// the server's answer otherwise. what says what the page was doing.
// reportFileFailure('saving the bookmarks')
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

// true only when the server confirms the write; a failed attempt leaves the
// file on the server exactly as it was
export async function writeFile(file, text) {
  const answer = await fileRequest(fileURL('write', file), {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: text
  });
  return answer !== null && answer.ok === true;
}

// Writes a file and calls onSuccess(file) once the server takes it, reporting
// a failure in the error popup. Fire and forget, for callers with nothing to
// await. savePOST('pwd', ['secret'], afterPOSTActions)
export function savePOST(file, data, onSuccess) {
  writeFile(file, String(data)).then((sent) => {
    if (sent !== true) {
      reportFileFailure('saving the file');
      return;
    }
    if (typeof onSuccess === 'function') {
      onSuccess(file);
    }
  });
}

// Hands a file to the browser to send on its own. A page being torn down is
// exactly when the browser cancels requests still in flight; a beacon is
// finished off without the page. There is no answer to read, so this reports
// only whether the browser accepted it.
export function sendFileBeacon(file, text) {
  if (typeof navigator === 'undefined') {
    return false;
  }
  if (typeof navigator.sendBeacon !== 'function') {
    return false;
  }
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

// true for a string holding a whole number ("" and " " are not numeric)
export function isNumeric(value) {
  return typeof value === 'string' && value.trim() !== '' &&
    Number.isInteger(Number(value));
}

// A palette file: one colour per line, `hex_value|display_name`. Shared by
// the theme menu and the colour prompt's default swatches. A line with no
// colour is dropped, and a value written without its "#" still reads.
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

// one dynamic menu row: menuRow('cmd~lrxmit~usb', 'grnind', 'USB')
export function menuRow(col0, col1, col2) {
  return `${col0}|${col1}|${col2}\n`;
}

// the indicator column for a row that is either on or off
export function indicator(on) {
  if (on === true) {
    return 'grnind';
  }
  return 'blkind';
}

//// base64 ////

export function base64URLSafeEncode(buffer) {
  return btoa(buffer)
    .replace(/\+/g, '-')  // '+' -> '-'
    .replace(/\//g, '_')  // '/' -> '_'
    .replace(/\=/g, '@'); // '=' -> '@'
}

// URL-safe base64 back to the standard alphabet, which is what an
// <img src="data:..."> expects. Swaps characters only, does not decode.
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

// Tag function for DOM markup. Strips whitespace between tags so the result
// holds no stray text nodes (they would render as gaps between inline-block
// elements). html`<div id="${id}"></div>`
export function html(strings, ...values) {
  return strings
    .reduce((out, str, i) => out + str + (i < values.length ? values[i] : ""), "")
    .replace(/>\s+</g, "><")
    .trim();
}

// new element with its class and, optionally, its markup already set
export function makeElement(tag, className = '', innerHTML = '') {
  const elem = document.createElement(tag);
  elem.className = className;
  if (innerHTML) {
    elem.innerHTML = innerHTML;
  }
  return elem;
}

// new SVG element in the SVG namespace: svgElement('circle', { cx: '25' })
export function svgElement(tag, attributes = {}) {
  const elem = document.createElementNS("http://www.w3.org/2000/svg", tag);
  Object.keys(attributes).forEach(name => {
    elem.setAttribute(name, attributes[name]);
  });
  return elem;
}

// display style of every element carrying a class
export function classDisplay(_elem, _state) {
  const _class = document.getElementsByClassName(_elem);
  for (let _itr = 0; _itr < _class.length; _itr++) {
    _class[_itr].style.display = _state;
  }
}

// add / remove a class on an element by ID: addClassToElem('add', 'id', 'cls')
export function addClassToElem(_action, _elmid, _class) {
  const _elm = document.getElementById(_elmid);
  if (_action === 'add') {
    _elm.classList.add(_class);
  }
  if (_action === 'remove') {
    _elm.classList.remove(_class);
  }
}

// display style of one element by ID, when it is on the page
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

// remove an element by ID
export function removeWindow(id) {
  const win = document.getElementById(id);
  if (win) {
    document.body.removeChild(win);
  }
}

// Draws a centred prompt window and adds it to the page.
// promptWindow('bed__prompt', '<div class="bed__title">Bed</div>')
export function promptWindow(id, markup, className = 'prompt_win') {
  const win = document.createElement("div");
  win.id = id;
  win.className = className;
  win.innerHTML = markup;
  document.body.appendChild(win);
  return win;
}

// Calls handler(button) for every button press inside a window. The listener
// goes away with the window, so nothing has to unbind it.
export function onButtons(win, handler) {
  win.addEventListener("click", (click_event) => {
    const button = click_event.target.closest('button');
    if (button) {
      handler(button);
    }
  });
}

// Swaps a button's label for its spinner while a request is in flight.
// setSpinner('hfinmenu-text', 'hfinmenu-spinner', true)
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
    text.style.visibility = 'visible';
    spinner.classList.remove(spinnerClass);
  }
}

//// button templates ////
//
// The markup every button repeats: the room grids, the Pixels and APT
// groups, the text scroller, the Server window's option panels and the
// windows' own buttons. They are built from these two rather than writing the same button out once per
// device, so a change to how a button is drawn is made in one place.

// The inside of a button: its icon when it has one, then its label when it
// has one. The icon carries an id when something changes it later.
// buttonLabelMarkup('fad fa-lamp-floor', 'Lamp')
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

// The classes a button carries: "button", then any of its own.
// buttonClassNames('svropt_button') -> "button svropt_button"
function buttonClassNames(extraClass) {
  if (extraClass) {
    return `button ${extraClass}`;
  }
  return 'button';
}

// One plain button, which opens no menu of its own: a button of a button
// grid, or one of a window's own buttons.
// config: buttonId, and optionally
//   icon         the icon's classes (a text-only button has none)
//   iconId       an id for the icon, when something changes it later
//   label        the text after the icon (an icon-only button has none)
//   buttonClass  the classes the button carries besides "button"
export function gridButtonTemplate(config) {
  const labelMarkup = buttonLabelMarkup(config.icon, config.label, config.iconId);
  const buttonClass = buttonClassNames(config.buttonClass);
  return `
  <button type="button" class="${buttonClass}" id="${config.buttonId}">
    <span class="button__text">${labelMarkup}</span>
  </button>`;
}

// One button of a button grid together with the dropdown that opens under it.
// config: buttonId, menuId, and optionally
//   icon, label         as gridButtonTemplate() takes them
//   textId + spinnerId  the label and the spinner that takes its place while
//                       the device is being asked for its state
//   buttonClass         a class the button carries besides "button"
//   menuClass           "ind-button" (the default) or "dd-button"
//   menuContent         rows the menu holds from the start
export function dropdownButtonTemplate(config) {
  const labelMarkup = buttonLabelMarkup(config.icon, config.label);
  const buttonClass = buttonClassNames(config.buttonClass);
  let menuClass = 'ind-button';
  if (config.menuClass) {
    menuClass = config.menuClass;
  }
  let menuContent = '';
  if (config.menuContent) {
    menuContent = config.menuContent;
  }
  // a button with no spinner has a label with no id, as nothing swaps it out
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
//
// Each template registers its own button handlers with registerListeners()
// directly beneath its markup; the groups merge into this one map, which the
// page's single click listener looks up through dispatchRegisteredListener().
// Delegation means handlers keep working for markup rendered later (room
// grids, bookmark buttons, svropt panels) and never need re-attaching after
// a re-render. The map lives here, below every module that registers into
// it, so a room module can register while it loads without importing main.js.
const DELEGATED_LISTENERS = {};

// Adds a group of { elementId: handler } pairs. The first handler registered
// for an id is the one that stays.
export function registerListeners(listeners) {
  for (const [elementId, handler] of Object.entries(listeners)) {
    if (!(elementId in DELEGATED_LISTENERS)) {
      DELEGATED_LISTENERS[elementId] = handler;
    }
  }
}

// Walks up from the clicked element to the first id with a registered handler
// and runs it. Answers true when a handler ran.
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

export function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

// Rate limiter: calls func at most once per limit ms, with the last call of a
// burst sent on the trailing edge. throttled.cancel() drops a trailing call
// that has not fired yet, so a value cannot arrive after the window it came
// from has been torn down.
export function requestThrottle(func, limit) {
  let lastFunc;
  let lastRan;
  // a declaration, not an arrow: `this` and `arguments` reach the wrapped
  // call, and cancel() hangs off the name
  function throttled() {
    const context = this;
    const args = arguments;
    if (!lastRan) {
      func.apply(context, args);
      lastRan = Date.now();
    } else {
      clearTimeout(lastFunc);
      lastFunc = setTimeout(function() {
        if ((Date.now() - lastRan) >= limit) {
          func.apply(context, args);
          lastRan = Date.now();
        }
      }, limit - (Date.now() - lastRan));
    }
  }
  throttled.cancel = function() {
    clearTimeout(lastFunc);
    lastFunc = undefined;
  };
  return throttled;
}

//// in-flight promises ////
//
// One operation at a time on a shared resource. The promise is held on the
// owner under `key` and cleared when it settles, so a second caller can see
// that one is already running.

// Holds a promise that has just been started. Always starts a new one.
export function trackPromise(owner, key, running) {
  owner[key] = running;
  // .finally() hands back a promise of its own, and a rejection travels down
  // it: without this catch it surfaced as an unhandled rejection even where
  // the caller handles the failure itself
  running.finally(() => {
    if (owner[key] === running) {
      owner[key] = null;
    }
  }).catch(() => {});
  return running;
}

// Returns the promise already in flight, or starts one with factory().
export function singleFlight(owner, key, factory) {
  if (owner[key]) {
    return owner[key];
  }
  return trackPromise(owner, key, factory());
}

// Waits for a tracked promise to settle, however it settles. Whoever started
// it reports its failure.
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

// The six faces of the hue wheel, in 60 degree steps. Each entry names how
// that segment builds its raw colour from the chroma, the ramp between the
// two strongest channels, and zero.
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
  // wrap into [0, 360) so hue === 360 (slider at max) maps back to red
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
//
// The one search row, used by the bookmarks drop-down and the lists window.
// The row is an icon and a box: a magnifier while the box is empty, a cross
// that clears it once there is anything in it. Everything the host does with
// what is typed goes through one onSearch callback, clearing included. The
// look is one block of CSS, in styles/lists.css.

export const SEARCH_ROW_CLASS = 'search-row';
export const SEARCH_ICON_CLASS = 'search-row__icon';
export const SEARCH_CLEAR_CLASS = 'search-row__icon--clear';
export const SEARCH_BOX_CLASS = 'search-row__box';
export const SEARCH_GLASS_ICON = 'fa-search';
export const SEARCH_CLEAR_ICON = 'fa-times';
export const SEARCH_PLACEHOLDER = 'Search here...';

// The icon follows what is in the box rather than what is being matched on: a
// box holding only spaces filters nothing, but there is still something in it
// to clear.
function setSearchIcon(icon, value) {
  const clear = value !== "";
  icon.classList.toggle(SEARCH_CLEAR_ICON, clear);
  icon.classList.toggle(SEARCH_GLASS_ICON, !clear);
  icon.classList.toggle(SEARCH_CLEAR_CLASS, clear);
}

// what was typed, ready to match against
export function searchTerm(value) {
  if (value === null || value === undefined) {
    return "";
  }
  return String(value).trim().toLowerCase();
}

// one name against a term from searchTerm(); an empty term matches everything
export function searchMatches(text, term) {
  if (term === "") {
    return true;
  }
  let subject = "";
  if (text !== null && text !== undefined) {
    subject = String(text);
  }
  return subject.toLowerCase().indexOf(term) !== -1;
}

// Builds the search row.
//   tag          element to build the row from ('div', or 'a' in a menu)
//   id           id for the row, if the host needs to find it again
//   rowClass     extra class on the row alone, for the host's own chrome
//   partClass    class every part carries -- the bookmarks menu's "nohide"
//   boxId        id for the box, if the host needs to find it again
//   placeholder  what the empty box says
//   maxLength    longest term the box will take
//   value        what the box opens with, so a re-drawn row keeps its search
//   onSearch     called with the box's value on every change, clearing included
export function searchRowElement(options = {}) {
  const row = document.createElement(options.tag || 'div');
  if (options.id) {
    row.id = options.id;
  }
  row.className = SEARCH_ROW_CLASS;
  const icon = document.createElement('i');
  icon.className = `fad ${SEARCH_GLASS_ICON} ${SEARCH_ICON_CLASS}`;
  const box = document.createElement('input');
  box.type = "text";
  box.className = SEARCH_BOX_CLASS;
  if (options.boxId) {
    box.id = options.boxId;
  }
  if (options.rowClass) {
    row.className += ` ${options.rowClass}`;
  }
  if (options.partClass) {
    // on the row and both of its parts: a menu that decides what closes it by
    // looking at what was pressed has to be told about the icon as well, or
    // clearing a search would shut the menu it was clearing
    row.className += ` ${options.partClass}`;
    icon.className += ` ${options.partClass}`;
    box.className += ` ${options.partClass}`;
  }
  box.placeholder = options.placeholder || SEARCH_PLACEHOLDER;
  box.setAttribute("autocorrect", "off");
  box.setAttribute("autocapitalize", "none");
  box.setAttribute("spellcheck", "false");
  if (options.maxLength) {
    box.maxLength = options.maxLength;
  }
  box.value = options.value ? String(options.value) : "";
  const report = () => {
    setSearchIcon(icon, box.value);
    if (typeof options.onSearch === 'function') {
      options.onSearch(box.value);
    }
  };
  const clear = () => {
    if (box.value === "") {
      return false; // still a magnifier, and a magnifier does nothing
    }
    box.value = "";
    report();
    return true;
  };
  // the press must not take the focus off the box beside it, so the keyboard
  // does not drop away underneath a clear
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
      // rows are filtered as it is typed, so this only puts a keyboard away
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

export const DRAG_GRIP_CLASS = 'drag-grip';
export const DRAG_GRIP_OFF_CLASS = 'drag-grip--off';
export const DRAG_GRIP_HTML = '<i class="fad fa-grip-lines"></i>';

// grip handle for a draggable row; a fixed row gets a grip that cannot drag
export function dragGripElement(fixed = false) {
  let className = DRAG_GRIP_CLASS;
  if (fixed === true) {
    className += ` ${DRAG_GRIP_OFF_CLASS}`;
  }
  return makeElement('span', className, DRAG_GRIP_HTML);
}

// Pointer drag re-ordering for a list of rows. The row is moved in the DOM
// while it is dragged, so the caller only has to read the new order back out
// of the container when onDrop fires.
//   rowClass  class every draggable row carries
//   onDrop    called once the row is released
export class DragSort {

  constructor(options = {}) {
    this.rowClass = options.rowClass || 'drag-row';
    this.onDrop = function () {};
    if (typeof options.onDrop === 'function') {
      this.onDrop = options.onDrop;
    }
    this.drag = null;
  }

  // true when the event started a drag
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
    if (grip.setPointerCapture) {
      grip.setPointerCapture(event.pointerId);
    }
    return true;
  }

  // next / previous row that is allowed to swap with the dragged row
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

  move(event) {
    const drag = this.drag;
    if (!drag || event.pointerId !== drag.pointer) {
      return;
    }
    event.preventDefault();
    drag.offset = event.clientY - drag.startY;
    // step over a neighbour once the row has travelled past its mid-point,
    // then re-base the offset so the row stays under the pointer
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

  end(event) {
    const drag = this.drag;
    if (!drag || (event && event.pointerId !== drag.pointer)) {
      return;
    }
    this.cancel();
    this.onDrop();
  }

  // drops the drag without reporting it (used when tearing down)
  cancel() {
    const drag = this.drag;
    if (!drag) {
      return;
    }
    drag.row.style.transform = "";
    drag.row.classList.remove("drag-row--active");
    if (drag.grip.hasPointerCapture && drag.grip.hasPointerCapture(drag.pointer)) {
      drag.grip.releasePointerCapture(drag.pointer);
    }
    this.drag = null;
  }

  isDragging() {
    return this.drag !== null;
  }
}

//// shared edit lock ////
//
// One editor at a time for a file several browsers can open. The lock lives
// in a small file of its own beside the data it guards -- lists-lock.save,
// bookmarks-lock.save -- holding a single row:
//   OWNER_TOKEN|TIMESTAMP
// A blank token, or a timestamp older than the expiry, means it is free.
//
// Keeping it out of the data file is what makes re-stamping cheap: a one row
// write to a tiny file, so it can run often without pushing the whole menu
// back over the wire. Older data files carry a leading LOCK| row from when
// the two shared a file; both menus drop that row when they parse.

// still exported so the menus can recognise the old leading LOCK| row
export const LOCK_TAG = 'LOCK';

export const LOCK_EXPIRE_MS = 75000; // a lock left behind is ignored after this
export const LOCK_BEAT_MS = 15000;   // owner re-stamps the lock this often
export const LOCK_SETTLE_MS = 600;   // let a competing claim land before checking

export class FileLock {

  // options: file (the lock file), onLost(), onExchange(ok)
  constructor(options = {}) {
    this.file = options.file;
    this.onLost = function () {};
    if (typeof options.onLost === 'function') {
      this.onLost = options.onLost;
    }
    // called with true / false after every exchange, for a caller showing a
    // connection state of its own
    this.onExchange = function () {};
    if (typeof options.onExchange === 'function') {
      this.onExchange = options.onExchange;
    }
    this.owned = false;
    this.beatTimer = null;
    this.confirming = null;
    this.writing = null; // lock row on the wire, so writes cannot overtake
    // identifies this window in the lock row (ASCII, no separators)
    this.token = Date.now().toString(36) + Math.random().toString(36).substring(2, 8);
  }

  //// the row ////

  // the whole file: one row, OWNER_TOKEN|TIMESTAMP
  line(held) {
    if (held === true) {
      return `${this.token}|${Date.now()}\n`;
    }
    return "|0\n";
  }

  // lock file text -> { token, stamp }. Anything unreadable counts as free,
  // which is the same answer a missing file gives.
  parse(text) {
    const free = { token: "", stamp: 0 };
    if (typeof text !== 'string' || text === "") {
      return free;
    }
    const rows = splitMenuData(text)
      .map(row => cleanString(row))
      .filter(row => row);
    if (rows.length === 0) {
      return free;
    }
    const cols = rows[0].split('|');
    if (cols.length < 2 || !isNumeric(cols[1])) {
      return free;
    }
    return { token: cols[0], stamp: Number(cols[1]) };
  }

  // someone else is editing, recently enough that they have not gone away
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

  //// file ////

  async read() {
    const text = await readFile(this.file);
    this.onExchange(text !== null);
    return text;
  }

  // One write at a time, in the order they were asked for. A re-stamp still
  // being retried when the lock is handed back would otherwise land after
  // the free row and leave the lock held by a window that has gone.
  write(text) {
    const previous = this.writing;
    return trackPromise(this, 'writing', this.runWrite(previous, text));
  }

  async runWrite(previous, text) {
    await awaitQuiet(previous);
    const sent = await writeFile(this.file, text);
    this.onExchange(sent);
    return sent;
  }

  //// claiming ////

  // Takes the lock. Pass the lock file text if it has already been read --
  // opening a menu reads the data and the lock together -- or leave it out
  // and this reads it. False when another window holds it, or on a failure.
  async claim(text) {
    let lockText = text;
    if (lockText === undefined) {
      lockText = await this.read();
    }
    if (lockText === null) {
      return false; // the lock file could not be read, so do not assume it
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

  // Reads the row back after the settle pause. Two windows that claimed
  // together both land here, and the one whose token is not in the file
  // stands down. Left running in the background so the caller draws without
  // waiting; settle() holds any later write until it lands.
  confirm() {
    return trackPromise(this, 'confirming', this.runConfirm());
  }

  async runConfirm() {
    await sleep(LOCK_SETTLE_MS);
    if (this.owned !== true) {
      return true; // released or abandoned while the pause ran
    }
    const text = await this.read();
    if (text === null) {
      return true; // nothing to go on, so leave the claim standing
    }
    if (this.parse(text).token === this.token) {
      return true;
    }
    this.stopBeat();
    this.owned = false;
    this.onLost();
    return false;
  }

  // waits for a confirm that is still running, so nothing is written while it
  // is unknown whether this window holds the lock
  async settle() {
    await awaitQuiet(this.confirming);
  }

  //// keeping and handing back ////

  startBeat() {
    this.stopBeat();
    this.beatTimer = setInterval(() => this.refresh(), LOCK_BEAT_MS);
  }

  stopBeat() {
    if (this.beatTimer !== null) {
      clearInterval(this.beatTimer);
      this.beatTimer = null;
    }
  }

  // Re-stamps the timestamp so the lock does not expire under an open window.
  // One row, to a file of its own -- the data is not involved.
  async refresh() {
    if (this.owned !== true) {
      return true;
    }
    return await this.write(this.line(true));
  }

  // Hands the lock back. write is false when the server is known to be
  // unreachable, and then the lock is left to expire instead.
  async release(write) {
    this.stopBeat();
    await this.settle();
    if (this.owned !== true) {
      return true;
    }
    this.owned = false;
    if (write !== true) {
      return true;
    }
    return await this.write(this.line(false));
  }

  // drops the claim without writing anything
  abandon() {
    this.stopBeat();
    this.owned = false;
  }
}
