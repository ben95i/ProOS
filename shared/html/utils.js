// Automate Website - JavaScript Utilities

const REFRESH_INTERVAL = 10; // in seconds
const TIMEOUT_INTERVAL = 50; // in seconds

// transmit a command
export async function sendCmd(act, arg1, arg2 = '') {
  // construct API URL
  const url = location.protocol + "//"+ location.hostname + "/api?var=" + arg2 + "&arg=" + arg1 + "&action=" + act;
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
  return function() {
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
  };
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
    // add to page
    document.body.appendChild(msgpopwin);
  }
  msgpopwin.classList.remove("fade-out");
  // set message text
  const msgtext = document.getElementById(text_name);
  msgtext.innerHTML = message;
  // reset hide window timeout
  clearTimeout(messagePopupTimer);
  // hide window after delay
  messagePopupTimer = setTimeout(() => {
    // fade out window
    msgpopwin.classList.add("fade-out");
    // wait for the transition to end before removing window
    msgpopwin.addEventListener("animationend", () => {
      if (msgpopwin.isConnected) {
        msgpopwin.remove();
      }
    }, { once: true });
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
