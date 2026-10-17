// Automate Website - Pixels window
//
// The More... row of the Pixels Control menu opens this window: one
// brightness slider per LED host in the mainmenu file, each under the host's
// name, a line saying what it is showing ("Maze is active") and a link to its
// own page. Every host is asked for its status on its own and all at once, so
// a host that is off is marked "Not available" while the others fill in. Server page only: main.js hands openPixelsWindow() to
// the menus as their openPixels hook, and calls closePixelsWindow() when the
// page is taken down. The window's state lives on App.pixelsWindow while it
// is open.

import { sendCmd, readFile, splitMenuData, promptWindow, onButtons,
  makeElement, requestThrottle, gridButtonTemplate } from './utils.js';
import { removeDropdowns, SLIDER_MS } from './menus.js';

// the LED hosts, one per line as URL|NAME; nodeapi/ledparser.js sends
// every Pixels command to the same list
const PIXELS_HOST_FILE = 'mainmenu';
// the Animated menu's effects, one per line as effect|Name, for the name a
// status line shows
const PIXELS_EFFECT_FILE = 'ledeffectmenu';

// The API's commands for one host (nodeapi/ledparser.js), with the host's
// NAME as the second argument:
//   /api?action=leds&arg=fc_host_status&var=LEDwall
//     -> { effect: "maze", state: "active", color: null, brightness: 128 }
//   /api?action=leds&arg=fc_host_set_120&var=LEDwall   -> "OK"
// Anything else back means the host did not answer.
const PIXELS_STATUS_CMD = 'fc_host_status';
const PIXELS_SET_CMD = 'fc_host_set';
const PIXELS_SET_OK = 'OK';

const PIXELS_PROMPT_ID = 'pixels__prompt';
const PIXELS_TITLE = 'Pixels';
const PIXELS_LOADING = 'Loading...';
const PIXELS_NO_FILE = 'Could not read the host list';
const PIXELS_NO_HOSTS = 'No LED hosts listed';
const PIXELS_UNAVAILABLE = 'Not available';

// What a host's status line reads for each state it can answer: an effect's
// name before the first two, a dot in the colour before the third.
const PIXELS_STATE_TEXT = {
  active: 'is active',
  paused: 'is paused',
  color: 'is set'
};
// the state line for 'off', and for anything else
const PIXELS_NOT_ACTIVE = 'Not active';
// the name an effect missing from the effects file goes by
const PIXELS_EFFECT_FALLBACK = 'Effect';
const PIXELS_LINK_ICON = 'fad fa-external-link-alt';
// the size of the window's two buttons (widened in pixels.css)
const PIXELS_BUTTON_CLASS = 'button_left_txtwin';

// a host's box while its level is being read, and once it did not answer
const PIXELS_LOADING_CLASS = 'is-loading';
const PIXELS_DOWN_CLASS = 'is-down';
const PIXELS_MESSAGE_ERROR_CLASS = 'pixels__message--error';
// on a state line that has just changed, to fade it in (pixels.css)
const PIXELS_STATE_FADE_CLASS = 'pixels__state--fade';
// the Refresh icon, which turns while the hosts are being asked
const PIXELS_REFRESH_ICON_ID = 'pixels__refresh-icon';
const PIXELS_SPIN_CLASS = 'fa-spin';
// how long a slider takes to move to a level read on Refresh
const PIXELS_SLIDE_MS = 300;

// the brightness every host counts in
const PIXELS_LEVEL_MIN = 0;
const PIXELS_LEVEL_MAX = 255;
const PIXELS_LEVEL_STEP = 1;

// Opens the window and asks every host for its level. A second press of
// More... while it is open does nothing.
export function openPixelsWindow() {
  if (App.pixelsWindow) {
    return;
  }
  removeDropdowns();
  const promptElement = promptWindow(PIXELS_PROMPT_ID, `
    <div class="pixels__title">${PIXELS_TITLE}</div>
    <div class="pixels__hosts scroll-bar-style" id="pixels__hosts"></div>
    <div class="popup-actions pixels__actions">
      ${gridButtonTemplate({
        buttonId: 'pixels__btn-close',
        icon: 'fad fa-times-octagon',
        buttonClass: PIXELS_BUTTON_CLASS
      })}
      ${gridButtonTemplate({
        buttonId: 'pixels__btn-refresh',
        icon: 'fad fa-sync-alt',
        iconId: PIXELS_REFRESH_ICON_ID,
        buttonClass: PIXELS_BUTTON_CLASS
      })}
    </div>
  `);
  const pixelsWindow = {
    win: promptElement,
    hostList: document.getElementById('pixels__hosts'),
    refreshIcon: document.getElementById(PIXELS_REFRESH_ICON_ID),
    // per host, in file order: its name, its box, its state line and slider
    hosts: [],
    // effect -> its name in the Animated menu, read with the host list
    effectNames: new Map(),
    // bumped per load, so a host list read before a Refresh cannot draw
    // over the one read after it
    loadSeq: 0,
    // While a slider is dragged its level goes at most every SLIDER_MS, as
    // the Control menu's does. One throttle serves every slider, as only one
    // can be dragged at a time.
    sendLevel: requestThrottle((hostName, level) => {
      sendCmd('leds', `${PIXELS_SET_CMD}_${level}`, hostName);
    }, SLIDER_MS),
    // set on close; a reply landing afterwards is dropped
    closed: false
  };
  App.pixelsWindow = pixelsWindow;
  onButtons(promptElement, (button) => {
    switch (button.id) {
      case 'pixels__btn-close':
        closePixelsWindow();
        break;
      case 'pixels__btn-refresh':
        pixelsLoadHosts(pixelsWindow);
        break;
    }
  });
  pixelsLoadHosts(pixelsWindow);
}

// Takes the window down, with the slider send still waiting to go and any
// slider still moving.
export function closePixelsWindow() {
  const pixelsWindow = App.pixelsWindow;
  if (!pixelsWindow) {
    return;
  }
  App.pixelsWindow = null;
  pixelsWindow.closed = true;
  pixelsWindow.sendLevel.cancel();
  pixelsWindow.hosts.forEach(hostControls => pixelsStopSlide(hostControls));
  pixelsWindow.win.remove();
}

// mainmenu file text -> [{ url, name }], read the way ledparser.js reads it:
// the name is the line's last field, so an older URL|link|NAME line reads
// the same; a line with no name goes by its URL, and a blank line is left out.
// 'http://ledwall.home:80|LEDwall' -> { url: 'http://ledwall.home:80', name: 'LEDwall' }
function parsePixelHosts(fileText) {
  const hosts = [];
  for (const line of splitMenuData(fileText)) {
    const fields = line.split('|');
    const url = fields[0].trim();
    if (url === '') {
      continue;
    }
    const lastField = fields[fields.length - 1].trim();
    let name = url;
    if (fields.length > 1 && lastField !== '') {
      name = lastField;
    }
    hosts.push({ url: url, name: name });
  }
  return hosts;
}

// ledeffectmenu file text -> Map of effect to its name in the Animated menu.
// 'maze|Maze' -> 'maze' => 'Maze'. A file that could not be read gives none.
function parseEffectNames(fileText) {
  const effectNames = new Map();
  if (fileText === null) {
    return effectNames;
  }
  for (const line of splitMenuData(fileText)) {
    const fields = line.split('|');
    const effect = fields[0].trim();
    if (effect === '' || fields.length < 2) {
      continue;
    }
    effectNames.set(effect, fields[1].trim());
  }
  return effectNames;
}

// Reads the host list and the effect names, draws a box per host, then asks
// every host for its status at once; each box fills in as its own answer
// lands, and the Refresh icon turns until the last one has. Refresh runs it
// again: while the list is the same, the boxes on screen stay and take the
// new statuses as they land, their sliders moving to the new level. Only a
// list that changed is drawn afresh.
async function pixelsLoadHosts(pixelsWindow) {
  pixelsWindow.loadSeq += 1;
  const sequence = pixelsWindow.loadSeq;
  if (pixelsWindow.hosts.length === 0) {
    pixelsShowMessage(pixelsWindow, PIXELS_LOADING, false);
  }
  pixelsWindow.refreshIcon.classList.add(PIXELS_SPIN_CLASS);
  const [fileText, effectFileText] = await Promise.all([
    readFile(PIXELS_HOST_FILE),
    readFile(PIXELS_EFFECT_FILE)
  ]);
  if (pixelsWindow.closed === true || sequence !== pixelsWindow.loadSeq) {
    return;
  }
  pixelsWindow.effectNames = parseEffectNames(effectFileText);
  let hosts = [];
  let listError = '';
  if (fileText === null) {
    listError = PIXELS_NO_FILE;
  } else {
    hosts = parsePixelHosts(fileText);
    if (hosts.length === 0) {
      listError = PIXELS_NO_HOSTS;
    }
  }
  if (listError !== '') {
    pixelsClearHosts(pixelsWindow);
    pixelsShowMessage(pixelsWindow, listError, true);
    pixelsWindow.refreshIcon.classList.remove(PIXELS_SPIN_CLASS);
    return;
  }
  if (!pixelsSameHosts(pixelsWindow.hosts, hosts)) {
    pixelsClearHosts(pixelsWindow);
    pixelsWindow.hostList.textContent = '';
    hosts.forEach((host, hostIndex) => {
      const hostControls = pixelsHostBox(pixelsWindow, host, hostIndex);
      pixelsWindow.hosts.push(hostControls);
      pixelsWindow.hostList.appendChild(hostControls.box);
    });
  }
  const statusReads = pixelsWindow.hosts.map(hostControls => {
    return pixelsReadStatus(pixelsWindow, hostControls);
  });
  await Promise.all(statusReads);
  // a newer Refresh owns the icon from here
  if (pixelsWindow.closed === true || sequence !== pixelsWindow.loadSeq) {
    return;
  }
  pixelsWindow.refreshIcon.classList.remove(PIXELS_SPIN_CLASS);
}

// Are these the hosts the boxes on screen were drawn for, in the same order?
function pixelsSameHosts(hostControlsList, hosts) {
  if (hostControlsList.length !== hosts.length) {
    return false;
  }
  return hosts.every((host, hostIndex) => {
    const hostControls = hostControlsList[hostIndex];
    return hostControls.name === host.name && hostControls.url === host.url;
  });
}

// Lets go of the boxes about to be taken off the screen: a level still
// waiting to go and a slider still moving belong to them.
function pixelsClearHosts(pixelsWindow) {
  pixelsWindow.sendLevel.cancel();
  pixelsWindow.hosts.forEach(hostControls => pixelsStopSlide(hostControls));
  pixelsWindow.hosts = [];
}

// one line of text in place of the host boxes: Loading..., or what went wrong
function pixelsShowMessage(pixelsWindow, text, isError) {
  const message = makeElement('div', 'pixels__message');
  message.classList.toggle(PIXELS_MESSAGE_ERROR_CLASS, isError === true);
  message.textContent = text;
  pixelsWindow.hostList.textContent = '';
  pixelsWindow.hostList.appendChild(message);
}

// One host's box: its name over its state line, and a link to its page,
// above a slider, greyed out until its status is read. Built node by node,
// as the names and URLs are file text.
// Answers { name, url, box, stateLine, stateKey, slider, slideFrame,
// dragging, readSeq, saveSeq }.
function pixelsHostBox(pixelsWindow, host, hostIndex) {
  const box = makeElement('div', 'pixels__host');
  box.id = `pixels__host-${hostIndex}`;
  box.classList.add(PIXELS_LOADING_CLASS);
  const head = makeElement('div', 'pixels__host-head');
  const nameColumn = makeElement('div', 'pixels__name');
  const label = makeElement('span', 'pixels__label');
  label.textContent = host.name;
  const stateLine = makeElement('span', 'pixels__state');
  stateLine.textContent = PIXELS_LOADING;
  nameColumn.append(label, stateLine);
  const link = makeElement('a', 'pixels__link');
  link.href = host.url;
  link.title = host.name;
  link.appendChild(makeElement('i', PIXELS_LINK_ICON));
  // shown in the link's place once the host has not answered
  const status = makeElement('span', 'pixels__status');
  status.textContent = PIXELS_UNAVAILABLE;
  head.append(nameColumn, link, status);
  const sliderBox = makeElement('div', 'slider-wide_container pixels__slider');
  const slider = makeElement('input', 'slider-wide');
  slider.type = 'range';
  slider.min = PIXELS_LEVEL_MIN;
  slider.max = PIXELS_LEVEL_MAX;
  slider.step = PIXELS_LEVEL_STEP;
  slider.value = PIXELS_LEVEL_MIN;
  slider.disabled = true;
  sliderBox.appendChild(slider);
  box.append(head, sliderBox);
  const hostControls = {
    name: host.name,
    url: host.url,
    box: box,
    stateLine: stateLine,
    // what the state line shows, so an unchanged status is not faded in again
    stateKey: '',
    slider: slider,
    // the animation frame of a slider moving to a new level, or null
    slideFrame: null,
    // while a finger or the mouse holds the slider, a level read leaves it be
    dragging: false,
    // bumped on each status read: only the newest read's answer is shown
    readSeq: 0,
    // bumped on each release: only the newest release's answer marks the host
    saveSeq: 0
  };
  // a slider taken hold of stops moving and stays where it is put
  slider.addEventListener('pointerdown', () => {
    hostControls.dragging = true;
    pixelsStopSlide(hostControls);
  });
  slider.addEventListener('pointerup', () => {
    hostControls.dragging = false;
  });
  slider.addEventListener('pointercancel', () => {
    hostControls.dragging = false;
  });
  // the level goes while the slider is dragged, and once more on release so
  // the last one always lands
  slider.addEventListener('input', () => {
    pixelsWindow.sendLevel(hostControls.name, slider.value);
  });
  slider.addEventListener('change', () => {
    hostControls.dragging = false;
    pixelsWindow.sendLevel.cancel();
    pixelsSaveLevel(pixelsWindow, hostControls, slider.value);
  });
  return hostControls;
}

// Shows a host as answering, with its state line and its slider at the
// brightness, from the status the API answered; or, when status is null, as
// not available: the state line, slider and link give way to the red notice.
// A slider already showing a level moves to the new one; one just shown
// (first read, or back from not available) is put straight there.
function pixelsShowHost(pixelsWindow, hostControls, status) {
  const isDown = status === null;
  const wasLoading = hostControls.box.classList.contains(PIXELS_LOADING_CLASS);
  const wasDown = hostControls.box.classList.contains(PIXELS_DOWN_CLASS);
  const sliderWasShowing = !wasLoading && !wasDown;
  hostControls.box.classList.remove(PIXELS_LOADING_CLASS);
  hostControls.box.classList.toggle(PIXELS_DOWN_CLASS, isDown);
  hostControls.slider.disabled = isDown;
  if (isDown) {
    pixelsStopSlide(hostControls);
    // so the line fades in again when the host comes back
    hostControls.stateKey = '';
    return;
  }
  if (sliderWasShowing) {
    pixelsSlideTo(hostControls, status.brightness);
  } else {
    pixelsStopSlide(hostControls);
    hostControls.slider.value = status.brightness;
  }
  pixelsShowState(pixelsWindow, hostControls, status);
}

// Moves a slider from where it stands to `level` over PIXELS_SLIDE_MS,
// easing out, so a level read on Refresh is seen to travel rather than
// jump. A slider being held is left where it is. Setting the value from
// here sends nothing: only the slider's own input and change events do.
function pixelsSlideTo(hostControls, level) {
  pixelsStopSlide(hostControls);
  if (hostControls.dragging === true) {
    return;
  }
  const slider = hostControls.slider;
  const startLevel = Number(slider.value);
  const levelChange = level - startLevel;
  if (levelChange === 0) {
    return;
  }
  const startTime = performance.now();
  const step = (frameTime) => {
    const elapsedMs = frameTime - startTime;
    let progress = elapsedMs / PIXELS_SLIDE_MS;
    if (progress > 1) {
      progress = 1;
    }
    // ease-out cubic: fast at first, settling gently on the level
    const progressLeft = 1 - progress;
    const easedLeft = progressLeft * progressLeft * progressLeft;
    const easedProgress = 1 - easedLeft;
    const levelMoved = levelChange * easedProgress;
    slider.value = Math.round(startLevel + levelMoved);
    if (progress < 1) {
      hostControls.slideFrame = requestAnimationFrame(step);
    } else {
      hostControls.slideFrame = null;
    }
  };
  hostControls.slideFrame = requestAnimationFrame(step);
}

// stops a slider that is moving, leaving it where it has got to
function pixelsStopSlide(hostControls) {
  if (hostControls.slideFrame === null) {
    return;
  }
  cancelAnimationFrame(hostControls.slideFrame);
  hostControls.slideFrame = null;
}

// Writes a host's state line, and fades it in when it says something new:
//   { state: 'active', effect: 'maze' }    -> "Maze is active"
//   { state: 'paused', effect: 'maze' }    -> "Maze is paused"
//   { state: 'color', color: 'FF8800' }    -> an orange dot, then "is set"
//   { state: 'off' }                       -> "Not active"
function pixelsShowState(pixelsWindow, hostControls, status) {
  const stateLine = hostControls.stateLine;
  let lineText = PIXELS_NOT_ACTIVE;
  let dotColor = null;
  if (status.state === 'color') {
    lineText = PIXELS_STATE_TEXT.color;
    dotColor = status.color;
  } else if (status.state === 'active' || status.state === 'paused') {
    let effectName = PIXELS_EFFECT_FALLBACK;
    if (status.effect !== null) {
      effectName = pixelsWindow.effectNames.get(status.effect) || status.effect;
    }
    lineText = `${effectName} ${PIXELS_STATE_TEXT[status.state]}`;
  }
  const stateKey = `${status.state}|${dotColor}|${lineText}`;
  if (stateKey === hostControls.stateKey) {
    return;
  }
  hostControls.stateKey = stateKey;
  stateLine.textContent = '';
  if (status.state === 'color') {
    const colorDot = makeElement('span', 'pixels__color');
    if (dotColor !== null) {
      colorDot.style.backgroundColor = `#${dotColor}`;
    }
    stateLine.append(colorDot, lineText);
  } else {
    stateLine.textContent = lineText;
  }
  // The fade is a CSS animation, which runs again only once the class has
  // come off and the browser has seen it gone: reading offsetWidth makes it
  // lay the line out without the class before it goes back on.
  stateLine.classList.remove(PIXELS_STATE_FADE_CLASS);
  void stateLine.offsetWidth;
  stateLine.classList.add(PIXELS_STATE_FADE_CLASS);
}

// Asks one host for its status. sendCmd() answers the parsed reply: the
// status object, text when the host is down, an object without a brightness
// for a request the API refused, or null when the API could not be reached.
async function pixelsReadStatus(pixelsWindow, hostControls) {
  hostControls.readSeq += 1;
  const sequence = hostControls.readSeq;
  const reply = await sendCmd('leds', PIXELS_STATUS_CMD, hostControls.name);
  // closed, or a later Refresh has asked again since
  if (pixelsWindow.closed === true || sequence !== hostControls.readSeq) {
    return;
  }
  const isStatus = reply !== null && typeof reply === 'object';
  if (!isStatus || !Number.isInteger(reply.brightness)) {
    pixelsShowHost(pixelsWindow, hostControls, null);
    return;
  }
  pixelsShowHost(pixelsWindow, hostControls, reply);
}

// Sends the level a slider was let go at. A host that does not take it is
// marked not available, unless a newer release is already on its way.
async function pixelsSaveLevel(pixelsWindow, hostControls, level) {
  hostControls.saveSeq += 1;
  const sequence = hostControls.saveSeq;
  const reply = await sendCmd('leds', `${PIXELS_SET_CMD}_${level}`, hostControls.name);
  if (pixelsWindow.closed === true || sequence !== hostControls.saveSeq) {
    return;
  }
  if (reply !== PIXELS_SET_OK) {
    pixelsShowHost(pixelsWindow, hostControls, null);
  }
}
