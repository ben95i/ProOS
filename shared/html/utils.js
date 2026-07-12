// Automate Website - JavaScript Utilities

// transmit a command
export async function sendCmd(act, arg1, arg2) {
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

export function isNumeric(value) {
  // Coerce to number and check if it's an integer. 
  // We check typeof to prevent empty strings ("") from converting to 0.
  return typeof value === 'string' && value.trim() !== '' && Number.isInteger(Number(value));
}

export function mapNumber(num, inMin, inMax, outMin, outMax) {
  return (num - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

export function hsvToRgb(h, s, v) {
  let r, g, b, i, f, p, q, t;
  h /= 360;
  i = Math.floor(h * 6);
  f = h * 6 - i;
  p = v * (1 - s);
  q = v * (1 - f * s);
  t = v * (1 - (1 - f) * s);
  switch (i % 6) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    case 5: r = v, g = p, b = q; break;
  }
  return { r: Math.round(r * 255), g: Math.round(g * 255), b: Math.round(b * 255) };
}

export function rgbToHsv(r, g, b) {
  r /= 255, g /= 255, b /= 255;
  let max = Math.max(r, g, b), min = Math.min(r, g, b);
  let h, s, v = max;
  let d = max - min;
  s = max === 0 ? 0 : d / max;
  if (max === min) {
    h = 0; 
  } else {
    switch (max) {
      case r: h = (g - b) / d + (g < b ? 6 : 0); break;
      case g: h = (b - r) / d + 2; break;
      case b: h = (r - g) / d + 4; break;
    }
    h /= 6;
  }
  return { h: h * 360, s: s, v: v };
}

export function hexToRgb(hex) {
  let bigint = parseInt(hex.replace('#', ''), 16);
  return { r: (bigint >> 16) & 255, g: (bigint >> 8) & 255, b: bigint & 255 };
}

export function rgbToHex(r, g, b) {
  return "#" + ((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1).toUpperCase();
}