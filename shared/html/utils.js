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

// --- Mathematical Conversion Helpers (Verbose Implementation) --- //

export function hexToRgb(hexString) {
  const cleanHex = hexString.replace(/^#/, '');
  const redDecimal = parseInt(cleanHex.substring(0, 2), 16);
  const greenDecimal = parseInt(cleanHex.substring(2, 4), 16);
  const blueDecimal = parseInt(cleanHex.substring(4, 6), 16);
  return { red: redDecimal, green: greenDecimal, blue: blueDecimal };
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
  const chroma = value * saturation;
  const hueSegment = hue / 60;
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
  } else if (hueSegment >= 5 && hueSegment < 6) {
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