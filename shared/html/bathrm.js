// Automate - bathroom: button grid, the Accent and Ambient buttons' menus, and the Settings
// window for the lights' motion sensor.

import { registerListeners, gridButtonTemplate, dropdownButtonTemplate,
  cleanString, isNumeric, menuRow, indicator, promptWindow, onButtons, picoReplyError,
  picoRequestQueue, parsePicoNumber, readPicoLog,
  PICO_REFUSED } from './utils.js';
import { openDeviceMenu, inputMenuRows, removeDropdowns, spinnerIds } from './dropdowns.js';

// The room's config. key names its API commands (bthrstatus, bthron, bthroff). No amp, so
// no volume target. climate null: no sensor, so the room is not in the temperature window's
// Source menu.
export const BATHROOM_CONFIG = {
  key: 'bthr',
  name: 'Bathroom',
  icon: 'fad fa-bath',
  climate: null,
  template: bathroomGridTemplate
};

// the two menus
const BTHR_ACCENT_MENU = 'bthraccentmenu';
const BTHR_MAIN_MENU = 'bthrmainmenu';

// the room's button grid: the BathroomLight Pico's two lights (Accent, and Ambient for the main
// light) and its Settings window
function bathroomGridTemplate() {
  return `
<div>
  ${dropdownButtonTemplate({
    buttonId: 'btn-bthr-accent',
    icon: 'fad fa-lightbulb-on',
    label: 'Accent',
    ...spinnerIds(BTHR_ACCENT_MENU),
    menuId: BTHR_ACCENT_MENU
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-bthr-main',
    icon: 'fad fa-light-ceiling',
    label: 'Ambient',
    ...spinnerIds(BTHR_MAIN_MENU),
    menuId: BTHR_MAIN_MENU
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-bthr-settings',
    icon: 'fad fa-cog',
    label: 'Settings'
  })}
</div>`;
}
registerListeners({
  'btn-bthr-accent':   () => showAccentMenu(),
  'btn-bthr-main':     () => showMainMenu(),
  'btn-bthr-settings': () => bathroomSettingsPrompt()
});

//// bathroom ////

// Every command reaches the BathroomLight Pico as /api?action=main&arg=bthrxmit&var=<command>.

const BTHR_TARGET = 'bthrxmit';
// the Pico, whose own commands (config, log) carry its name
const BTHR_DEVICE = 'bthr-light1';
// its two lights: the accent (LED0) dims, the main light (LED1) is on or off
const BTHR_ACCENT = `${BTHR_DEVICE}_accent`;
const BTHR_MAIN = `${BTHR_DEVICE}_main`;

// bthrReplyError()'s answer for a light that did not answer
const BTHR_OFFLINE = 'Light offline';
const BTHR_XMIT_PREFIX = 'bthrXmit:';

// '' for a good reply, or a short line saying what went wrong
function bthrReplyError(reply) {
  return picoReplyError(reply, BTHR_XMIT_PREFIX, BTHR_OFFLINE);
}

// the light serves one request at a time
const bthrRequest = picoRequestQueue(BTHR_TARGET);

// what a light's _state answers while it is on
const BTHR_LIGHT_ON = '1';
// the mode that turns a light on and off by motion
const BTHR_MODE_AUTOMATIC = '1';
// the mode that keeps a light off
const BTHR_MODE_KEEP_OFF = '0';

// A light's modes for inputMenuRows(): code is what <light>_mode answers.
// bathroomModes(BTHR_MAIN)[0] -> { code: '2', cmd: 'bthr-light1_main_mode_set_2', label: 'Stay On' }
function bathroomModes(light) {
  return [
    { code: '2', cmd: `${light}_mode_set_2`, label: 'Stay On' },
    { code: '1', cmd: `${light}_mode_set_1`, label: 'Automatic' },
    { code: '0', cmd: `${light}_mode_set_0`, label: 'Keep Off' }
  ];
}

// The row a bathroom menu shows for an unusable answer: black when offline, else red.
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

// A menu read's fields, or none when the request failed (null):
// bathroomReplyFields('180~1~0') -> ['180', '1', '0']
function bathroomReplyFields(reply) {
  if (typeof reply !== 'string') {
    return [];
  }
  return reply.split('~');
}

// A light's mode rows, the mode in force marked: green, but yellow for Keep Off and while
// Automatic holds the light off (nobody within its distance).
// bathroomModeRows(BTHR_MAIN, '1', '0') -> Automatic yellow;
// bathroomModeRows(BTHR_MAIN, '1', '1') -> Automatic green; bathroomModeRows(BTHR_MAIN, '0', '0')
// -> Keep Off yellow.
function bathroomModeRows(light, mode, state) {
  const automaticAndOff = mode === BTHR_MODE_AUTOMATIC && state !== BTHR_LIGHT_ON;
  const keepOff = mode === BTHR_MODE_KEEP_OFF;
  let selectedIndicator = indicator(true);
  if (automaticAndOff || keepOff) {
    selectedIndicator = 'ylwind';
  }
  return inputMenuRows(BTHR_TARGET, mode, bathroomModes(light), selectedIndicator);
}

// Accent: the brightness slider over the mode rows. One request reads all three, as
// level~mode~state; the level is 1-255 on or off, the level the light is at or will come
// on at, and the state 1 while it is on.
function showAccentMenu() {
  openDeviceMenu({
    menu: BTHR_ACCENT_MENU,
    target: BTHR_TARGET,
    cmd: `${BTHR_ACCENT}_menu`,
    build: (reply) => {
      const [level, mode, state] = bathroomReplyFields(reply);
      if (isNumeric(level) !== true || isNumeric(mode) !== true || isNumeric(state) !== true) {
        return bathroomErrorRow(BTHR_ACCENT_MENU, reply);
      }
      const sliderRow = menuRow(`${level}~${BTHR_TARGET}~${BTHR_ACCENT}`, 'slider', '');
      const modeRows = bathroomModeRows(BTHR_ACCENT, mode, state);
      return sliderRow + modeRows;
    }
  });
}

// Ambient: the main light's mode rows only, as it does not dim. One request reads its mode
// and state, as mode~state.
function showMainMenu() {
  openDeviceMenu({
    menu: BTHR_MAIN_MENU,
    target: BTHR_TARGET,
    cmd: `${BTHR_MAIN}_menu`,
    build: (reply) => {
      const [mode, state] = bathroomReplyFields(reply);
      if (isNumeric(mode) !== true || isNumeric(state) !== true) {
        return bathroomErrorRow(BTHR_MAIN_MENU, reply);
      }
      return bathroomModeRows(BTHR_MAIN, mode, state);
    }
  });
}

/////// Bathroom Settings Window ///////

// bthr-light1_config answers
// accent range~accent delay~accent mode~main range~main delay~main mode, as
// "150~30~1~30~15~1"; field is each setting's place in it, key its name in the _set
// command (bthr-light1_<key>_set_<n>). Delays are in seconds, ranges (how near someone
// must come to turn the light on) in cm. group is the box the setting is drawn in, label
// its name there, and name how a change or error names it in the heading.
const BTHR_CONFIG_FIELDS = 6;
const BTHR_GROUPS = [
  { id: 'main', title: 'Ambient Light' },
  { id: 'accent', title: 'Accent Lights' }
];
const BTHR_SETTINGS = [
  { key: 'main_range', group: 'main', field: 3, kind: 'range', label: 'Turns on within',
    name: 'Ambient distance', min: 10, max: 305 },
  { key: 'main_delay', group: 'main', field: 4, kind: 'delay', label: 'Off delay',
    name: 'Ambient off delay', min: 1, max: 3600 },
  { key: 'accent_range', group: 'accent', field: 0, kind: 'range', label: 'Turns on within',
    name: 'Accent distance', min: 10, max: 305 },
  { key: 'accent_delay', group: 'accent', field: 1, kind: 'delay', label: 'Off delay',
    name: 'Accent off delay', min: 1, max: 3600 }
];

// Every slider runs 0-BTHR_SLIDER_STEPS and maps to its setting on an exponential curve,
// value = min * (max / min) ^ (position / steps), so each step changes the value by the same
// share of it: short delays and near distances move a second or an inch at a time, long ones
// in larger steps. Halfway is 60 s on the off delay (1 s - 1 h) and 22 in on the distance
// (4 in - 10 ft). A value set outside min-max shows as it is, with the slider at the nearer end.
const BTHR_SLIDER_STEPS = 1000;

const BTHR_TITLE_SETTINGS = 'Bathroom';
// The log box's read-outs: what reads each, its name over the box (capitals by CSS) and
// icon, also its button's, and whether it opens at its end.
const BTHR_LOG_VIEW = {
  command: `${BTHR_DEVICE}_log`,
  title: 'Light Log',
  icon: 'fad fa-terminal',
  scrollToEnd: true
};
const BTHR_SENSOR_VIEW = {
  command: `${BTHR_DEVICE}_sensor`,
  title: 'Sensor Status',
  icon: 'fad fa-broadcast-tower',
  scrollToEnd: false
};
const BTHR_DISABLED_CLASS = 'is-disabled';
// how long a change or error stands in place of the heading
const BTHR_MESSAGE_MS = 3000;
const BTHR_TITLE_ERROR_CLASS = 'bthr__title--error';
// on the heading while a message stands in its place
const BTHR_TITLE_MESSAGE_CLASS = 'bthr__title--message';
const BTHR_WIDE_CLASS = 'bthr__prompt--wide';
const BTHR_PROMPT_ID = 'bthr__prompt';
// the window's six icon buttons
const BTHR_BUTTON_CLASS = 'button_left_txtwin';

// Opens the Settings window and fills it from the light.
function bathroomSettingsPrompt() {
  if (document.getElementById(BTHR_PROMPT_ID)) {
    return;
  }
  removeDropdowns();
  let settingsMarkup = '';
  BTHR_GROUPS.forEach(group => {
    settingsMarkup += bthrGroupHTML(group);
  });
  const promptElement = promptWindow(BTHR_PROMPT_ID, `
    <div class="bthr__title" id="bthr__title"></div>
    <div id="bthr__settings">
      ${settingsMarkup}
      <div class="popup-actions bthr__actions">
        ${gridButtonTemplate({
          buttonId: 'bthr__btn-close',
          icon: 'fas fa-times',
          buttonClass: BTHR_BUTTON_CLASS
        })}
        ${gridButtonTemplate({
          buttonId: 'bthr__btn-log',
          icon: BTHR_LOG_VIEW.icon,
          buttonClass: BTHR_BUTTON_CLASS
        })}
        ${gridButtonTemplate({
          buttonId: 'bthr__btn-sensor',
          icon: BTHR_SENSOR_VIEW.icon,
          buttonClass: BTHR_BUTTON_CLASS
        })}
      </div>
    </div>
    <div class="popup-ctn bthr__log" id="bthr__log">
      <div class="btn-group-label bthr__log-title"><i id="bthr__log-icon"></i><span id="bthr__log-title"></span></div>
      <textarea readonly class="scroll-bar-style pico-logbox" id="bthr__logbox" spellcheck="false"></textarea>
      <div class="popup-actions bthr__actions">
        ${gridButtonTemplate({
          buttonId: 'bthr__btn-logclose',
          icon: 'fas fa-times',
          buttonClass: BTHR_BUTTON_CLASS
        })}
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
      </div>
    </div>
  `);
  const settingsWindow = {
    win: promptElement,
    title: document.getElementById('bthr__title'),
    panel: 'settings',     // which of the two is showing
    messageTimer: null,
    settingsPanel: document.getElementById('bthr__settings'),
    logPanel: document.getElementById('bthr__log'),
    logBox: document.getElementById('bthr__logbox'),
    logTitle: document.getElementById('bthr__log-title'),
    logIcon: document.getElementById('bthr__log-icon'),
    controls: {},     // per setting key: its box, slider and value label
    values: {},       // per setting key: what the light last confirmed
    // per setting key, bumped on each save, so an older reply cannot pull a slider back
    saveSeq: {},
    // bumped per log read, so Back or a second read drops one in the air
    logSeq: 0,
    // the read-out on show, which Refresh reads again
    logView: null,
    // set on close; a late reply is dropped
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
        bthrShowLog(settingsWindow, BTHR_LOG_VIEW);
        break;
      case 'bthr__btn-sensor':
        bthrShowLog(settingsWindow, BTHR_SENSOR_VIEW);
        break;
      case 'bthr__btn-refresh':
        bthrRefreshLog(settingsWindow);
        break;
      case 'bthr__btn-back':
        settingsWindow.logSeq += 1; // a read in the air is no longer wanted
        bthrShowPanel(settingsWindow, 'settings');
        break;
    }
  });
  bthrShowPanel(settingsWindow, 'settings');
  bthrLoadConfig(settingsWindow);
}

// Takes the window down; a reply still in the air is dropped.
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
  settingsWindow.win.remove();
}

// the id of one setting's box, which its slider and value ids extend
function bthrSettingId(setting) {
  return `bthr__set-${setting.key}`;
}

// One group's box: its title over its settings.
function bthrGroupHTML(group) {
  let itemsMarkup = '';
  BTHR_SETTINGS.forEach(setting => {
    if (setting.group === group.id) {
      itemsMarkup += bthrSettingHTML(setting);
    }
  });
  return `
    <div class="bthr__setting" id="bthr__group-${group.id}">
      <div class="bthr__group-title">${group.title}</div>
      ${itemsMarkup}
    </div>`;
}

// One setting in its group's box: a label and value over a slider.
function bthrSettingHTML(setting) {
  const settingId = bthrSettingId(setting);
  return `
    <div class="bthr__item" id="${settingId}">
      <div class="bthr__setting-head">
        <span class="bthr__label">${setting.label}</span>
        <span class="bthr__value" id="${settingId}-value">--</span>
      </div>
      <div class="slider-wide_container bthr__slider">
        <input type="range" class="slider-wide" id="${settingId}-slider"
          min="0" max="${BTHR_SLIDER_STEPS}" step="1" value="0">
      </div>
    </div>`;
}

// finds a setting's elements and wires its slider
function bthrBindSetting(settingsWindow, setting) {
  const settingId = bthrSettingId(setting);
  const settingControls = {
    box: document.getElementById(settingId),
    slider: document.getElementById(`${settingId}-slider`),
    value: document.getElementById(`${settingId}-value`)
  };
  settingsWindow.controls[setting.key] = settingControls;
  // the label follows the drag; the light hears only the release
  settingControls.slider.addEventListener('input', () => {
    bthrSliderMoved(settingsWindow, setting);
  });
  settingControls.slider.addEventListener('change', () => {
    bthrSliderReleased(settingsWindow, setting);
  });
}

// Swaps between the settings ('settings') and the log box ('log'). The settings show
// under the heading; the log box carries its read-out's own title instead.
function bthrShowPanel(settingsWindow, name) {
  let settingsDisplay = 'none';
  let logDisplay = 'flex';
  let titleDisplay = 'none';
  if (name === 'settings') {
    settingsDisplay = 'block';
    logDisplay = 'none';
    titleDisplay = 'block';
  }
  settingsWindow.settingsPanel.style.display = settingsDisplay;
  settingsWindow.logPanel.style.display = logDisplay;
  settingsWindow.title.style.display = titleDisplay;
  // the log and sensor status run wide
  settingsWindow.win.classList.toggle(BTHR_WIDE_CLASS, name !== 'settings');
  settingsWindow.panel = name;
  // a message showing belonged to the panel being left
  bthrRestoreTitle(settingsWindow);
}

// Reads one of the light's read-outs (BTHR_LOG_VIEW, BTHR_SENSOR_VIEW) into the log box,
// under its name and icon. The log opens on its newest line, the sensor status at the top.
function bthrShowLog(settingsWindow, logView) {
  settingsWindow.logView = logView;
  bthrShowPanel(settingsWindow, 'log');
  settingsWindow.logTitle.textContent = logView.title;
  settingsWindow.logIcon.className = logView.icon;
  settingsWindow.logBox.value = 'Loading...';
  settingsWindow.logBox.scrollTop = 0;
  readPicoLog(settingsWindow, bthrRequest, bthrReplyError, logView.command, logView.scrollToEnd);
}

// reads the read-out on show again
function bthrRefreshLog(settingsWindow) {
  bthrShowLog(settingsWindow, settingsWindow.logView);
}

// puts the heading back, dropping any message in its place
function bthrRestoreTitle(settingsWindow) {
  clearTimeout(settingsWindow.messageTimer);
  settingsWindow.messageTimer = null;
  settingsWindow.title.textContent = BTHR_TITLE_SETTINGS;
  settingsWindow.title.classList.remove(BTHR_TITLE_ERROR_CLASS);
  settingsWindow.title.classList.remove(BTHR_TITLE_MESSAGE_CLASS);
}

// Shows a message in place of the settings heading: 'Off delay: 6 min 8 s', an error in
// red. It gives way after BTHR_MESSAGE_MS, or held, to the next message. A closed window,
// or the log box showing, leaves it unshown.
function bthrShowMessage(settingsWindow, text, isError = false, held = false) {
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

// greys the settings out until a read from the light lands
function bthrSetReady(settingsWindow, ready) {
  BTHR_SETTINGS.forEach(setting => {
    const settingControls = settingsWindow.controls[setting.key];
    settingControls.box.classList.toggle(BTHR_DISABLED_CLASS, ready !== true);
    settingControls.slider.disabled = ready !== true;
  });
}

// Fills every setting from the one bthr-light1_config read.
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
  return bthrFormatDistance(value);
}

// Distances are cm on the light and the sliders; only the read-out is in feet, or in
// inches under 3 ft, where the main light's short reach sits.
const BTHR_CM_PER_INCH = 2.54;
const BTHR_INCHES_PER_FOOT = 12;
const BTHR_INCHES_SHOWN_BELOW_FEET = 3;

// 30 -> "12 in", 150 -> "4.9 ft"
function bthrFormatDistance(centimetres) {
  const inches = centimetres / BTHR_CM_PER_INCH;
  const feet = inches / BTHR_INCHES_PER_FOOT;
  if (feet < BTHR_INCHES_SHOWN_BELOW_FEET) {
    const wholeInches = Math.round(inches);
    return `${wholeInches} in`;
  }
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

// slider position -> the setting's value: bthrValueFromSlider(offDelay, 500) -> 60 (s)
function bthrValueFromSlider(setting, position) {
  const sliderFraction = position / BTHR_SLIDER_STEPS;
  const rangeRatio = setting.max / setting.min;
  const growth = Math.pow(rangeRatio, sliderFraction);
  const value = setting.min * growth;
  return Math.round(value);
}

// the setting's value -> slider position: bthrSliderFromValue(offDelay, 60) -> 500
function bthrSliderFromValue(setting, value) {
  // a value outside the slider's range stands at the nearer end
  let boundedValue = value;
  if (boundedValue < setting.min) {
    boundedValue = setting.min;
  }
  if (boundedValue > setting.max) {
    boundedValue = setting.max;
  }
  const rangeRatio = setting.max / setting.min;
  const valueRatio = boundedValue / setting.min;
  const sliderFraction = Math.log(valueRatio) / Math.log(rangeRatio);
  const sliderPosition = sliderFraction * BTHR_SLIDER_STEPS;
  return Math.round(sliderPosition);
}

// the value the slider stands at now, as the light counts it
function bthrSliderValue(settingsWindow, setting) {
  const position = Number(settingsWindow.controls[setting.key].slider.value);
  return bthrValueFromSlider(setting, position);
}

// puts a value on its slider and beside it
function bthrShowValue(settingsWindow, setting, value) {
  const settingControls = settingsWindow.controls[setting.key];
  settingControls.slider.value = bthrSliderFromValue(setting, value);
  settingControls.value.textContent = bthrFormatValue(setting, value);
}

// while dragged: only the value beside the slider follows
function bthrSliderMoved(settingsWindow, setting) {
  const value = bthrSliderValue(settingsWindow, setting);
  settingsWindow.controls[setting.key].value.textContent = bthrFormatValue(setting, value);
}

// on release: sends the value, unless the slider is back where it was
function bthrSliderReleased(settingsWindow, setting) {
  const value = bthrSliderValue(settingsWindow, setting);
  if (value === settingsWindow.values[setting.key]) {
    bthrShowValue(settingsWindow, setting, value);
    return;
  }
  bthrSaveSetting(settingsWindow, setting, value);
}

// Sends one setting and shows what the light took. A refusal puts the control back and
// re-reads every setting.
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
  const { answer, error } = parsePicoNumber(reply, bthrReplyError);
  const newest = sequence === settingsWindow.saveSeq[setting.key];
  if (error !== '') {
    if (newest) {
      bthrShowValue(settingsWindow, setting, settingsWindow.values[setting.key]);
    }
    if (error === PICO_REFUSED) {
      await bthrLoadConfig(settingsWindow);
    }
    // after the re-read, so the reason is what stays on screen
    bthrShowMessage(settingsWindow, `${setting.name}: ${error}`, true);
    return;
  }
  const confirmed = Number(answer);
  settingsWindow.values[setting.key] = confirmed;
  if (newest) {
    bthrShowValue(settingsWindow, setting, confirmed);
    bthrShowMessage(settingsWindow, `${setting.name}: ${bthrFormatValue(setting, confirmed)}`);
  }
}
