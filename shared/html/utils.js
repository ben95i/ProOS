// Automate Website - JavaScript Utilities

const REFRESH_INTERVAL = 10; // in seconds
const TIMEOUT_INTERVAL = 50; // in seconds

// Query values are pasted straight into the URL below. Only the characters
// that would end a value or be read as an escape are encoded; everything the
// site actually sends (letters, digits, _ - ~ . @, including the '@' that
// base64URLSafeEncode uses for '=') goes out byte for byte as it always has.
const QUERY_UNSAFE = /[%&#+=?\s]/g;

function encodeArg(value) {
  const text = (value === null || value === undefined) ? '' : String(value);
  return text.replace(QUERY_UNSAFE, (ch) =>
    '%' + ch.charCodeAt(0).toString(16).toUpperCase().padStart(2, '0'));
}

// transmit a command
export async function sendCmd(act, arg1, arg2 = '') {
  // construct API URL
  const url = location.protocol + "//"+ location.hostname + "/api?var=" + encodeArg(arg2) + "&arg=" + encodeArg(arg1) + "&action=" + encodeArg(act);
  // send request
  try {
    const response = await fetch(url, {
      method: 'GET'
    });
    // return response
    let out = await response.json();
    return out;
  } catch (error) {
    showPopup("[sendCmd] " + error);
    return null;
  }
}

export function refreshInterval() {
  return REFRESH_INTERVAL;
}

export function timeoutInterval() {
  return TIMEOUT_INTERVAL;
}

export function splitMenuData(data) {
  // Strip quotes if they exist at the very beginning or end
  let start = data.charCodeAt(0) === 34 ? 1 : 0; // 34 is the '"' character
  let end = data.charCodeAt(data.length - 1) === 34 ? data.length - 1 : data.length;
  // Slice the string and split it in one go
  return data.slice(start, end).split(/\\n|\n/);
}

// request rate limiter
export function requestThrottle(func, limit) {
  let lastFunc;
  let lastRan;
  // A declaration rather than a function expression on a const: the throttled
  // function needs a name now that cancel() hangs off it, and naming it that
  // way is what the linter asks for. Still a plain function, not an arrow, so
  // `this` and `arguments` reach the wrapped call exactly as before.
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
  // Drops a trailing call that has not fired yet. Without this the last
  // value of a drag went out up to `limit` after the menu or window it came
  // from had been torn down -- a level arriving at a device whose slider is
  // no longer on screen.
  throttled.cancel = function() {
    clearTimeout(lastFunc);
    lastFunc = undefined;
  };
  return throttled;
}

export function setTheme(newTheme) {
  let body = document.getElementsByTagName("html")[0];
  body.style.setProperty('--main-color', newTheme);
  localStorage.setItem("main-color", newTheme);
}

export function classDisplay(_elem, _state) {
  let _itr;
  let _class = document.getElementsByClassName(_elem);
  for (_itr = 0; _itr < _class.length; _itr++) {
    _class[_itr].style.display = _state;
  }
}

// add / remove a class from a element
export function addClassToElem(_action, _elmid, _class) {
  let _elm = document.getElementById(_elmid);
  if (_action === 'add') {
    if (!(_elm.classList.contains(_class))) {
      _elm.classList.add(_class);
    }
  }
  if (_action === 'remove') {
    if (_elm.classList.contains(_class)) {
      _elm.classList.remove(_class);
    } 
  }
}

export function checkElemIsVisibleByID(id){
  let _elmvis = false;
  let _elem = document.getElementById(id);
  if (_elem) {
    var _style = window.getComputedStyle(_elem)
    if (_style.display !== 'none') {
      _elmvis = true;
    }
  }
  return _elmvis;
}

// remove element by ID
export function removeWindow(id) {
  let win = document.getElementById(id);
  if (win) { // window exists
    document.body.removeChild(win);
  }
}

export function splitByDelimiter(str, delimiter = '-') {
  if (!str) return ['', ''];
  const parts = str.split(delimiter);
  const before = parts[0] || '';
  const after = parts.length > 1 ? parts.slice(1).join(delimiter) : '';
  return [before, after];
}


export function cleanString(data) {
  if (data === null || data == undefined) {
    return null;
  } else {
    let out = data.replace(/(\r\n|\n|\r)/gm, '');
    return out;
  }
}

export function base64URLSafeEncode(buffer) {
  return btoa(buffer)
    .replace(/\+/g, '-')  // Convert '+' to '-'
    .replace(/\//g, '_')  // Convert '/' to '_'
    .replace(/\=/g, '@'); // Convert '=' to '@'
}

// URL-safe base64 back to the standard alphabet, which is what an
// <img src="data:..."> expects. Does not decode, only swaps characters.
export function base64URLSafeToStandard(data) {
  if (!data) {
    return '';
  }
  return data
    .replace(/-/g, '+')   // Convert '-' back to '+'
    .replace(/_/g, '/')   // Convert '_' back to '/'
    .replace(/@/g, '=');  // Convert '@' back to '='
}

export function isNumeric(value) {
  // Coerce to number and check if it's an integer. 
  // We check typeof to prevent empty strings ("") from converting to 0.
  return typeof value === 'string' && value.trim() !== '' && Number.isInteger(Number(value));
}

export function mapNumber(num, inMin, inMax, outMin, outMax) {
  return (num - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

export function hexToRgb(hex_in) {
  const hex = hex_in.replace(/^#/, '');
  return { 
    red: parseInt(hex.substring(0, 2), 16), 
    green: parseInt(hex.substring(2, 4), 16), 
    blue: parseInt(hex.substring(4, 6), 16) 
  };
}

export function rgbToHex(red, green, blue) {
  const redHex = red.toString(16).padStart(2, '0');
  const greenHex = green.toString(16).padStart(2, '0');
  const blueHex = blue.toString(16).padStart(2, '0');
  const combinedHex = `#${redHex}${greenHex}${blueHex}`;
  return combinedHex.toUpperCase();
}

export function rgbToHexFromStyle(rgbString) {
  // Converts "rgb(255, 0, 0)" to "#FF0000"
  const rgbValues = rgbString.match(/\d+/g);
  if (!rgbValues || rgbValues.length < 3) return null;
  const red = parseInt(rgbValues[0]);
  const green = parseInt(rgbValues[1]);
  const blue = parseInt(rgbValues[2]);
  return rgbToHex(red, green, blue);
}

export function hsvToRgb(hue, saturation, value) {
  // Wrap into [0, 360) so hue === 360 (slider at max) maps back to red
  // instead of falling through every branch below and returning gray.
  const wrappedHue = ((hue % 360) + 360) % 360;
  const chroma = value * saturation;
  const hueSegment = wrappedHue / 60;
  const intermediateValue = chroma * (1 - Math.abs((hueSegment % 2) - 1));
  const valueMatch = value - chroma;
  let redRaw = 0;
  let greenRaw = 0;
  let blueRaw = 0;
  if (hueSegment >= 0 && hueSegment < 1) {
    redRaw = chroma; greenRaw = intermediateValue; blueRaw = 0;
  } else if (hueSegment >= 1 && hueSegment < 2) {
    redRaw = intermediateValue; greenRaw = chroma; blueRaw = 0;
  } else if (hueSegment >= 2 && hueSegment < 3) {
    redRaw = 0; greenRaw = chroma; blueRaw = intermediateValue;
  } else if (hueSegment >= 3 && hueSegment < 4) {
    redRaw = 0; greenRaw = intermediateValue; blueRaw = chroma;
  } else if (hueSegment >= 4 && hueSegment < 5) {
    redRaw = intermediateValue; greenRaw = 0; blueRaw = chroma;
  } else {
    redRaw = chroma; greenRaw = 0; blueRaw = intermediateValue;
  }
  const redFinal = Math.round((redRaw + valueMatch) * 255);
  const greenFinal = Math.round((greenRaw + valueMatch) * 255);
  const blueFinal = Math.round((blueRaw + valueMatch) * 255);
  return { red: redFinal, green: greenFinal, blue: blueFinal };
}

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
  const value = maxColorComponent;
  return { hue: hue, saturation: saturation, value: value };
}
//// Shared UI helpers ////
// Moved here out of main.js so bookmarks.js / lists.js can use the same copy.

// Tag function for building DOM markup with template literals.
// Strips whitespace between tags so the resulting DOM contains no stray
// text nodes -- output is identical to element-by-element createElement()
// construction (whitespace text nodes between inline-block elements would
// otherwise render as visible gaps).
export function html(strings, ...values) {
  return strings
    .reduce((out, str, i) => out + str + (i < values.length ? values[i] : ""), "")
    .replace(/>\s+</g, "><")
    .trim();
}

let messagePopupTimer = null;

// lets the page teardown drop a pending fade (main.js owns the other popups'
// timers, this one has always lived in this module)
export function clearPopupTimer() {
  if (messagePopupTimer) {
    clearTimeout(messagePopupTimer);
    messagePopupTimer = null;
  }
}

export function showPopup(message) {
  const win_name = "msg-popup";
  const text_name = "msg-text";
  let msgpopwin = document.getElementById(win_name);
  if (!msgpopwin) { // window does not exist
    // draw window (popup container + message text box)
    msgpopwin = document.createElement("div");
    msgpopwin.classList.add("popup-alert");
    msgpopwin.id = win_name;
    msgpopwin.innerHTML = html`<div id="${text_name}"></div>`;
    // Attached once, for the life of the window. A message arriving mid-fade
    // takes the class off again, which cancels the animation before it ends:
    // a {once:true} listener registered per fade would never fire and never
    // unregister, so they stacked up on this one long-lived element.
    msgpopwin.addEventListener("animationend", () => {
      if (msgpopwin.classList.contains("fade-out") && msgpopwin.isConnected) {
        msgpopwin.remove();
      }
    });
    // add to page
    document.body.appendChild(msgpopwin);
  }
  msgpopwin.classList.remove("fade-out");
  // set message text
  const msgtext = document.getElementById(text_name);
  msgtext.innerHTML = message;
  // reset hide window timeout
  clearPopupTimer();
  // hide window after delay
  messagePopupTimer = setTimeout(() => {
    messagePopupTimer = null;
    // fade out window; the listener above removes it once the fade ends
    msgpopwin.classList.add("fade-out");
  }, 4000); // hide delay in (ms)
}

// read a menu / list file
export async function loadMenu(url) {
  try {
    const response = await fetch(url, {
      method: "GET"
    });
    // return response
    return await response.text();
  } catch (error) {
    showPopup("[loadMenu] failed to fetch: " + error);
  }
}

// save file API POST call
export function savePOST(file, data, onSuccess) {
  const url = location.protocol + "//" + location.hostname + "/api/write?file=" + file;
  // submit request
  const xhr = new XMLHttpRequest();
  xhr.open("POST", url);
  xhr.setRequestHeader("Content-Type", "text/plain");
  xhr.onreadystatechange = function() {
    if (xhr.readyState === XMLHttpRequest.DONE) {
      if (xhr.status == 200) {
        // action on successful transmit
        if (typeof onSuccess === 'function') {
          onSuccess(file);
        }
      } else {
        showPopup("[savePOST] failed to send: " + file);
      }
    }
  }
  xhr.send(data);
}

export function scrollToBottom() {
  window.scrollTo({
    top: document.body.scrollHeight,
    behavior: 'smooth'
  });
}

//// Drag to re-order ////

// grip handle markup, shared by every draggable list so the icon matches
export const DRAG_GRIP_CLASS = 'drag-grip';
export const DRAG_GRIP_OFF_CLASS = 'drag-grip--off';
export const DRAG_GRIP_HTML = '<i class="fad fa-grip-lines"></i>';

export function dragGripElement(fixed = false) {
  const grip = document.createElement('span');
  grip.className = DRAG_GRIP_CLASS + (fixed === true ? ` ${DRAG_GRIP_OFF_CLASS}` : '');
  grip.innerHTML = DRAG_GRIP_HTML;
  return grip;
}

// Pointer drag re-ordering for a list of rows. The row is moved in the DOM
// while it is dragged, so the caller only has to read the new order back out
// of the container when onDrop fires.
//   rowClass  class every draggable row carries
//   onDrop    called once the row is released
export class DragSort {

  constructor(options = {}) {
    this.rowClass = options.rowClass || 'drag-row';
    this.onDrop = typeof options.onDrop === 'function' ? options.onDrop : function () {};
    this.drag = null;
  }

  // returns true when the event started a drag
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
    const next = forward === true ? row.nextElementSibling : row.previousElementSibling;
    if (!next || !next.classList.contains(this.rowClass)) {
      return null;
    }
    // rows that cannot be dragged hold their place
    return next.querySelector(`.${DRAG_GRIP_OFF_CLASS}`) ? null : next;
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

//// Shared file transfer ////

export function fileURL(action, file) {
  return `${location.protocol}//${location.hostname}/api/${action}?file=${file}`;
}

// returns the file text, or null when the server cannot be reached
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

// returns true only when the server confirms the write; a failed attempt
// leaves the file on the server exactly as it was
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

// Hands a file to the browser to send on its own. A normal write is awaited,
// and a page being torn down is exactly when the browser cancels requests
// still in flight; a beacon is handed over and finished off without the page.
// There is no answer to read, so this reports only whether the browser
// accepted it. Shared by the lists window and the bookmarks menu.
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

export function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

//// Shared edit lock ////
//
// One editor at a time for a file that several browsers can open. The lock is
// the first row of the file itself:
//   LOCK|STATE|OWNER_TOKEN|TIMESTAMP|0
// so it travels with the data and needs no second endpoint. The owner
// re-stamps the timestamp while it holds the lock, and a lock left behind by
// a browser that went away expires on its own.
//
// The owner supplies three callbacks:
//   read()   async, the file text or null
//   write()  async (text) => true when the server took it
//   body()   the data rows to write under the lock row
// and optionally refresh() (what the heartbeat writes, defaults to lock row +
// body) and onLost(text) (a competing window won the lock).

// Still exported so the menus can recognise -- and drop -- the leading LOCK|
// row that older data files carry from when the lock shared their file.
export const LOCK_TAG = 'LOCK';

//// Shared edit lock ////
//
// One editor at a time for a menu that several browsers can open at once. The
// lock lives in a small file of its own beside the data it guards -- the
// lists window uses lists-lock.save, the bookmarks menu bookmarks-lock.save --
// holding a single row:
//   OWNER_TOKEN|TIMESTAMP
// A blank token, or a timestamp older than the expiry, means the lock is free.
//
// Keeping it out of the data file is what makes re-stamping cheap: it is a one
// row write to a tiny file, so it can run often without pushing the whole menu
// back over the wire, and it never touches the data at all. Older data files
// carry a leading LOCK| row from when the two shared a file; both menus drop
// that row when they parse, and it goes for good the first time they save.

export const LOCK_EXPIRE_MS = 75000; // a lock left behind is ignored after this
export const LOCK_BEAT_MS = 15000;   // owner re-stamps the lock this often
export const LOCK_SETTLE_MS = 600;   // let a competing claim land before checking

export class FileLock {

  constructor(options = {}) {
    this.file = options.file; // the lock file this instance guards
    if (typeof options.onLost === 'function') {
      this.onLost = options.onLost;
    } else {
      this.onLost = function () {};
    }
    // called with true / false after every exchange, for a caller that shows
    // a connection state of its own
    if (typeof options.onExchange === 'function') {
      this.onExchange = options.onExchange;
    } else {
      this.onExchange = function () {};
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
    if (cols.length < 2) {
      return free;
    }
    if (!isNumeric(cols[1])) {
      return free;
    }
    return { token: cols[0], stamp: Number(cols[1]) };
  }

  // someone else is editing, and recently enough that they have not gone away
  busy(text) {
    if (text === null) {
      return false; // nothing to go on; claim() refuses on its own
    }
    const row = this.parse(text);
    if (row.token === "") {
      return false;
    }
    if (row.token === this.token) {
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
  // and this reads it. Returns false when another window holds it, or when
  // the read or the write failed.
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
  // waiting on it; settle() below holds any later write until it lands.
  confirm() {
    const running = this.runConfirm();
    this.confirming = running;
    // The promise .finally() hands back is its own, and a rejection travels
    // down it as well: with nothing attached it surfaced as an unhandled
    // rejection even though settle() below catches the one that matters.
    // This branch exists only to end that chain.
    running.finally(() => {
      if (this.confirming === running) {
        this.confirming = null;
      }
    }).catch(() => {});
    return running;
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
  // is still unknown whether this window holds the lock
  async settle() {
    if (this.confirming === null) {
      return;
    }
    try {
      await this.confirming;
    } catch (error) {
      // a failed confirm leaves the claim standing, which release handles
    }
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
  // One row, to a file of its own -- the menu is not involved, and nothing the
  // user has not saved goes anywhere.
  async refresh() {
    if (this.owned !== true) {
      return true;
    }
    return await this.write(this.line(true));
  }

  // hands the lock back; write is false when the server is known to be
  // unreachable, and then the lock is left to expire instead
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
