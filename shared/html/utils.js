// Automate Website - shared utilities
//
// Everything more than one module needs: API calls, the file read/write
// pair, DOM helpers, colour maths, the search row, drag re-ordering, the
// edit lock and the browser-side encryption.

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
// null when the request failed. sendCmd('main', 'brxmit', 'brtvon')
export async function sendCmd(act, arg1, arg2 = '') {
  const url = location.protocol + "//" + location.hostname + "/api?var=" +
    encodeArg(arg2) + "&arg=" + encodeArg(arg1) + "&action=" + encodeArg(act);
  try {
    const response = await fetch(url, { method: 'GET' });
    return await response.json();
  } catch (error) {
    showPopup("[sendCmd] " + error);
    return null;
  }
}

// reads a menu / list file at a full URL, as text
export async function loadMenu(url) {
  try {
    const response = await fetch(url, { method: "GET" });
    return await response.text();
  } catch (error) {
    showPopup("[loadMenu] failed to fetch: " + error);
  }
}

//// file transfer ////

// read / write URL for one file on the device: fileURL('read', 'lists.save')
export function fileURL(action, file) {
  return `${location.protocol}//${location.hostname}/api/${action}?file=${file}`;
}

// the file text, or null when the server cannot be reached
export async function readFile(file) {
  try {
    const response = await fetch(fileURL('read', file), { method: 'GET' });
    if (!response.ok) {
      throw new Error(`status ${response.status}`);
    }
    return await response.text();
  } catch (error) {
    return null;
  }
}

// true only when the server confirms the write; a failed attempt leaves the
// file on the server exactly as it was
export async function writeFile(file, text) {
  try {
    const response = await fetch(fileURL('write', file), {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: text
    });
    return response.ok === true;
  } catch (error) {
    return false;
  }
}

// Writes a file and calls onSuccess(file) once the server takes it, reporting
// a failure in a popup. Fire and forget, for callers with nothing to await.
// savePOST('pwd', ['secret'], afterPOSTActions)
export function savePOST(file, data, onSuccess) {
  writeFile(file, String(data)).then((sent) => {
    if (sent !== true) {
      showPopup("[savePOST] failed to send: " + file);
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

//// message popup ////

let messagePopupTimer = null;

// lets the page teardown drop a pending fade
export function clearPopupTimer() {
  if (messagePopupTimer) {
    clearTimeout(messagePopupTimer);
    messagePopupTimer = null;
  }
}

// shows a message in the corner for four seconds: showPopup("[lists] saved.")
export function showPopup(message) {
  const win_name = "msg-popup";
  const text_name = "msg-text";
  let msgpopwin = document.getElementById(win_name);
  if (!msgpopwin) {
    msgpopwin = document.createElement("div");
    msgpopwin.classList.add("popup-alert");
    msgpopwin.id = win_name;
    msgpopwin.innerHTML = html`<div id="${text_name}"></div>`;
    // Attached once, for the life of the window. A message arriving mid-fade
    // takes the class off again, which cancels the animation before it ends:
    // a {once:true} listener per fade would never fire and never unregister.
    msgpopwin.addEventListener("animationend", () => {
      if (msgpopwin.classList.contains("fade-out") && msgpopwin.isConnected) {
        msgpopwin.remove();
      }
    });
    document.body.appendChild(msgpopwin);
  }
  msgpopwin.classList.remove("fade-out");
  document.getElementById(text_name).innerHTML = message;
  clearPopupTimer();
  messagePopupTimer = setTimeout(() => {
    messagePopupTimer = null;
    // the listener above removes the window once the fade ends
    msgpopwin.classList.add("fade-out");
  }, 4000);
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

  async write(text) {
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

//// shared encryption ////
//
// The browser side of encrypted files. Nothing here trusts the server with
// anything it could decrypt with. One PBKDF2 pass produces 64 bytes:
//   bytes  0..31  the AES-256-GCM data key -- stays in this tab, never sent
//   bytes 32..63  the verifier, hashed into the password record
//
// A blob on a row looks like E1.<iv>.<ciphertext+tag> in the site's URL-safe
// base64, so it survives the pipe-delimited files as it stands.
//
// crypto.subtle is only there in a secure context (https, or localhost), so
// cryptoReady() is false over plain http and the caller has to say so rather
// than quietly storing something weaker.

export const CRYPT_TAG = 'E1';          // blob format, first field
export const CRYPT_ITERATIONS = 310000; // PBKDF2 rounds for a new password
export const CRYPT_SALT_BYTES = 16;
export const CRYPT_IV_BYTES = 12;       // 96 bits, what AES-GCM expects
export const CRYPT_KEY_BYTES = 32;      // AES-256
export const CRYPT_MIN_PASSWORD = 8;
export const CRYPT_MAX_PASSWORD = 256;
const CRYPT_BLOB_REGEX = /^E1\.[A-Za-z0-9\-_@]+\.[A-Za-z0-9\-_@]+$/;

// WebCrypto, or nothing -- there is no fallback and there should not be one
export function cryptoReady() {
  if (typeof crypto === 'undefined') {
    return false;
  }
  if (!crypto.subtle || typeof crypto.subtle.deriveBits !== 'function') {
    return false;
  }
  return true;
}

export function randomBytes(count) {
  const bytes = new Uint8Array(count);
  crypto.getRandomValues(bytes);
  return bytes;
}

// bytes -> the site's URL-safe base64
export function bytesToB64(bytes) {
  let binary = '';
  const view = new Uint8Array(bytes);
  // a chunk at a time: fromCharCode is given the array as arguments, and a
  // long one overruns the call stack
  for (let i = 0; i < view.length; i += 4096) {
    binary += String.fromCharCode.apply(null, view.subarray(i, i + 4096));
  }
  return base64URLSafeEncode(binary);
}

export function b64ToBytes(value) {
  const binary = atob(base64URLSafeToStandard(String(value)));
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

export function newCryptSalt() {
  return bytesToB64(randomBytes(CRYPT_SALT_BYTES));
}

// "" when the password will do, or the reason it will not
export function checkPassword(value) {
  let text = '';
  if (typeof value === 'string') {
    text = value;
  }
  if (text.length < CRYPT_MIN_PASSWORD) {
    return `at least ${CRYPT_MIN_PASSWORD} characters`;
  }
  if (text.length > CRYPT_MAX_PASSWORD) {
    return `at most ${CRYPT_MAX_PASSWORD} characters`;
  }
  return "";
}

// The one slow step. Both halves come out of a single PBKDF2 pass, so the
// verifier the server sees tells it nothing about the key it never sees.
// Returns { key, verifier }.
export async function deriveCryptKeys(password, salt, iterations) {
  const encoder = new TextEncoder();
  // NFC first: the same password typed on two devices can arrive as two
  // different byte strings otherwise, and derive two different keys
  const secret = encoder.encode(String(password).normalize('NFC'));
  const material = await crypto.subtle.importKey(
    'raw', secret, { name: 'PBKDF2' }, false, ['deriveBits']
  );
  const bits = await crypto.subtle.deriveBits({
    name: 'PBKDF2',
    salt: b64ToBytes(salt),
    iterations: Number(iterations),
    hash: 'SHA-256'
  }, material, CRYPT_KEY_BYTES * 8 * 2);
  const derived = new Uint8Array(bits);
  const keyBytes = derived.slice(0, CRYPT_KEY_BYTES);
  const verifier = derived.slice(CRYPT_KEY_BYTES);
  const key = await crypto.subtle.importKey(
    'raw', keyBytes, { name: 'AES-GCM' }, false, ['encrypt', 'decrypt']
  );
  // the raw bytes are wiped once the key object holds them; the CryptoKey is
  // not extractable, so nothing on the page can read them back out
  keyBytes.fill(0);
  derived.fill(0);
  return { key: key, verifier: bytesToB64(verifier) };
}

export function isCryptBlob(value) {
  return typeof value === 'string' && CRYPT_BLOB_REGEX.test(value);
}

// A label tying ciphertext to where it is kept, passed to AES-GCM as
// additional data: a blob moved to another list, or another file, fails to
// open rather than opening somewhere it does not belong.
export function cryptContext(file, id) {
  return `automate:${file}:${id}`;
}

export async function encryptText(key, text, context = '') {
  const iv = randomBytes(CRYPT_IV_BYTES);
  const params = { name: 'AES-GCM', iv: iv, tagLength: 128 };
  if (context !== '') {
    params.additionalData = new TextEncoder().encode(context);
  }
  const sealed = await crypto.subtle.encrypt(
    params, key, new TextEncoder().encode(String(text))
  );
  return `${CRYPT_TAG}.${bytesToB64(iv)}.${bytesToB64(new Uint8Array(sealed))}`;
}

// null for anything that will not open: the wrong key, a blob that has been
// meddled with, one that belongs to another list. GCM cannot tell those apart
// and neither should the caller.
export async function decryptText(key, blob, context = '') {
  if (!isCryptBlob(blob)) {
    return null;
  }
  const parts = String(blob).split('.');
  try {
    const params = { name: 'AES-GCM', iv: b64ToBytes(parts[1]), tagLength: 128 };
    if (context !== '') {
      params.additionalData = new TextEncoder().encode(context);
    }
    const opened = await crypto.subtle.decrypt(params, key, b64ToBytes(parts[2]));
    return new TextDecoder().decode(opened);
  } catch (error) {
    return null;
  }
}

//// password record ////
//
// A file of its own beside the data it guards (lists-pwd.save), one row per
// slot:
//   V1|SALT|ITERATIONS|CHECK
// SALT and ITERATIONS are what PBKDF2 needs to arrive back at the same key;
// neither is a secret. CHECK is SHA-256 of the verifier, so a password can be
// told right from wrong without the file holding anything that opens the
// data. A missing or blank file means no password has been set up.
//
// A password change writes a second row while the first still describes what
// the data is encrypted under, and promotes it once the re-encrypted file has
// landed -- so a change cut off part way leaves both, and unlock() tries each.

export const CRYPT_REC_TAG = 'V1';
const CRYPT_REC_COLS = 4;

// SHA-256 of the verifier. The verifier is already the far end of a slow KDF,
// so a fast hash here gives nothing away.
export async function hashVerifier(verifier) {
  const digest = await crypto.subtle.digest('SHA-256', b64ToBytes(verifier));
  return bytesToB64(new Uint8Array(digest));
}

// record text -> [{ salt, iterations, check }], oldest first. Anything that
// will not parse is dropped, and no rows reads as "not set up".
export function parseCryptRecord(text) {
  if (typeof text !== 'string' || text === "") {
    return [];
  }
  const slots = [];
  splitMenuData(text)
    .map(row => cleanString(row))
    .filter(row => row)
    .forEach(row => {
      const cols = row.split('|');
      if (cols.length < CRYPT_REC_COLS || cols[0] !== CRYPT_REC_TAG) {
        return;
      }
      if (!isNumeric(cols[2])) {
        return;
      }
      const iterations = Number(cols[2]);
      if (iterations < 1000 || cols[1] === "" || cols[3] === "") {
        return;
      }
      slots.push({ salt: cols[1], iterations: iterations, check: cols[3] });
    });
  return slots;
}

export function buildCryptRecord(slots) {
  let text = "";
  slots.forEach(slot => {
    text += `${CRYPT_REC_TAG}|${slot.salt}|${slot.iterations}|${slot.check}\n`;
  });
  return text;
}

//// FileCrypt ////
//
// The password for one file, and the key derived from it, for as long as the
// window is open:
//   status()   what the record file says -- whether a password is set at all
//   setup()    the first password, refused once one is set
//   unlock()   derive, check against the record, keep the key in memory
//   lock()     drop the key
//   change()   old password -> new one, re-encrypting in between
//
// The key lives on the instance and nowhere else: not in storage, not in a
// cookie, never on the server.

export class FileCrypt {

  // options: file (the record file, e.g. 'lists-pwd.save')
  constructor(options = {}) {
    this.file = options.file;
    this.key = null;    // CryptoKey while unlocked
    this.params = null; // { salt, iterations } the key came from
    this.slots = null;  // last record read, so a change can rebuild it
  }

  //// state ////

  isUnlocked() {
    return this.key !== null;
  }

  lock() {
    this.key = null;
    this.params = null;
  }

  //// the record file ////

  // { ready, slots }, or null when the file could not be read at all. A blank
  // file is not a failure: it is what "no password yet" looks like.
  async status() {
    const text = await readFile(this.file);
    if (text === null) {
      return null;
    }
    const slots = parseCryptRecord(text);
    this.slots = slots;
    return { ready: slots.length > 0, slots: slots };
  }

  async writeRecord(slots) {
    const sent = await writeFile(this.file, buildCryptRecord(slots));
    if (sent === true) {
      this.slots = slots;
    }
    return sent;
  }

  //// setting up ////

  // First password for this file. The record is re-read immediately before
  // writing: a window sitting open on a stale "not set up" does not get to
  // walk over a password set in the meantime, which would leave whatever was
  // encrypted under it unreadable for good.
  async setup(password) {
    if (!cryptoReady()) {
      return { ok: false, error: 'encryption not available' };
    }
    const bad = checkPassword(password);
    if (bad !== "") {
      return { ok: false, error: bad };
    }
    const state = await this.status();
    if (state === null) {
      return { ok: false, error: 'server connection not available' };
    }
    if (state.ready === true) {
      return { ok: false, error: 'a password is already set' };
    }
    const salt = newCryptSalt();
    const iterations = CRYPT_ITERATIONS;
    const derived = await deriveCryptKeys(password, salt, iterations);
    const slot = {
      salt: salt,
      iterations: iterations,
      check: await hashVerifier(derived.verifier)
    };
    if (await this.writeRecord([slot]) !== true) {
      return { ok: false, error: 'password not saved' };
    }
    this.key = derived.key;
    this.params = { salt: salt, iterations: iterations };
    return { ok: true, error: "" };
  }

  //// unlocking ////

  // Derives against every set of parameters the record holds and keeps the
  // key belonging to the one that matches. There are two only while a
  // password change was started and never finished, and then either password
  // opens the file -- whichever one the data is actually encrypted under.
  async unlock(password) {
    if (!cryptoReady()) {
      return { ok: false, error: 'encryption not available' };
    }
    if (typeof password !== 'string' || password === "") {
      return { ok: false, error: 'enter the password' };
    }
    const state = await this.status();
    if (state === null) {
      return { ok: false, error: 'server connection not available' };
    }
    if (state.ready !== true) {
      return { ok: false, error: 'no password set' };
    }
    for (let i = 0; i < state.slots.length; i++) {
      const slot = state.slots[i];
      const derived = await deriveCryptKeys(password, slot.salt, slot.iterations);
      if (await hashVerifier(derived.verifier) === slot.check) {
        this.key = derived.key;
        this.params = { salt: slot.salt, iterations: slot.iterations };
        return { ok: true, error: "" };
      }
    }
    return { ok: false, error: 'wrong password' };
  }

  //// using it ////

  async encrypt(text, context = '') {
    if (!this.isUnlocked()) {
      return null;
    }
    return await encryptText(this.key, text, context);
  }

  async decrypt(blob, context = '') {
    if (!this.isUnlocked()) {
      return null;
    }
    return await decryptText(this.key, blob, context);
  }

  //// changing the password ////

  // Three steps, because the re-encrypted data and the new record cannot land
  // together:
  //   park     the new record is written as a second row, under the old one,
  //            which is still what the data answers to
  //   rewrite  the caller re-encrypts everything with the new key and gets it
  //            onto the server; it returns true only if that write landed
  //   promote  the new row becomes the only row
  // A rewrite that fails puts the record back as it was, and the old password
  // still opens everything. A browser that dies in between leaves both rows,
  // and unlock() finds its way in with either password.
  //
  // rewrite is handed the new key and must not touch this instance: the swap
  // happens here, once the write is known to have landed.
  async change(oldPassword, newPassword, rewrite) {
    if (!cryptoReady()) {
      return { ok: false, error: 'encryption not available' };
    }
    const bad = checkPassword(newPassword);
    if (bad !== "") {
      return { ok: false, error: bad };
    }
    if (String(oldPassword) === String(newPassword)) {
      return { ok: false, error: 'new password must differ' };
    }
    const opened = await this.unlock(oldPassword);
    if (opened.ok !== true) {
      return opened;
    }
    const current = { ...this.params, check: "" };
    const held = (this.slots || []).find(slot => slot.salt === this.params.salt);
    if (!held) {
      return { ok: false, error: 'password record changed, try again' };
    }
    current.check = held.check;
    const salt = newCryptSalt();
    const iterations = CRYPT_ITERATIONS;
    const derived = await deriveCryptKeys(newPassword, salt, iterations);
    const next = {
      salt: salt,
      iterations: iterations,
      check: await hashVerifier(derived.verifier)
    };
    if (await this.writeRecord([current, next]) !== true) {
      return { ok: false, error: 'server connection not available' };
    }
    let written = false;
    try {
      written = (await rewrite(derived.key)) === true;
    } catch (error) {
      written = false;
    }
    if (written !== true) {
      // the data is still under the old password, so the record goes back to
      // describing only that
      await this.writeRecord([current]);
      return { ok: false, error: 'password not changed, nothing was saved' };
    }
    this.key = derived.key;
    this.params = { salt: salt, iterations: iterations };
    if (await this.writeRecord([next]) !== true) {
      // the data is already under the new password and the old row is still
      // there, which unlock() copes with -- it is tidied on the next change
      return { ok: true, error: 'password changed, record not tidied' };
    }
    return { ok: true, error: "" };
  }
}
