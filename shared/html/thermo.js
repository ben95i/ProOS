// Automate - temperature and thermostat window: two thermometers for a room (main.js picks
// it, and its config's climate names the source), the SmartThermo's setpoint display, mode
// and blower boxes, and the settings view (text colour, brightness, status and log).
// openThermoWindow(), refreshThermoWindow() and closeThermoWindow(); state is on
// App.thermoWindow while open.

import { registerListeners, gridButtonTemplate, dropdownButtonTemplate,
  promptWindow, sendCmd, cleanString, isNumeric, makeElement,
  requestThrottle, picoReplyError, picoRequestQueue, parsePicoNumber, readPicoLog,
  PICO_REFUSED, PICO_NO_REPLY } from './utils.js';
import { openDropdown, removeDropdowns, beginMenuRequest,
  menuRequestCurrent } from './dropdowns.js';
import { reportNotConnected, reportException } from './error.js';

//// thermostat API ////

// Commands reach the Pico through lrxmit (lr-thermo1<command>) on one queue.

const THERMO_TARGET = 'lrxmit';
const THERMO_DEVICE = 'lr-thermo1';
const THERMO_XMIT_PREFIX = 'lrXmit:';
const THERMO_OFFLINE = 'Offline';
const thermoRequest = picoRequestQueue(THERMO_TARGET);

// '' for a good reply, or a short line saying what went wrong
function thermoReplyError(reply) {
  return picoReplyError(reply, THERMO_XMIT_PREFIX, THERMO_OFFLINE);
}

// lr-thermo1 answers mode~fan~setpoint~temperature~humidity~sensor~W1~Y1~G~running;
// the places of the fields the window reads
const STATUS_FIELD_COUNT = 10;
const STATUS_FIELDS = {
  mode: 0,
  fan: 1,
  setpoint: 2,
  sensor: 5,
  fanRelay: 8,
  running: 9
};

// the running field while the furnace or AC runs, and a relay field while closed
const RUNNING_HEAT = '1';
const RUNNING_COOL = '2';
const RELAY_CLOSED = '1';

// the sensor states that are a fault: 2 startup lockout, 3 stale, 4 failed
const SENSOR_FAULTS = ['2', '3', '4'];

// The setpoint range in whole °F. A press shows at once and is sent at most this often.
const SETPOINT_MIN = 50;
const SETPOINT_MAX = 80;
const SETPOINT_STEP = 1;
const SETPOINT_SEND_MS = 600;

// the backlight slider's send rate while dragged, the release always sent
const BACKLIGHT_SEND_MS = 250;

// The Mode and Fan menus. code is the setting's status field while in force, running the
// running state that lights the mode's icon, and isRunning(fields) whether what the
// setting controls is running.
const THERMO_SETTINGS = {
  mode: {
    command: 'mode',
    what: 'mode',
    field: STATUS_FIELDS.mode,
    isRunning: (fields) => {
      const mode = thermoChoice('mode', fields[STATUS_FIELDS.mode]);
      return mode.running !== null && fields[STATUS_FIELDS.running] === mode.running;
    },
    menuId: 'thermo__mode-menu',
    // the one refusal the thermostat gives a valid mode
    refusedText: 'The thermostat refused it: cool is refused while its sensor has failed.',
    choices: [
      { code: '1', label: 'Heat', icon: 'fad fa-fire-alt', running: RUNNING_HEAT },
      { code: '2', label: 'Cool', icon: 'fad fa-snowflake', running: RUNNING_COOL },
      { code: '0', label: 'Off', icon: 'fad fa-power-off', running: null }
    ]
  },
  fan: {
    command: 'fan',
    what: 'fan mode',
    field: STATUS_FIELDS.fan,
    isRunning: (fields) => fields[STATUS_FIELDS.fanRelay] === RELAY_CLOSED,
    menuId: 'thermo__fan-menu',
    refusedText: 'The thermostat refused it.',
    choices: [
      { code: '0', label: 'Auto' },
      { code: '1', label: 'On' }
    ]
  }
};

// The display colours the swatches offer, 8 of the firmware's 15 (DISPLAY_TEXT_COLORS in
// display.h). code is the thermostat's answer while set.
const THERMO_TEXT_COLORS = [
  { code: '0', label: 'White', color: '#ffffff' },
  { code: '2', label: 'Red', color: '#ff0000' },
  { code: '3', label: 'Orange', color: '#ff7900' },
  { code: '5', label: 'Yellow', color: '#ffff00' },
  { code: '7', label: 'Green', color: '#00ff00' },
  { code: '9', label: 'Cyan', color: '#00ffff' },
  { code: '11', label: 'Blue', color: '#296dff' },
  { code: '13', label: 'Magenta', color: '#ff00ff' }
];
const THERMO_READ_REFUSED = 'The thermostat refused it: its firmware may be older than this command.';

//// thermometers ////

// the range each thermometer's tube covers
const TEMP_MIN = 25;
const TEMP_MAX = 100;
const HUMIDITY_MIN = 5;
const HUMIDITY_MAX = 100;

// a thermometer with no reading
const NO_READING = '--';

//// window ////

const THERMO_PROMPT_ID = 'temp__prompt';
// the Refresh icon, turning while the window reads
const THERMO_REFRESH_ICON = 'thermo__refresh-icon';
const THERMO_SPIN_CLASS = 'fa-spin';
// the Brightness circle's icon, turning while the level is read
const THERMO_BRIGHTNESS_ICON = 'thermo__brightness-icon';
const THERMO_BACKLIGHT_MENU = 'thermo__backlight-menu';
const THERMO_VIEW_MENU = 'thermo__view-menu';
// the swatch of the colour set, drawn larger (colorprompt.css)
const THERMO_SWATCH_ACTIVE_CLASS = 'active-swatch';
// The log box's read-outs: what reads each, its name (capitals by CSS) and icon, and
// whether it opens at its end.
const THERMO_STATUS_VIEW = {
  command: `${THERMO_DEVICE}_status`,
  title: 'Status',
  icon: 'fad fa-terminal',
  scrollToEnd: false
};
const THERMO_LOG_VIEW = {
  command: `${THERMO_DEVICE}_log`,
  title: 'System Logs',
  icon: 'fad fa-scroll',
  scrollToEnd: true
};
// on whichever of the two panels is not showing
const THERMO_HIDDEN_CLASS = 'thermo__panel--hidden';
// the icon drawn lit while its equipment runs
const THERMO_ICON_CLASS = 'thermo__icon';
const THERMO_FAULT_TEXT = 'Fault';
const THERMO_ERROR_TEXT = 'Error';
// the dot of the menu row in force: green while what it sets runs, yellow while idle
const DOT_GREEN_CLASS = 'ind_dot_green';
const DOT_YELLOW_CLASS = 'ind_dot_yellow';

// the id of a choice's menu row
function thermoChoiceRowId(kind, code) {
  return `thermo__row-${kind}-${code}`;
}

// the id of the dot in a choice's row
function thermoChoiceDotId(kind, code) {
  return `thermo__dot-${kind}-${code}`;
}

// One menu row per choice, with its dot: thermoChoiceRowsTemplate('fan')
function thermoChoiceRowsTemplate(kind) {
  let rows = '';
  THERMO_SETTINGS[kind].choices.forEach(choice => {
    rows += `
      <a id="${thermoChoiceRowId(kind, choice.code)}">${choice.label}<span class="ind_dot" id="${thermoChoiceDotId(kind, choice.code)}"></span></a>`;
  });
  return rows;
}

// The mode's or blower's box: its icon over its name, opening its menu.
// thermoStateButtonTemplate('fan', 'thermo__icon fad fa-fan')
function thermoStateButtonTemplate(kind, iconClass) {
  return dropdownButtonTemplate({
    buttonId: `thermo__btn-${kind}`,
    icon: iconClass,
    iconId: `thermo__${kind}-icon`,
    label: `<span class="thermo__caption" id="thermo__${kind}-text"></span>`,
    buttonClass: 'thermo__state',
    menuId: THERMO_SETTINGS[kind].menuId,
    menuClass: 'dd-button',
    menuContent: thermoChoiceRowsTemplate(kind)
  });
}

// the id of a text colour's swatch
function thermoSwatchId(code) {
  return `thermo__swatch-${code}`;
}

// one swatch per text colour, in the colour prompt's style
function thermoSwatchesTemplate() {
  let swatches = '';
  THERMO_TEXT_COLORS.forEach(textColor => {
    swatches += `
        <div class="color-swatch" id="${thermoSwatchId(textColor.code)}" title="${textColor.label}" style="background-color: ${textColor.color}"></div>`;
  });
  return swatches;
}

// The Display panel: the colours, then the Brightness circle and its slider.
function thermoDisplayPanelTemplate() {
  return `
    <fieldset class="btn-group thermo__display">
      <div class="btn-group-label"><i class="fad fa-tv"></i><span>Display</span></div>
      <div class="thermo__swatches">${thermoSwatchesTemplate()}
        ${dropdownButtonTemplate({
          buttonId: 'thermo__btn-brightness',
          icon: 'fad fa-sun',
          iconId: THERMO_BRIGHTNESS_ICON,
          buttonClass: 'thermo__brightness',
          menuId: THERMO_BACKLIGHT_MENU,
          menuContent: `
            <a class="slider-wide_container no-hover">
              <input type="range" class="slider-wide" id="thermo__backlight-slider"
                min="0" max="255" step="1" value="0">
            </a>`
        })}
      </div>
    </fieldset>`;
}

// The log box's title, also a button: its menu's one row is the other read-out.
function thermoViewTitleTemplate() {
  return dropdownButtonTemplate({
    buttonId: 'thermo__btn-view',
    icon: THERMO_STATUS_VIEW.icon,
    iconId: 'thermo__view-icon',
    label: '<span id="thermo__log-title"></span>',
    buttonClass: 'thermo__view',
    menuId: THERMO_VIEW_MENU,
    menuClass: 'dd-button',
    menuContent: `
      <a id="thermo__row-view"><i id="thermo__row-view-icon"></i><span id="thermo__row-view-text"></span></a>`
  });
}

// The window: thermometers and thermostat over three buttons, and the settings view that
// takes their place.
function thermoWindowTemplate() {
  return `
  <div class="thermo__main" id="thermo__main">
    <fieldset class="btn-group thermo__climate">
      <div class="btn-group-label"><span>Humidity/Temperature</span></div>
      <div id="temp_therm_grid">
        <div class="thermometer">
          <div class="temperature" id="thermo__2" data-value="${NO_READING}"></div>
        </div>
        <div class="thermometer">
          <div class="temperature" id="thermo__1" data-value="${NO_READING}"></div>
        </div>
      </div>
      <div class="thermo__source" id="thermo__source"></div>
    </fieldset>
    <fieldset class="btn-group thermo__controls">
      <div class="btn-group-label"><span>Thermostat</span></div>
      <div class="thermo__lcd" id="thermo__lcd"></div>
      ${thermoStateButtonTemplate('mode', THERMO_ICON_CLASS)}
      ${thermoStateButtonTemplate('fan', `${THERMO_ICON_CLASS} fad fa-fan`)}
      ${gridButtonTemplate({
        buttonId: 'thermo__btn-up',
        icon: 'fad fa-chevron-up'
      })}
      ${gridButtonTemplate({
        buttonId: 'thermo__btn-down',
        icon: 'fad fa-chevron-down'
      })}
    </fieldset>
    <div class="thermo__actions">
      ${gridButtonTemplate({
        buttonId: 'temp__btn-close',
        icon: 'fad fa-times-octagon'
      })}
      ${gridButtonTemplate({
        buttonId: 'thermo__btn-refresh',
        icon: 'fad fa-sync-alt',
        iconId: THERMO_REFRESH_ICON
      })}
      ${gridButtonTemplate({
        buttonId: 'thermo__btn-settings',
        icon: 'fad fa-cog'
      })}
    </div>
  </div>
  <div class="thermo__logview ${THERMO_HIDDEN_CLASS}" id="thermo__log">
    <div class="btn-group-label thermo__setup-title"><i class="fad fa-thermometer-half"></i><span>Thermostat Setup</span></div>
    ${thermoDisplayPanelTemplate()}
    <fieldset class="btn-group thermo__log">
      <div class="btn-group-label">${thermoViewTitleTemplate()}</div>
      <div class="popup-ctn">
        <textarea readonly class="scroll-bar-style" id="thermo__logbox" spellcheck="false"></textarea>
      </div>
    </fieldset>
    <div class="thermo__actions">
      ${gridButtonTemplate({
        buttonId: 'thermo__btn-logclose',
        icon: 'fad fa-times-octagon'
      })}
      ${gridButtonTemplate({
        buttonId: 'thermo__btn-back',
        icon: 'fad fa-arrow-left'
      })}
      ${gridButtonTemplate({
        buttonId: 'thermo__btn-logrefresh',
        icon: 'fad fa-sync-alt'
      })}
    </div>
  </div>`;
}
registerListeners({
  'temp__btn-close':        () => closeThermoWindow(),
  'thermo__btn-logclose':   () => closeThermoWindow(),
  'thermo__btn-up':         () => thermoStepSetpoint(App.thermoWindow, SETPOINT_STEP),
  'thermo__btn-down':       () => thermoStepSetpoint(App.thermoWindow, -SETPOINT_STEP),
  'thermo__btn-mode':       () => thermoOpenChoiceMenu(App.thermoWindow, 'mode'),
  'thermo__btn-fan':        () => thermoOpenChoiceMenu(App.thermoWindow, 'fan'),
  'thermo__btn-refresh':    () => thermoRefresh(App.thermoWindow),
  'thermo__btn-settings':   () => thermoOpenSettings(App.thermoWindow),
  'thermo__btn-brightness': () => thermoOpenBrightnessMenu(App.thermoWindow),
  'thermo__btn-view':       () => openDropdown(THERMO_VIEW_MENU),
  'thermo__row-view':       () => thermoSwitchView(App.thermoWindow),
  'thermo__btn-logrefresh': () => thermoRefreshSettings(App.thermoWindow),
  'thermo__btn-back':       () => thermoBack(App.thermoWindow)
});
registerListeners(thermoChoiceListeners());
registerListeners(thermoSwatchListeners());

// one listener per choice row of the Mode and Fan menus
function thermoChoiceListeners() {
  const listeners = {};
  for (const kind of Object.keys(THERMO_SETTINGS)) {
    THERMO_SETTINGS[kind].choices.forEach(choice => {
      listeners[thermoChoiceRowId(kind, choice.code)] = () => thermoSaveChoice(App.thermoWindow, kind, choice.code);
    });
  }
  return listeners;
}

// one listener per swatch
function thermoSwatchListeners() {
  const listeners = {};
  THERMO_TEXT_COLORS.forEach(textColor => {
    listeners[thermoSwatchId(textColor.code)] = () => thermoSaveColor(App.thermoWindow, textColor);
  });
  return listeners;
}

// Opens the window and reads the thermostat and the room's climate into it.
// pickClimateRoom() answers the room config to read, asked again on every refresh.
export function openThermoWindow(pickClimateRoom) {
  if (App.thermoWindow) {
    return;
  }
  removeDropdowns();
  const promptElement = promptWindow(THERMO_PROMPT_ID, thermoWindowTemplate());
  const thermoWindow = {
    win: promptElement,
    pickClimateRoom: pickClimateRoom,
    mainPanel: document.getElementById('thermo__main'),
    logPanel: document.getElementById('thermo__log'),
    brightnessIcon: document.getElementById(THERMO_BRIGHTNESS_ICON),
    backlightSlider: document.getElementById('thermo__backlight-slider'),
    logTitle: document.getElementById('thermo__log-title'),
    viewIcon: document.getElementById('thermo__view-icon'),
    viewRowIcon: document.getElementById('thermo__row-view-icon'),
    viewRowText: document.getElementById('thermo__row-view-text'),
    logBox: document.getElementById('thermo__logbox'),
    lcd: document.getElementById('thermo__lcd'),
    source: document.getElementById('thermo__source'),
    temperature: document.getElementById('thermo__1'),
    humidity: document.getElementById('thermo__2'),
    modeIcon: document.getElementById('thermo__mode-icon'),
    modeText: document.getElementById('thermo__mode-text'),
    fanIcon: document.getElementById('thermo__fan-icon'),
    fanText: document.getElementById('thermo__fan-text'),
    upButton: document.getElementById('thermo__btn-up'),
    refreshIcon: document.getElementById(THERMO_REFRESH_ICON),
    downButton: document.getElementById('thermo__btn-down'),
    status: null,           // the fields of the last status read, or null
    setpoint: null,         // on the display: the thermostat's, or a press of ours
    setpointPending: false, // a press the thermostat has not confirmed yet
    sentSetpoint: null,     // the last setpoint sent
    sendSetpoint: null,     // throttled thermoSaveSetpoint()
    sendBacklight: null,    // throttled thermoSaveBacklight()
    readSeq: 0,             // bumped per status read, so only the newest is drawn
    logSeq: 0,              // bumped per log read, so Back or a second read drops one
    colorSeq: 0,            // bumped per colour read or press, so only the newest marks a swatch
    logView: null,          // the read-out in the log box, which Refresh reads again
    closed: false           // set on close; a late reply is dropped
  };
  thermoWindow.sendSetpoint = requestThrottle((value) => {
    thermoSaveSetpoint(thermoWindow, value);
  }, SETPOINT_SEND_MS);
  thermoWindow.sendBacklight = requestThrottle((value) => {
    thermoSaveBacklight(thermoWindow, value);
  }, BACKLIGHT_SEND_MS);
  thermoWireBacklightSlider(thermoWindow);
  App.thermoWindow = thermoWindow;
  thermoShowStatus(thermoWindow, null);
  thermoRefresh(thermoWindow);
}

// Reads an open window again, for a room switch.
export function refreshThermoWindow() {
  if (App.thermoWindow) {
    thermoRefresh(App.thermoWindow);
  }
}

// Takes the window down; a setpoint press still waiting is sent rather than lost.
export function closeThermoWindow() {
  const thermoWindow = App.thermoWindow;
  if (!thermoWindow) {
    return;
  }
  App.thermoWindow = null;
  thermoWindow.closed = true;
  thermoWindow.sendSetpoint.cancel();
  thermoWindow.sendBacklight.cancel();
  const unsentPress = thermoWindow.setpointPending === true &&
    thermoWindow.setpoint !== thermoWindow.sentSetpoint;
  if (unsentPress) {
    thermoRequest(`${THERMO_DEVICE}_target_set_${thermoWindow.setpoint}`);
  }
  thermoWindow.logSeq += 1;
  thermoWindow.win.remove();
}

// Swaps between the panels and the settings view, which stand in one place, closing any
// open menu.
function thermoShowPanel(thermoWindow, name) {
  removeDropdowns();
  const showingLog = name === 'log';
  thermoWindow.mainPanel.classList.toggle(THERMO_HIDDEN_CLASS, showingLog);
  thermoWindow.logPanel.classList.toggle(THERMO_HIDDEN_CLASS, !showingLog);
}

//// reading ////

// Reads the thermostat's status and the room's climate together and shows both. When the
// climate is the thermostat's own, one read serves both.
async function thermoRefresh(thermoWindow) {
  thermoWindow.readSeq += 1;
  const sequence = thermoWindow.readSeq;
  const climateRoom = thermoWindow.pickClimateRoom();
  const climate = climateRoom.climate;
  thermoWindow.source.textContent = `Source: ${climateRoom.name}`;
  thermoWindow.refreshIcon.classList.add(THERMO_SPIN_CLASS);
  const statusRead = thermoRequest(THERMO_DEVICE);
  let climateRead = statusRead;
  const climateIsThermostat = climate.target === THERMO_TARGET && climate.cmd === THERMO_DEVICE;
  if (climateIsThermostat === false) {
    climateRead = sendCmd('main', climate.target, climate.cmd);
  }
  const [statusReply, climateReply] = await Promise.all([statusRead, climateRead]);
  // a newer read stops the Refresh icon when it lands
  if (thermoWindow.closed === true || sequence !== thermoWindow.readSeq) {
    return;
  }
  thermoWindow.refreshIcon.classList.remove(THERMO_SPIN_CLASS);
  thermoShowStatus(thermoWindow, statusReply);
  thermoShowClimate(thermoWindow, climate, climateReply);
}

// a thermometer reading: "71.3" or "72", where the thermostat answers "NA" with none
function isReading(value) {
  return typeof value === 'string' && /^-?\d+(\.\d+)?$/.test(value);
}

// The temperature and humidity out of a climate reply, at climate.fields' places:
// "72~41" with [0, 1] -> { temperature: '72', humidity: '41' }. null when either is missing.
function thermoClimateValues(climate, reply) {
  if (typeof reply !== 'string') {
    return null;
  }
  const fields = cleanString(reply).split('~');
  const [temperatureField, humidityField] = climate.fields;
  const temperature = fields[temperatureField];
  const humidity = fields[humidityField];
  if (!isReading(temperature) || !isReading(humidity)) {
    return null;
  }
  return { temperature: temperature, humidity: humidity };
}

// how much of the tube one reading fills, as a percentage of its range
function thermometerFill(value, minValue, maxValue) {
  let held = Number(value);
  if (held >= maxValue) {
    held = maxValue;
  }
  if (held <= minValue) {
    held = minValue;
  }
  const aboveFloor = held - minValue;
  const fullRange = maxValue - minValue;
  return (aboveFloor / fullRange) * 100;
}

// puts one reading on its thermometer, or empties it for no reading
function thermometerShow(element, value, unit, minValue, maxValue) {
  if (value === null) {
    element.style.height = '0%';
    element.dataset.value = NO_READING;
    return;
  }
  const fillPercent = thermometerFill(value, minValue, maxValue);
  element.style.height = `${fillPercent}%`;
  element.dataset.value = `${value}${unit}`;
}

// fills both thermometers from a climate reply
function thermoShowClimate(thermoWindow, climate, reply) {
  const values = thermoClimateValues(climate, reply);
  let temperature = null;
  let humidity = null;
  if (values !== null) {
    temperature = values.temperature;
    humidity = values.humidity;
  }
  thermometerShow(thermoWindow.temperature, temperature, '°F', TEMP_MIN, TEMP_MAX);
  thermometerShow(thermoWindow.humidity, humidity, '%', HUMIDITY_MIN, HUMIDITY_MAX);
}

// The status reply's fields, or null with what went wrong:
// "1~0~70~69.4~41.0~0~1~0~0~1" -> { fields: ['1', '0', '70', …], error: '' }
function thermoParseStatus(reply) {
  const error = thermoReplyError(reply);
  if (error !== '') {
    return { fields: null, error: error };
  }
  const fields = cleanString(String(reply)).split('~');
  const complete = fields.length === STATUS_FIELD_COUNT;
  if (!complete || !isNumeric(fields[STATUS_FIELDS.setpoint])) {
    return { fields: null, error: THERMO_ERROR_TEXT };
  }
  return { fields: fields, error: '' };
}

// Draws the thermostat panel from a status reply; with none usable the display reads "--"
// and the setpoint buttons are off.
function thermoShowStatus(thermoWindow, reply) {
  let parsed = { fields: null, error: '' };
  if (reply !== null) {
    parsed = thermoParseStatus(reply);
  }
  const fields = parsed.fields;
  thermoWindow.status = fields;
  thermoWindow.upButton.disabled = fields === null;
  thermoWindow.downButton.disabled = fields === null;
  if (fields === null) {
    if (thermoWindow.setpointPending === false) {
      thermoWindow.setpoint = null;
    }
    thermoShowSetpoint(thermoWindow);
    thermoShowIcon(thermoWindow.modeIcon, 'fad fa-thermometer-half', '');
    thermoWindow.modeText.textContent = parsed.error;
    thermoShowIcon(thermoWindow.fanIcon, 'fad fa-fan', '');
    thermoWindow.fanText.textContent = '';
    return;
  }
  if (thermoWindow.setpointPending === false) {
    thermoWindow.setpoint = Number(fields[STATUS_FIELDS.setpoint]);
  }
  thermoShowSetpoint(thermoWindow);
  thermoShowMode(thermoWindow, fields);
  thermoShowFan(thermoWindow, fields);
}

// the mode's icon and name, lit while its equipment runs
function thermoShowMode(thermoWindow, fields) {
  const mode = thermoChoice('mode', fields[STATUS_FIELDS.mode]);
  const running = fields[STATUS_FIELDS.running];
  let state = '';
  if (running === RUNNING_HEAT && mode.running === RUNNING_HEAT) {
    state = 'is-heat';
  } else if (running === RUNNING_COOL && mode.running === RUNNING_COOL) {
    state = 'is-cool';
  }
  let caption = mode.label;
  const sensorFault = SENSOR_FAULTS.includes(fields[STATUS_FIELDS.sensor]);
  if (sensorFault === true) {
    state = 'is-fault';
    caption = THERMO_FAULT_TEXT;
  }
  thermoShowIcon(thermoWindow.modeIcon, mode.icon, state);
  thermoWindow.modeText.textContent = caption;
}

// the blower's icon, turning while its relay is closed, and the fan mode
function thermoShowFan(thermoWindow, fields) {
  const fanMode = thermoChoice('fan', fields[STATUS_FIELDS.fan]);
  let state = '';
  if (fields[STATUS_FIELDS.fanRelay] === RELAY_CLOSED) {
    state = 'is-fan';
  }
  thermoShowIcon(thermoWindow.fanIcon, 'fad fa-fan', state);
  thermoWindow.fanText.textContent = fanMode.label;
}

// a setting's choice for a code, or a stand-in named by the code when it is not known
function thermoChoice(kind, code) {
  const choice = THERMO_SETTINGS[kind].choices.find(known => known.code === code);
  if (choice) {
    return choice;
  }
  return { code: code, label: code, icon: 'fad fa-question', running: null };
}

// sets an icon's glyph and state ('' for unlit); the blower's turns while lit
function thermoShowIcon(iconElement, iconClass, state) {
  iconElement.className = `${THERMO_ICON_CLASS} ${iconClass}`;
  if (state !== '') {
    iconElement.classList.add(state);
  }
  iconElement.classList.toggle('fa-spin', state === 'is-fan');
}

//// setpoint display ////

// 5 x 7 dot-matrix glyphs, one string per row, '#' lit. The degree sign is three wide.
const LCD_BLANK = ' ';
const LCD_GLYPHS = {
  ' ': ['.....', '.....', '.....', '.....', '.....', '.....', '.....'],
  '0': ['.###.', '#...#', '#..##', '#.#.#', '##..#', '#...#', '.###.'],
  '1': ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'],
  '2': ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'],
  '3': ['#####', '...#.', '..#..', '...#.', '....#', '#...#', '.###.'],
  '4': ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'],
  '5': ['#####', '#....', '####.', '....#', '....#', '#...#', '.###.'],
  '6': ['..##.', '.#...', '#....', '####.', '#...#', '#...#', '.###.'],
  '7': ['#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'],
  '8': ['.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'],
  '9': ['.###.', '#...#', '#...#', '.####', '....#', '...#.', '.##..'],
  '-': ['.....', '.....', '.....', '#####', '.....', '.....', '.....'],
  '°': ['.#.', '#.#', '.#.', '...', '...', '...', '...']
};

// Draws text on the display, one grid of dots per character; no glyph draws blank.
// lcdShow(lcd, '72°')
function lcdShow(lcdElement, text) {
  // every status read lands here, and a character is 35 dots to rebuild
  if (lcdElement.getAttribute('aria-label') === text) {
    return;
  }
  const glyphElements = [];
  for (const character of text) {
    let glyphRows = LCD_GLYPHS[character];
    if (!glyphRows) {
      glyphRows = LCD_GLYPHS[LCD_BLANK];
    }
    const glyphElement = makeElement('span', 'thermo__glyph');
    glyphElement.style.setProperty('--glyph-columns', glyphRows[0].length);
    for (const glyphRow of glyphRows) {
      for (const dot of glyphRow) {
        let dotClass = 'thermo__dot';
        if (dot === '#') {
          dotClass = 'thermo__dot is-lit';
        }
        glyphElement.appendChild(makeElement('span', dotClass));
      }
    }
    glyphElements.push(glyphElement);
  }
  lcdElement.replaceChildren(...glyphElements);
  lcdElement.setAttribute('aria-label', text);
}

// the setpoint on the display: "72°", or "--°" with none
function thermoShowSetpoint(thermoWindow) {
  let setpointText = NO_READING;
  if (thermoWindow.setpoint !== null) {
    setpointText = String(thermoWindow.setpoint);
  }
  lcdShow(thermoWindow.lcd, `${setpointText}°`);
}

//// changing settings ////

// Up or down a degree: shows at once, held in range, and the throttle sends it.
function thermoStepSetpoint(thermoWindow, step) {
  if (thermoWindow.setpoint === null) {
    return;
  }
  let wanted = thermoWindow.setpoint + step;
  if (wanted > SETPOINT_MAX) {
    wanted = SETPOINT_MAX;
  }
  if (wanted < SETPOINT_MIN) {
    wanted = SETPOINT_MIN;
  }
  if (wanted === thermoWindow.setpoint) {
    return;
  }
  thermoWindow.setpoint = wanted;
  thermoWindow.setpointPending = true;
  thermoShowSetpoint(thermoWindow);
  thermoWindow.sendSetpoint(wanted);
}

// Reports a setting the thermostat did not take; what is a gerund phrase. No reply at all
// was reported by sendCmd() already.
function thermoReportFailure(what, error, refusedText) {
  if (error === PICO_NO_REPLY) {
    return;
  }
  if (error === THERMO_OFFLINE) {
    reportNotConnected(what, 'The thermostat is offline.');
    return;
  }
  if (error === PICO_REFUSED) {
    reportException(what, refusedText);
    return;
  }
  reportException(what, `${error}.`);
}

// a one-number reply, parsed against the thermostat's errors
function thermoParseNumber(reply) {
  return parsePicoNumber(reply, thermoReplyError);
}

// Sends one setpoint. Its reply settles the display only when no press has come since; a
// refusal re-reads what the thermostat kept.
async function thermoSaveSetpoint(thermoWindow, value) {
  thermoWindow.sentSetpoint = value;
  const reply = await thermoRequest(`${THERMO_DEVICE}_target_set_${value}`);
  if (thermoWindow.closed === true) {
    return;
  }
  const { answer, error } = thermoParseNumber(reply);
  if (error !== '') {
    thermoReportFailure(`setting the thermostat to ${value}°F`, error,
      `The thermostat refused it: the setpoint runs ${SETPOINT_MIN} to ${SETPOINT_MAX}°F.`);
    thermoWindow.setpointPending = false;
    thermoRefresh(thermoWindow);
    return;
  }
  const confirmed = Number(answer);
  if (confirmed === thermoWindow.setpoint) {
    thermoWindow.setpointPending = false;
    thermoShowSetpoint(thermoWindow);
  }
}

// shuts a menu that is open, and answers whether it was
function thermoCloseIfOpen(menuId) {
  const menuElement = document.getElementById(menuId);
  if (menuElement.style.display !== 'block') {
    return false;
  }
  menuElement.style.display = 'none';
  return true;
}

// Mode or Fan: reads the thermostat first (Refresh icon turning), marks the choice in force,
// and opens the menu. A second press closes it; a press elsewhere during the read takes
// the menu token and the menu stays shut.
async function thermoOpenChoiceMenu(thermoWindow, kind) {
  const setting = THERMO_SETTINGS[kind];
  if (thermoCloseIfOpen(setting.menuId) === true) {
    return;
  }
  removeDropdowns();
  const token = beginMenuRequest();
  await thermoRefresh(thermoWindow);
  if (thermoWindow.closed === true || !menuRequestCurrent(token)) {
    return;
  }
  const fields = thermoWindow.status;
  let selectedCode = null;
  let running = false;
  if (fields !== null) {
    selectedCode = fields[setting.field];
    running = setting.isRunning(fields);
  }
  setting.choices.forEach(choice => {
    const dot = document.getElementById(thermoChoiceDotId(kind, choice.code));
    const selected = choice.code === selectedCode;
    dot.classList.toggle(DOT_GREEN_CLASS, selected && running);
    dot.classList.toggle(DOT_YELLOW_CLASS, selected && !running);
  });
  openDropdown(setting.menuId);
}

// A Mode or Fan row: sets the choice, then reads the status back.
async function thermoSaveChoice(thermoWindow, kind, code) {
  const setting = THERMO_SETTINGS[kind];
  const reply = await thermoRequest(`${THERMO_DEVICE}_${setting.command}_set_${code}`);
  if (thermoWindow.closed === true) {
    return;
  }
  const error = thermoReplyError(reply);
  if (error !== '') {
    const choice = thermoChoice(kind, code);
    thermoReportFailure(`setting the thermostat ${setting.what} to ${choice.label}`,
      error, setting.refusedText);
  }
  thermoRefresh(thermoWindow);
}

// Brightness: reads the level (the circle's icon turning), puts it on the slider and opens
// the menu. A second press closes it; a press elsewhere during the read, or a failed
// read, leaves it shut.
async function thermoOpenBrightnessMenu(thermoWindow) {
  if (thermoCloseIfOpen(THERMO_BACKLIGHT_MENU) === true) {
    return;
  }
  removeDropdowns();
  const token = beginMenuRequest();
  thermoWindow.brightnessIcon.classList.add(THERMO_SPIN_CLASS);
  const reply = await thermoRequest(`${THERMO_DEVICE}_backlight`);
  thermoWindow.brightnessIcon.classList.remove(THERMO_SPIN_CLASS);
  if (thermoWindow.closed === true || !menuRequestCurrent(token)) {
    return;
  }
  const { answer, error } = thermoParseNumber(reply);
  if (error !== '') {
    thermoReportFailure('reading the thermostat backlight level', error, THERMO_READ_REFUSED);
    return;
  }
  thermoWindow.backlightSlider.value = answer;
  openDropdown(THERMO_BACKLIGHT_MENU);
}

// the slider sends while dragged, throttled, and once more on release
function thermoWireBacklightSlider(thermoWindow) {
  const slider = thermoWindow.backlightSlider;
  slider.addEventListener('input', () => {
    thermoWindow.sendBacklight(slider.value);
  });
  slider.addEventListener('change', () => {
    thermoWindow.sendBacklight.cancel();
    thermoSaveBacklight(thermoWindow, slider.value);
  });
}

// sends one backlight level, reporting it when not taken
async function thermoSaveBacklight(thermoWindow, level) {
  const reply = await thermoRequest(`${THERMO_DEVICE}_backlight_set_${level}`);
  if (thermoWindow.closed === true) {
    return;
  }
  const { error } = thermoParseNumber(reply);
  if (error !== '') {
    thermoReportFailure(`setting the thermostat backlight to ${level}`, error,
      'The thermostat refused it: the backlight runs 0 to 255.');
  }
}

//// text colour ////

// Draws the colour set's swatch larger; an unknown code (null) leaves them all the same.
function thermoMarkColor(code) {
  THERMO_TEXT_COLORS.forEach(textColor => {
    const swatch = document.getElementById(thermoSwatchId(textColor.code));
    swatch.classList.toggle(THERMO_SWATCH_ACTIVE_CLASS, textColor.code === code);
  });
}

// reads the display's colour into the swatches
async function thermoReadColor(thermoWindow) {
  thermoWindow.colorSeq += 1;
  const sequence = thermoWindow.colorSeq;
  const reply = await thermoRequest(`${THERMO_DEVICE}_color`);
  if (thermoWindow.closed === true || sequence !== thermoWindow.colorSeq) {
    return;
  }
  const { answer, error } = thermoParseNumber(reply);
  if (error !== '') {
    thermoReportFailure('reading the thermostat text colour', error, THERMO_READ_REFUSED);
    thermoMarkColor(null);
    return;
  }
  thermoMarkColor(answer);
}

// A swatch: marks it at once and sets it, then reads the read-out on show again. A colour
// not taken is reported and the swatches go back to what the thermostat kept.
async function thermoSaveColor(thermoWindow, textColor) {
  thermoWindow.colorSeq += 1;
  const sequence = thermoWindow.colorSeq;
  thermoMarkColor(textColor.code);
  const reply = await thermoRequest(`${THERMO_DEVICE}_color_set_${textColor.code}`);
  if (thermoWindow.closed === true) {
    return;
  }
  const error = thermoReplyError(reply);
  if (error !== '') {
    thermoReportFailure(`setting the thermostat text colour to ${textColor.label}`, error,
      'The thermostat refused it: the text colours run 0 to 14.');
    if (sequence === thermoWindow.colorSeq) {
      thermoReadColor(thermoWindow);
    }
  }
  thermoReadLog(thermoWindow, thermoWindow.logView);
}

//// status and log ////

// Settings: the display's colour, and the status first
function thermoOpenSettings(thermoWindow) {
  thermoReadColor(thermoWindow);
  thermoShowLog(thermoWindow, THERMO_STATUS_VIEW);
}

// Refresh under the settings: the colour and the read-out on show
function thermoRefreshSettings(thermoWindow) {
  thermoReadColor(thermoWindow);
  thermoShowLog(thermoWindow, thermoWindow.logView);
}

// Back: to the two panels, dropping a read in the air
function thermoBack(thermoWindow) {
  thermoWindow.logSeq += 1;
  thermoShowPanel(thermoWindow, 'main');
}

// the read-out the log box is not showing
function thermoOtherView(logView) {
  if (logView === THERMO_STATUS_VIEW) {
    return THERMO_LOG_VIEW;
  }
  return THERMO_STATUS_VIEW;
}

// the title's menu row: switches to the other read-out
function thermoSwitchView(thermoWindow) {
  thermoShowLog(thermoWindow, thermoOtherView(thermoWindow.logView));
}

// Shows a read-out in the log box, names it, and offers the other in the title's menu.
function thermoShowLog(thermoWindow, logView) {
  thermoWindow.logView = logView;
  thermoShowPanel(thermoWindow, 'log');
  thermoWindow.logTitle.textContent = logView.title;
  thermoWindow.viewIcon.className = logView.icon;
  const otherView = thermoOtherView(logView);
  thermoWindow.viewRowIcon.className = otherView.icon;
  thermoWindow.viewRowText.textContent = otherView.title;
  thermoWindow.logBox.value = 'Loading...';
  thermoWindow.logBox.scrollTop = 0;
  thermoReadLog(thermoWindow, logView);
}

// reads a read-out into the log box, over what it holds
function thermoReadLog(thermoWindow, logView) {
  readPicoLog(thermoWindow, thermoRequest, thermoReplyError, logView.command, logView.scrollToEnd);
}
