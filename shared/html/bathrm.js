// Automate Website - bathroom
//
// The bathroom's button grid and everything behind it: the brightness and
// mode menus, and the Settings window for the light's motion sensor.
// main.js lists BATHROOM_CONFIG among its rooms, draws the grid, and calls
// closeBathroomSettings() when the page is taken down.

import { registerListeners, gridButtonTemplate, dropdownButtonTemplate,
  sendCmd, cleanString, isNumeric, menuRow, promptWindow,
  onButtons } from './utils.js';
import { openDeviceMenu, inputMenuRows, removeDropdowns } from './dropdowns.js';
import ToggleSwitch from './switch.js';

// What main.js needs to know about the room. key names its API commands
// (bthrstatus, bthron, bthroff). There is no amp in this room, so its grid
// carries no volume buttons and it has no volume target.
export const BATHROOM_CONFIG = {
  key: 'bthr',
  name: 'Bathroom',
  icon: 'fad fa-bath',
  voltarget: null,
  volmax: null,
  template: bathroomGridTemplate
};

// the two menus, named once for their buttons, spinners and rows
const BTHR_BRIGHTNESS_MENU = 'bthrbrightmenu';
const BTHR_MODE_MENU = 'bthrmodemenu';

// one device, the BathroomLight Pico (bthr-light1): its dimmer, its light
// mode, and the Settings window for its motion sensor
function bathroomGridTemplate() {
  return `
<div>
  ${dropdownButtonTemplate({
    buttonId: 'btn-bthr-bright',
    icon: 'fad fa-sun',
    label: 'Brightness',
    textId: `${BTHR_BRIGHTNESS_MENU}-text`,
    spinnerId: `${BTHR_BRIGHTNESS_MENU}-spinner`,
    menuId: BTHR_BRIGHTNESS_MENU
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-bthr-mode',
    icon: 'fad fa-walking',
    label: 'Mode',
    textId: `${BTHR_MODE_MENU}-text`,
    spinnerId: `${BTHR_MODE_MENU}-spinner`,
    menuId: BTHR_MODE_MENU
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-bthr-settings',
    icon: 'fad fa-cog',
    label: 'Settings'
  })}
</div>`;
}
registerListeners({
  'btn-bthr-bright':   () => showBathroomBrightness(),
  'btn-bthr-mode':     () => showBathroomMode(),
  'btn-bthr-settings': () => bathroomSettingsPrompt()
});

//// bathroom ////
//
// The bathroom's one device is the BathroomLight Pico (bthr-light1). Every
// command reaches it through the API as
//   /api?action=main&arg=bthrxmit&var=<command>
// and an error comes back as an ordinary string with HTTP 200, so the text is
// what says whether a reply is one (see the Bathroom API Reference).

const BTHR_TARGET = 'bthrxmit';
const BTHR_DEVICE = 'bthr-light1';

// what bthrReplyError() answers for each kind of failure
const BTHR_OFFLINE = 'Light offline';
const BTHR_REFUSED = 'Value out of range';
const BTHR_NO_REPLY = 'No reply from server';
const BTHR_CMD_ERROR = 'Command error';

// checked in this order: the first two are both "commonAPIRequest" replies
const BTHR_OFFLINE_PREFIX = 'commonAPIRequest exception';
const BTHR_REFUSED_PREFIX = 'commonAPIRequest: invalid response';
const BTHR_ERROR_PREFIXES = ['bthrXmit:', 'commonAPIRequest', 'executeMain:'];

// '' for a reply that is not an error, or a short line saying what went wrong
function bthrReplyError(reply) {
  if (reply === null || reply === undefined) {
    return BTHR_NO_REPLY; // sendCmd() could not reach the API at all
  }
  const text = String(reply);
  if (text.startsWith(BTHR_OFFLINE_PREFIX)) {
    return BTHR_OFFLINE;
  }
  if (text.startsWith(BTHR_REFUSED_PREFIX)) {
    return BTHR_REFUSED;
  }
  for (const prefix of BTHR_ERROR_PREFIXES) {
    if (text.startsWith(prefix)) {
      return BTHR_CMD_ERROR;
    }
  }
  return '';
}

// The light serves one request at a time, so the Settings window's requests
// join one queue and reach it one after another rather than all at once.
// sendCmd() never rejects (it answers null), so nothing breaks the chain.
let bthrQueue = Promise.resolve();

// Queues one command for the light and answers a promise of its reply.
function bthrRequest(command) {
  const running = bthrQueue.then(() => sendCmd('main', BTHR_TARGET, command));
  bthrQueue = running.catch(() => null);
  return running;
}

// The light modes, in the shape inputMenuRows() takes: code is what
// bthr-light1_mode answers with while that mode is the one in force.
const BATHROOM_MODES = [
  { code: '2', cmd: `${BTHR_DEVICE}_mode_set_2`, label: 'Stay On' },
  { code: '1', cmd: `${BTHR_DEVICE}_mode_set_1`, label: 'Automatic' },
  { code: '0', cmd: `${BTHR_DEVICE}_mode_set_0`, label: 'Keep Off' }
];

// The one row a bathroom menu shows when the light did not give a usable
// answer: black for a light that is offline, as the other menus show it, and
// red for anything else.
function bathroomErrorRow(menuId, reply) {
  let label = bthrReplyError(reply);
  if (label === '') {
    label = 'Error';
  }
  let indicatorType = 'redind';
  if (label === BTHR_OFFLINE) {
    indicatorType = 'blkind';
  }
  return menuRow(`${menuId}-error`, indicatorType, label);
}

// Brightness: one slider, set from bthr-light1. That reads 1-255 whether the
// light is on or off: on, the level it is at (or fading to); off -- Auto with
// the room empty, or Off -- the level it will come on at, which is also what
// a level set while it is off changes.
function showBathroomBrightness() {
  const menuId = BTHR_BRIGHTNESS_MENU;
  openDeviceMenu({
    menu: menuId,
    target: BTHR_TARGET,
    cmd: BTHR_DEVICE,
    textId: `${menuId}-text`,
    spinnerId: `${menuId}-spinner`,
    build: (reply) => {
      if (isNumeric(reply) !== true) {
        return bathroomErrorRow(menuId, reply);
      }
      return menuRow(`${reply}~${BTHR_TARGET}~${BTHR_DEVICE}`, 'slider', '');
    }
  });
}

// Mode: Always On / Auto / Off, green on the one in force
function showBathroomMode() {
  const menuId = BTHR_MODE_MENU;
  openDeviceMenu({
    menu: menuId,
    target: BTHR_TARGET,
    cmd: `${BTHR_DEVICE}_mode`,
    textId: `${menuId}-text`,
    spinnerId: `${menuId}-spinner`,
    build: (reply) => {
      if (isNumeric(reply) !== true) {
        return bathroomErrorRow(menuId, reply);
      }
      return inputMenuRows(BTHR_TARGET, reply, BATHROOM_MODES);
    }
  });
}

/////// Bathroom Settings Window ///////
//
// The light's motion sensor settings as sliders and a switch, and its log and
// sensor read-outs in a log box that takes the settings' place. The window's
// state lives on App.bathroomSettings while it is open, so the page teardown
// can close it.

// bthr-light1_config answers min~max~presence~timeout~mode; `field` is each
// setting's place in it, and `key` is its name in the _set command
const BTHR_CONFIG_FIELDS = 5;
const BTHR_SETTINGS = [
  { key: 'timeout', field: 3, kind: 'delay', label: 'Off delay', min: 10, max: 3600 }, // in seconds
  { key: 'presence', field: 2, kind: 'switch', label: 'Presence hold' },
  { key: 'rangemin', field: 0, kind: 'range', label: 'Nearest detection', min: 0, max: 762, partner: 'rangemax' }, // in cm
  { key: 'rangemax', field: 1, kind: 'range', label: 'Farthest detection', min: 0, max: 762, partner: 'rangemin' } // in cm
];

// The off delay runs 6 s to an hour, too wide for a plain slider: the short
// delays that matter most would be a few pixels each. The slider runs 0 to
// BTHR_DELAY_STEPS and the delay grows with the cube of it, so short delays
// move about a second at a time and long ones in bigger steps. A delay
// already set past an hour shows as it is, with the slider at its end.
const BTHR_DELAY_STEPS = 1000;
const BTHR_DELAY_POWER = 3;

const BTHR_TITLE_SETTINGS = 'Bathroom Light';
const BTHR_TITLE_LOG = 'Light Log';
const BTHR_TITLE_SENSOR = 'Sensor Status';
const BTHR_DISABLED_CLASS = 'is-disabled';
// A change, or what went wrong, is shown in place of the heading for this
// long, and then the heading comes back.
const BTHR_MESSAGE_MS = 3000;
const BTHR_TITLE_ERROR_CLASS = 'bthr__title--error';
// on the heading while a message stands in its place: smaller and grey
const BTHR_TITLE_MESSAGE_CLASS = 'bthr__title--message';
const BTHR_WIDE_CLASS = 'bthr__prompt--wide';
const BTHR_PROMPT_ID = 'bthr__prompt';
// the size of the window's six icon buttons
const BTHR_BUTTON_CLASS = 'button_left_txtwin';
// the size the presence switch is drawn at
const BTHR_SWITCH_WIDTH = '75px';
const BTHR_SWITCH_HEIGHT = '35px';

// Opens the Settings window and fills it from the light. A second press of
// Settings while it is open does nothing.
function bathroomSettingsPrompt() {
  if (document.getElementById(BTHR_PROMPT_ID)) {
    return;
  }
  removeDropdowns();
  let settingsMarkup = '';
  BTHR_SETTINGS.forEach(setting => {
    settingsMarkup += bthrSettingHTML(setting);
  });
  const promptElement = promptWindow(BTHR_PROMPT_ID, `
    <div class="bthr__title" id="bthr__title"></div>
    <div id="bthr__settings">
      ${settingsMarkup}
      <div class="popup-actions bthr__actions">
        ${gridButtonTemplate({
          buttonId: 'bthr__btn-close',
          icon: 'fad fa-times-octagon',
          buttonClass: BTHR_BUTTON_CLASS
        })}
        ${gridButtonTemplate({
          buttonId: 'bthr__btn-log',
          icon: 'fad fa-terminal',
          buttonClass: BTHR_BUTTON_CLASS
        })}
        ${gridButtonTemplate({
          buttonId: 'bthr__btn-sensor',
          icon: 'fad fa-broadcast-tower',
          buttonClass: BTHR_BUTTON_CLASS
        })}
      </div>
    </div>
    <div class="popup-ctn bthr__log" id="bthr__log">
      <textarea readonly class="scroll-bar-style" id="bthr__logbox" spellcheck="false"></textarea>
      <div class="popup-actions bthr__actions">
        ${gridButtonTemplate({
          buttonId: 'bthr__btn-back',
          icon: 'fad fa-arrow-left',
          buttonClass: BTHR_BUTTON_CLASS
        })}
        ${gridButtonTemplate({
          buttonId: 'bthr__btn-refresh',
          icon: 'fad fa-sync-alt',
          buttonClass: BTHR_BUTTON_CLASS
        })}
        ${gridButtonTemplate({
          buttonId: 'bthr__btn-logclose',
          icon: 'fad fa-times-octagon',
          buttonClass: BTHR_BUTTON_CLASS
        })}
      </div>
    </div>
  `);
  const settingsWindow = {
    win: promptElement,
    title: document.getElementById('bthr__title'),
    panel: 'settings',     // which of the two is showing
    titleText: BTHR_TITLE_SETTINGS, // the heading a message gives way to
    messageTimer: null,    // brings the heading back after a message
    settingsPanel: document.getElementById('bthr__settings'),
    logPanel: document.getElementById('bthr__log'),
    logBox: document.getElementById('bthr__logbox'),
    presenceSwitch: null,
    controls: {},     // per setting key: its box, slider and value label
    values: {},    // per setting key: what the light last confirmed
    // per setting key, bumped on each save: only the newest save of a slider
    // moves it, so an older reply cannot pull it back under a newer release
    saveSeq: {},
    // bumped per log read, so Back -- or a second read -- drops one in the air
    logSeq: 0,
    // the read-out the log box is showing, which Refresh reads again
    logView: null,
    // set on close; a reply landing afterwards is dropped
    closed: false
  };
  App.bathroomSettings = settingsWindow;
  BTHR_SETTINGS.forEach(setting => bthrBindSetting(settingsWindow, setting));
  onButtons(promptElement, (button) => {
    switch (button.id) {
      case 'bthr__btn-close':
      case 'bthr__btn-logclose':
        closeBathroomSettings();
        break;
      case 'bthr__btn-log':
        bthrShowLog(settingsWindow, `${BTHR_DEVICE}_log`, BTHR_TITLE_LOG, true);
        break;
      case 'bthr__btn-sensor':
        bthrShowLog(settingsWindow, `${BTHR_DEVICE}_sensor`, BTHR_TITLE_SENSOR, false);
        break;
      case 'bthr__btn-refresh':
        bthrRefreshLog(settingsWindow);
        break;
      case 'bthr__btn-back':
        settingsWindow.logSeq += 1; // a read still in the air is no longer wanted
        bthrShowPanel(settingsWindow, 'settings', BTHR_TITLE_SETTINGS);
        break;
    }
  });
  bthrShowPanel(settingsWindow, 'settings', BTHR_TITLE_SETTINGS);
  bthrLoadConfig(settingsWindow);
}

// Takes the window down. The switch holds pointer listeners on window while
// it is dragged, so it is destroyed rather than just dropped with the markup.
export function closeBathroomSettings() {
  const settingsWindow = App.bathroomSettings;
  if (!settingsWindow) {
    return;
  }
  App.bathroomSettings = null;
  settingsWindow.closed = true;
  clearTimeout(settingsWindow.messageTimer);
  settingsWindow.messageTimer = null;
  settingsWindow.logSeq += 1;
  if (settingsWindow.presenceSwitch) {
    settingsWindow.presenceSwitch.destroy();
    settingsWindow.presenceSwitch = null;
  }
  settingsWindow.win.remove();
}

// the id of one setting's box, which its slider, value and switch ids extend
function bthrSettingId(setting) {
  return `bthr__set-${setting.key}`;
}

// one setting's box: a label and its value over a slider, or a label and a
// switch on one line. Labels are fixed strings from BTHR_SETTINGS.
function bthrSettingHTML(setting) {
  const settingId = bthrSettingId(setting);
  if (setting.kind === 'switch') {
    return `
      <div class="bthr__setting bthr__setting--switch" id="${settingId}">
        <span class="bthr__label">${setting.label}</span>
        <span id="${settingId}-switch"></span>
      </div>`;
  }
  let sliderMin = setting.min;
  let sliderMax = setting.max;
  if (setting.kind === 'delay') {
    sliderMin = 0;
    sliderMax = BTHR_DELAY_STEPS;
  }
  return `
    <div class="bthr__setting" id="${settingId}">
      <div class="bthr__setting-head">
        <span class="bthr__label">${setting.label}</span>
        <span class="bthr__value" id="${settingId}-value">--</span>
      </div>
      <div class="slider-wide_container bthr__slider">
        <input type="range" class="slider-wide" id="${settingId}-slider"
          min="${sliderMin}" max="${sliderMax}" step="1" value="${sliderMin}">
      </div>
    </div>`;
}

// finds a setting's elements and wires its slider or switch
function bthrBindSetting(settingsWindow, setting) {
  const settingId = bthrSettingId(setting);
  const settingControls = {
    box: document.getElementById(settingId),
    slider: null,
    value: null
  };
  settingsWindow.controls[setting.key] = settingControls;
  if (setting.kind === 'switch') {
    const switchCommand = `${setting.key}-`;
    settingsWindow.presenceSwitch = new ToggleSwitch(document.getElementById(`${settingId}-switch`), {
      id: `bthr-${setting.key}`,
      checked: false,
      width: BTHR_SWITCH_WIDTH,
      height: BTHR_SWITCH_HEIGHT,
      cmdHost: BTHR_TARGET,
      command: switchCommand,
      onToggle: (payload) => {
        let wanted = 0;
        if (payload.cmd === `${switchCommand}on`) {
          wanted = 1;
        }
        bthrSaveSetting(settingsWindow, setting, wanted);
      }
    });
    settingsWindow.presenceSwitch.activate();
    return;
  }
  settingControls.slider = document.getElementById(`${settingId}-slider`);
  settingControls.value = document.getElementById(`${settingId}-value`);
  // the label follows the drag; the light hears only the release
  settingControls.slider.addEventListener('input', () => {
    bthrSliderMoved(settingsWindow, setting);
  });
  settingControls.slider.addEventListener('change', () => {
    bthrSliderReleased(settingsWindow, setting);
  });
}

// swaps between the settings and the log box, and the heading with them
function bthrShowPanel(settingsWindow, name, title) {
  let settingsDisplay = 'none';
  let logDisplay = 'flex';
  if (name === 'settings') {
    settingsDisplay = 'block';
    logDisplay = 'none';
  }
  settingsWindow.settingsPanel.style.display = settingsDisplay;
  settingsWindow.logPanel.style.display = logDisplay;
  // the log and the sensor status run wide, so the window doubles for them
  settingsWindow.win.classList.toggle(BTHR_WIDE_CLASS, name !== 'settings');
  settingsWindow.panel = name;
  settingsWindow.titleText = title;
  // a message still showing belonged to the panel being left
  bthrRestoreTitle(settingsWindow);
}

// Reads one of the light's text read-outs into the log box. The log is oldest
// first, so it opens at the bottom on the newest line; the sensor status
// opens at the top, where its first lines are.
async function bthrShowLog(settingsWindow, command, title, scrollToEnd) {
  settingsWindow.logView = { command, title, scrollToEnd };
  bthrShowPanel(settingsWindow, 'log', title);
  settingsWindow.logBox.value = 'Loading...';
  settingsWindow.logBox.scrollTop = 0;
  settingsWindow.logSeq += 1;
  const sequence = settingsWindow.logSeq;
  const reply = await bthrRequest(command);
  if (settingsWindow.closed === true || sequence !== settingsWindow.logSeq) {
    return;
  }
  const error = bthrReplyError(reply);
  if (error !== '') {
    settingsWindow.logBox.value = error;
    return;
  }
  settingsWindow.logBox.value = String(reply).replace(/\r\n/g, '\n');
  if (scrollToEnd === true) {
    settingsWindow.logBox.scrollTop = settingsWindow.logBox.scrollHeight;
  }
}

// Reads the read-out on show again, for its latest lines. The read before it,
// if it is still in the air, is dropped by bthrShowLog()'s sequence count.
function bthrRefreshLog(settingsWindow) {
  const logView = settingsWindow.logView;
  if (!logView) {
    return;
  }
  bthrShowLog(settingsWindow, logView.command, logView.title, logView.scrollToEnd);
}

// puts the heading back, dropping any message showing in its place
function bthrRestoreTitle(settingsWindow) {
  clearTimeout(settingsWindow.messageTimer);
  settingsWindow.messageTimer = null;
  settingsWindow.title.textContent = settingsWindow.titleText;
  settingsWindow.title.classList.remove(BTHR_TITLE_ERROR_CLASS);
  settingsWindow.title.classList.remove(BTHR_TITLE_MESSAGE_CLASS);
}

// Shows a message in place of the settings heading -- 'Off delay: 6 min 8 s'
// -- with an error drawn in red. It gives the heading back after
// BTHR_MESSAGE_MS, or, held, stays until the next message. Messages are for
// the settings: one landing while the log box is showing is left unshown.
function bthrShowMessage(settingsWindow, text, isError = false, held = false) {
  // a closed window has no heading left to write on, and its timer is gone
  if (settingsWindow.closed === true) {
    return;
  }
  if (settingsWindow.panel !== 'settings') {
    return;
  }
  clearTimeout(settingsWindow.messageTimer);
  settingsWindow.messageTimer = null;
  settingsWindow.title.textContent = text;
  settingsWindow.title.classList.add(BTHR_TITLE_MESSAGE_CLASS);
  settingsWindow.title.classList.toggle(BTHR_TITLE_ERROR_CLASS, isError === true);
  if (held === true) {
    return;
  }
  settingsWindow.messageTimer = setTimeout(() => bthrRestoreTitle(settingsWindow), BTHR_MESSAGE_MS);
}

// greys the settings out while there is nothing read from the light to
// change -- before the first read lands, or when it failed
function bthrSetReady(settingsWindow, ready) {
  BTHR_SETTINGS.forEach(setting => {
    const settingControls = settingsWindow.controls[setting.key];
    settingControls.box.classList.toggle(BTHR_DISABLED_CLASS, ready !== true);
    if (settingControls.slider) {
      settingControls.slider.disabled = ready !== true;
    }
  });
}

// Fills every setting from the one bthr-light1_config read, rather than one
// read per setting.
async function bthrLoadConfig(settingsWindow) {
  bthrSetReady(settingsWindow, false);
  bthrShowMessage(settingsWindow, 'Reading settings...', false, true);
  const reply = await bthrRequest(`${BTHR_DEVICE}_config`);
  if (settingsWindow.closed === true) {
    return;
  }
  let error = bthrReplyError(reply);
  let fields = [];
  if (error === '') {
    fields = cleanString(String(reply)).split('~');
    const complete = fields.length === BTHR_CONFIG_FIELDS;
    if (!complete || !fields.every(field => isNumeric(field))) {
      error = 'Could not read settings';
    }
  }
  if (error !== '') {
    // held: the settings stay greyed out, and this says why
    bthrShowMessage(settingsWindow, error, true, true);
    return;
  }
  BTHR_SETTINGS.forEach(setting => {
    const value = Number(fields[setting.field]);
    settingsWindow.values[setting.key] = value;
    bthrShowValue(settingsWindow, setting, value);
  });
  bthrSetReady(settingsWindow, true);
  if (settingsWindow.panel === 'settings') {
    bthrRestoreTitle(settingsWindow);
  }
}

// what a value reads as beside its slider
function bthrFormatValue(setting, value) {
  if (setting.kind === 'delay') {
    return bthrFormatDelay(value);
  }
  if (setting.kind === 'switch') {
    if (value === 1) {
      return 'on';
    }
    return 'off';
  }
  return bthrFormatDistance(value);
}

// The light counts its detection distances in centimetres, and so do the
// sliders; only the read-out is in feet. 100 -> "3.3 ft"
const BTHR_CM_PER_FOOT = 30.48;

function bthrFormatDistance(centimetres) {
  const feet = centimetres / BTHR_CM_PER_FOOT;
  return `${feet.toFixed(1)} ft`;
}

// 45 -> "45 s", 330 -> "5 min 30 s", 7260 -> "2 h 1 min"
function bthrFormatDelay(seconds) {
  const secondsPerMinute = 60;
  const minutesPerHour = 60;
  if (seconds < secondsPerMinute) {
    return `${seconds} s`;
  }
  const totalMinutes = Math.floor(seconds / secondsPerMinute);
  const leftSeconds = seconds % secondsPerMinute;
  if (totalMinutes < minutesPerHour) {
    if (leftSeconds === 0) {
      return `${totalMinutes} min`;
    }
    return `${totalMinutes} min ${leftSeconds} s`;
  }
  const hours = Math.floor(totalMinutes / minutesPerHour);
  const leftMinutes = totalMinutes % minutesPerHour;
  if (leftMinutes === 0) {
    return `${hours} h`;
  }
  return `${hours} h ${leftMinutes} min`;
}

// slider position -> off delay in seconds
function bthrDelayFromSlider(setting, position) {
  const sliderFraction = position / BTHR_DELAY_STEPS;
  const curvedFraction = Math.pow(sliderFraction, BTHR_DELAY_POWER);
  const delaySpan = setting.max - setting.min;
  const secondsAboveMinimum = delaySpan * curvedFraction;
  const delaySeconds = setting.min + secondsAboveMinimum;
  return Math.round(delaySeconds);
}

// off delay in seconds -> slider position
function bthrSliderFromDelay(setting, seconds) {
  const delaySpan = setting.max - setting.min;
  const secondsAboveMinimum = seconds - setting.min;
  // a delay set outside the slider's range stands at the nearer end of it
  let delayFraction = secondsAboveMinimum / delaySpan;
  if (delayFraction < 0) {
    delayFraction = 0;
  }
  if (delayFraction > 1) {
    delayFraction = 1;
  }
  const inversePower = 1 / BTHR_DELAY_POWER;
  const sliderFraction = Math.pow(delayFraction, inversePower);
  const sliderPosition = sliderFraction * BTHR_DELAY_STEPS;
  return Math.round(sliderPosition);
}

// the value the slider stands at now, as the light counts it
function bthrSliderValue(settingsWindow, setting) {
  const position = Number(settingsWindow.controls[setting.key].slider.value);
  if (setting.kind === 'delay') {
    return bthrDelayFromSlider(setting, position);
  }
  return position;
}

// Where a distance slider may go: the nearest has to stay below the farthest
// and the farthest above the nearest, or the light refuses it. Measured
// against where the other slider stands now, so a release on one that is
// still on its way to the light counts.
function bthrRangeLimits(settingsWindow, setting) {
  const limits = { low: setting.min, high: setting.max };
  const partnerSetting = BTHR_SETTINGS.find(other => other.key === setting.partner);
  if (!partnerSetting) {
    return limits;
  }
  const partnerValue = bthrSliderValue(settingsWindow, partnerSetting);
  if (setting.key === 'rangemin') {
    const highestBelowPartner = partnerValue - 1;
    limits.high = Math.min(setting.max, highestBelowPartner);
  } else {
    const lowestAbovePartner = partnerValue + 1;
    limits.low = Math.max(setting.min, lowestAbovePartner);
  }
  return limits;
}

// the slider's value, held inside what the light will take
function bthrClampedSliderValue(settingsWindow, setting) {
  let value = bthrSliderValue(settingsWindow, setting);
  if (setting.kind !== 'range') {
    return value;
  }
  const limits = bthrRangeLimits(settingsWindow, setting);
  if (value > limits.high) {
    value = limits.high;
  }
  if (value < limits.low) {
    value = limits.low;
  }
  // the thumb stops at the limit rather than running past it
  settingsWindow.controls[setting.key].slider.value = value;
  return value;
}

// puts a value on its slider or switch and beside it
function bthrShowValue(settingsWindow, setting, value) {
  const settingControls = settingsWindow.controls[setting.key];
  if (setting.kind === 'switch') {
    if (settingsWindow.presenceSwitch) {
      settingsWindow.presenceSwitch.checkbox.checked = value === 1;
    }
    return;
  }
  if (setting.kind === 'delay') {
    settingControls.slider.value = bthrSliderFromDelay(setting, value);
  } else {
    settingControls.slider.value = value;
  }
  settingControls.value.textContent = bthrFormatValue(setting, value);
}

// while a slider is dragged: only the value beside it follows
function bthrSliderMoved(settingsWindow, setting) {
  const value = bthrClampedSliderValue(settingsWindow, setting);
  settingsWindow.controls[setting.key].value.textContent = bthrFormatValue(setting, value);
}

// on release: the value is sent, unless the slider is back where it was
function bthrSliderReleased(settingsWindow, setting) {
  const value = bthrClampedSliderValue(settingsWindow, setting);
  if (value === settingsWindow.values[setting.key]) {
    bthrShowValue(settingsWindow, setting, value); // back where it was; nothing to send
    return;
  }
  bthrSaveSetting(settingsWindow, setting, value);
}

// Sends one setting and shows what the light took. A refusal puts the control
// back and re-reads every setting, as the API reference asks.
async function bthrSaveSetting(settingsWindow, setting, value) {
  let sequence = 1;
  if (settingsWindow.saveSeq[setting.key]) {
    sequence = settingsWindow.saveSeq[setting.key] + 1;
  }
  settingsWindow.saveSeq[setting.key] = sequence;
  const reply = await bthrRequest(`${BTHR_DEVICE}_${setting.key}_set_${value}`);
  if (settingsWindow.closed === true) {
    return;
  }
  let error = bthrReplyError(reply);
  let answer = '';
  if (error === '') {
    answer = cleanString(String(reply));
  }
  if (error === '' && isNumeric(answer) !== true) {
    error = BTHR_CMD_ERROR;
  }
  const newest = sequence === settingsWindow.saveSeq[setting.key];
  if (error !== '') {
    if (newest) {
      bthrShowValue(settingsWindow, setting, settingsWindow.values[setting.key]);
    }
    if (error === BTHR_REFUSED) {
      await bthrLoadConfig(settingsWindow);
      // the window was closed while the re-read was in the air: a message
      // now would start a timer on a window that is no longer there
      if (settingsWindow.closed === true) {
        return;
      }
    }
    // after the re-read, so the reason is what is left on screen
    bthrShowMessage(settingsWindow, `${setting.label}: ${error}`, true);
    return;
  }
  const confirmed = Number(answer);
  settingsWindow.values[setting.key] = confirmed;
  if (newest) {
    bthrShowValue(settingsWindow, setting, confirmed);
    bthrShowMessage(settingsWindow, `${setting.label}: ${bthrFormatValue(setting, confirmed)}`);
  }
}
