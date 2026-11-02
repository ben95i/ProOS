// Automate - dropdown menus: opening and closing, drawing rows from text (drawMenu /
// createListItem), the request token that keeps a late reply out of the wrong menu, and
// the device menus. Rows are text, HOST|TYPE|NAME per line (menuRow() in utils.js).
// What only the page knows is handed in through configureMenus().

import { classDisplay, cleanString, splitMenuData, isNumeric, splitByDelimiter,
  sendCmd, loadMenu, fileURL, makeElement, setSpinner, menuRow,
  indicator, requestThrottle, dropdownButtonTemplate } from './utils.js';
import { reportException, reportNotConnected, logError } from './error.js';
import { subwooferVolumeEnabled, subwooferVolumeMode,
  SUBWOOFER_CLASS } from './volctrl.js';
import ToggleSwitch, { toggleSwitchCallback } from './switch.js';

//// page hooks ////

// Page hooks, null until configureMenus() sets them; a missing hook is done without.
const MENU_HOOKS = {
  // () => void: closes the menus other modules own (bookmarks, weather)
  closePageMenus: null,
  // (roomKey) => that room's config, or undefined
  roomConfig: null,
  // (roomKey) => void: a row of the room menu was picked
  onRoomPicked: null,
  // () => void: the TextScroll row of the LED effects menu was picked
  openTextScroll: null,
  // () => void: the More... row of the Pixels Control menu was picked; server page
  // only, and without it the row is left off
  openPixels: null,
  // (settingName) => { get(), set(on) } for a 'chk' row, or undefined
  browserSetting: null
};

// Sets the page hooks named in `hooks`.
export function configureMenus(hooks) {
  for (const hookName of Object.keys(MENU_HOOKS)) {
    if (typeof hooks[hookName] === 'function') {
      MENU_HOOKS[hookName] = hooks[hookName];
    }
  }
}

// Runs one page hook, or answers undefined when the page has not supplied it.
function callMenuHook(hookName, ...hookArguments) {
  const hook = MENU_HOOKS[hookName];
  if (typeof hook !== 'function') {
    return undefined;
  }
  return hook(...hookArguments);
}

//// menu request token ////

// Stamped by every menu request and checked on its reply: opening or closing a menu
// invalidates what is in the air, so a slow reply cannot wipe the menu on screen.
let menuDrawToken = 0;

// stamps a new request and answers its token
export function beginMenuRequest() {
  menuDrawToken += 1;
  return menuDrawToken;
}

// is this token still the newest, so that its reply may draw?
export function menuRequestCurrent(token) {
  return token === menuDrawToken;
}

//// dropdowns ////

// Closes every dropdown and takes down what was drawn in them.
export function removeDropdowns() {
  beginMenuRequest();
  classDisplay("dd-content", "none");
  removeDynMenus();
  destroyActiveSwitches();
  callMenuHook('closePageMenus');
}

// Opens a dropdown, or shuts it when open. Answers true when the caller should fill it.
export function openDropdown(menuId) {
  const menuElement = document.getElementById(menuId);
  if (!menuElement) {
    return false;
  }
  if (menuElement.style.display === 'block') {
    menuElement.style.display = 'none';
    return false;
  }
  removeDropdowns();
  menuElement.style.display = 'block';
  return true;
}

// Destroys the menus' toggle switches, which own pointer listeners.
export function destroyActiveSwitches() {
  App.activeSwitches.forEach(toggleSwitch => toggleSwitch.destroy());
  App.activeSwitches = [];
}

//// drawing a menu ////

// Draws a menu from HOST|TYPE|NAME rows. TwoColumns reads HOST|NAME with the menu's id
// as the type. MenuScroll false is for redrawing a menu already on screen (its
// scroll step measures a fixed menu against the page offset, walking the page down).
export function drawMenu(data, menuId, { TwoColumns = false, MenuScroll = true } = {}) {
  removeDynMenus();
  if (data === null || data === undefined) {
    // the request that came back empty has already reported why
    logError(`drawing the ${menuId} menu`, 'no data came back to draw');
    return;
  }
  if (!data || typeof data !== 'string') {
    reportException(`drawing the ${menuId} menu`, 'No data came back to draw.');
    return;
  }
  const menuElement = document.getElementById(menuId);
  if (!menuElement) {
    return;
  }
  const fragment = document.createDocumentFragment();
  const rowLines = splitMenuData(data);
  if (rowLines == "") {
    return;
  }
  for (let rowIndex = 0; rowIndex < rowLines.length; rowIndex++) {
    const rowLine = rowLines[rowIndex];
    if (rowLine === "") {
      continue;
    }
    const rowColumns = rowLine.split("|");
    // 0=Host, 1=Type, 2=Name
    const hostColumn = rowColumns[0];
    let rowType = rowColumns[1];
    let rowLabel = '';
    if (TwoColumns === true) {
      rowType = menuId;
      rowLabel = rowColumns[1];
    } else {
      rowLabel = rowColumns[2].trim();
    }
    const rowElement = createListItem(hostColumn, rowType, rowLabel, rowIndex);
    App.dynMenuNodes.push(rowElement);
    fragment.appendChild(rowElement);
  }
  menuElement.appendChild(fragment);
  if (MenuScroll === false) {
    return;
  }
  scrollMenuIntoView(menuElement);
}

// scrolls down to a menu that runs past the bottom of the viewport
function scrollMenuIntoView(menuElement) {
  const viewportHeight = window.innerHeight || document.documentElement.clientHeight;
  const menuBounds = menuElement.getBoundingClientRect();
  if (menuBounds.bottom <= viewportHeight) {
    return;
  }
  const pageScrollTop = window.pageYOffset || document.documentElement.scrollTop;
  const menuPageTop = menuBounds.top + pageScrollTop;
  // only move down, and never past the top of the menu
  if (menuPageTop > pageScrollTop) {
    window.scrollTo({ top: menuPageTop, behavior: 'smooth' });
  }
}

// Takes every drawn row out of the page, with its switches and pending slider send.
export function removeDynMenus() {
  if (App.dynMenuNodes.length === 0) {
    return;
  }
  throttledSliderSend.cancel();
  destroyActiveSwitches();
  App.dynMenuNodes.forEach(rowElement => rowElement.remove());
  App.dynMenuNodes.length = 0;
}

//// menu rows ////

// the dot class each indicator type draws
const INDICATOR_CLASSES = {
  'noind': '',
  'blkind': '',
  'grnind': 'ind_dot_green',
  'redind': 'ind_dot_red',
  'ylwind': 'ind_dot_yellow'
};

// the icon an 'on', 'off' or 'pause' row carries
const ACTION_ROW_ICONS = {
  'on': 'fad fa-toggle-on',
  'off': 'fad fa-toggle-off',
  'pause': 'fad fa-pause'
};

// slider rows: a 'slidercust' device counts 0 as off
const SLIDER_MIN = 0;
const SLIDER_CUSTOM_MIN = 1;
const SLIDER_MAX = 255;
const SLIDER_STEP = 1;

// A dragged slider sends at most this often; one throttle serves every slider.
export const SLIDER_MS = 120;
const throttledSliderSend = requestThrottle((action, host, command) => {
  sendCmd(action, host, command);
}, SLIDER_MS);

// the switch a 'chk' row draws
const SETTING_SWITCH_WIDTH = '54px';
const SETTING_SWITCH_HEIGHT = '26px';

// Row type -> the function that fills the row in; any other type is a status row.
const ROW_BUILDERS = {
  'ledeffectmenu': buildLedEffectRow,
  'pixelsmore': buildPixelsMoreRow,
  'room': buildRoomRow,
  'relaxmenu': buildRelaxRow,
  'relaxactive': buildRelaxRow,
  'relaxstop': buildRelaxStopRow,
  'link': buildLinkRow,
  'chk': buildSettingRow,
  'slider': buildSliderRow,
  'slidercust': buildSliderRow,
  'subs': buildSubwooferRow,
  'toggle': buildToggleRow,
  'on': buildActionRow,
  'off': buildActionRow,
  'pause': buildActionRow
};

// Builds one row. hostColumn is plain data, or VALUE~HOST~COMMAND for rows that send.
function createListItem(hostColumn, rowType, rowLabel, rowIndex) {
  const rowElement = document.createElement('a');
  const [value, host, command] = hostColumn.split("~");
  rowElement.id = "menu-" + rowIndex;
  rowElement.innerText = rowLabel;
  const row = {
    element: rowElement,
    type: rowType,
    label: rowLabel,
    value: value,
    host: host,
    command: command
  };
  let buildRow = buildStatusRow;
  if (Object.prototype.hasOwnProperty.call(ROW_BUILDERS, rowType)) {
    buildRow = ROW_BUILDERS[rowType];
  }
  buildRow(row);
  return rowElement;
}

// the server's answer when LED hosts did not take a command, then their names
const LED_NO_ANSWER = 'No answer from';

// Sends an LED command to every host, reporting any that did not answer as Not
// connected. sendLeds('stop'); sendLeds('textscroll', base64)
export async function sendLeds(command, value = '') {
  const reply = await sendCmd('leds', command, value);
  if (typeof reply === 'string' && reply.startsWith(LED_NO_ANSWER)) {
    reportNotConnected(`sending the LED command ${command}`, `${reply}.`);
  }
  return reply;
}

// LED effects menu: a row plays its effect; TextScroll opens the scroller
function buildLedEffectRow(row) {
  if (row.value == 'shuffle') {
    row.element.appendChild(makeElement('i', 'fad fa-random'));
  }
  row.element.addEventListener("click", function() {
    if (row.value == 'textscroll') {
      callMenuHook('openTextScroll');
    } else {
      sendLeds(row.value);
    }
  });
}

// Pixels Control menu's More... row, which opens the Pixels window
function buildPixelsMoreRow(row) {
  row.element.appendChild(makeElement('i', 'fad fa-cog'));
  row.element.addEventListener("click", function() {
    callMenuHook('openPixels');
  });
}

// Room menu row: picks the room, with the room's icon on the right.
function buildRoomRow(row) {
  const roomConfig = callMenuHook('roomConfig', row.value);
  if (roomConfig && roomConfig.icon) {
    row.element.appendChild(makeElement('i', roomConfig.icon));
  }
  row.element.addEventListener("click", function() {
    callMenuHook('onRoomPicked', row.value);
  });
}

// relax sounds row: plays its sound; the one playing is marked
function buildRelaxRow(row) {
  if (row.type == 'relaxactive') {
    row.element.appendChild(makeElement('i', 'fad fa-play icon-green'));
  }
  row.element.addEventListener("click", function() {
    sendCmd('main', 'relax', row.value);
  });
}

// relax sounds row that stops whatever is playing
function buildRelaxStopRow(row) {
  row.element.addEventListener("click", function() {
    sendCmd('main', 'relaxstop');
  });
  row.element.appendChild(makeElement('i', 'fad fa-stop icon-red'));
}

// URL row: a plain link to its first field
function buildLinkRow(row) {
  row.element.classList.add('norm-cursor');
  row.element.href = row.value;
}

// Checkbox row for a browser-side setting: the page supplies its read and write, keyed
// by the first field. The label is the icon.
function buildSettingRow(row) {
  const setting = callMenuHook('browserSetting', row.value);
  if (!setting) {
    return;
  }
  row.element.classList.add('menu-checkbox');
  row.element.classList.add('nohide');
  row.element.classList.add('no-hover');
  row.element.innerText = '';
  row.element.appendChild(makeElement('i', row.label));
  const switchCommand = `${row.value}-`;
  const settingSwitch = new ToggleSwitch(row.element, {
    id: `setting-${row.value}`,
    checked: setting.get() === true,
    width: SETTING_SWITCH_WIDTH,
    height: SETTING_SWITCH_HEIGHT,
    cmdHost: 'local',
    command: switchCommand,
    onToggle: (payload) => setting.set(payload.cmd === `${switchCommand}on`)
  });
  settingSwitch.activate();
  App.activeSwitches.push(settingSwitch);
}

// Slider row, at the first field's level. Sends while dragged, throttled, and once more
// on release so the last one lands.
function buildSliderRow(row) {
  const slider = document.createElement('input');
  slider.classList.add("slider-wide");
  slider.type = 'range';
  if (row.type === 'slidercust') {
    slider.min = SLIDER_CUSTOM_MIN;
  } else {
    slider.min = SLIDER_MIN;
  }
  slider.max = SLIDER_MAX;
  slider.value = row.value;
  slider.step = SLIDER_STEP;
  slider.addEventListener('input', (event) => {
    throttledSliderSend('main', row.host, `${row.command}_set_${event.target.value}`);
  });
  slider.addEventListener('change', (event) => {
    throttledSliderSend.cancel();
    sendCmd('main', row.host, `${row.command}_set_${event.target.value}`);
  });
  row.element.appendChild(slider);
  row.element.classList.add('slider-wide_container');
  row.element.classList.add('no-hover');
}

// Subwoofer row: red while the volume buttons control the subwoofers; a press flips it.
function buildSubwooferRow(row) {
  const subwooferMode = subwooferVolumeEnabled(App.currentRoom);
  if (subwooferMode === true) {
    row.element.classList.add(SUBWOOFER_CLASS);
  } else {
    row.element.classList.remove(SUBWOOFER_CLASS);
  }
  row.element.addEventListener("click", function() {
    subwooferVolumeMode(App.currentRoom, !subwooferMode);
  });
}

// switch row for a device that is on (1) or off (0)
function buildToggleRow(row) {
  row.element.classList.add('no-hover');
  const toggleSwitch = new ToggleSwitch(row.element, {
    id: `${row.host}-${row.command}`,
    checked: row.value === '1',
    cmdHost: row.host,
    command: row.command,
    onToggle: toggleSwitchCallback
  });
  toggleSwitch.activate();
  App.activeSwitches.push(toggleSwitch);
}

// 'on', 'off' and 'pause' rows: an icon, and the row's action / host / command on press
function buildActionRow(row) {
  row.element.appendChild(makeElement('i', ACTION_ROW_ICONS[row.type]));
  row.element.addEventListener("click", function() {
    sendCmd(row.value, row.host, row.command);
  });
}

// Status row with a dot in the type's colour; any other type is a bare label. A 'cmd'
// row sends its command when pressed.
function buildStatusRow(row) {
  if (!Object.prototype.hasOwnProperty.call(INDICATOR_CLASSES, row.type)) {
    return;
  }
  if (row.value == 'cmd') {
    row.element.addEventListener("click", function() {
      sendCmd('main', row.host, row.command);
    });
  }
  if (row.type === 'noind') {
    return;
  }
  const indicatorDot = makeElement('span', 'ind_dot');
  const dotColorClass = INDICATOR_CLASSES[row.type];
  if (dotColorClass !== '') {
    indicatorDot.classList.add(dotColorClass);
  }
  indicatorDot.id = "ind-" + row.label;
  row.element.appendChild(indicatorDot);
}

//// rows the device menus share ////

// the one red row a menu shows for an answer it cannot read
export function menuErrorRow(menuId) {
  return menuRow(`${menuId}-error`, 'redind', 'Error');
}

// the one black row a menu shows while its device is off
export function menuOfflineRow(menuId) {
  return menuRow(`${menuId}-offline`, 'blkind', 'Offline');
}

// One row per choice, green on the selected one. choices: [{ code, cmd, label }], where
// code is what the device answers while that choice is live.
export function inputMenuRows(target, selectedCode, choices) {
  let rows = '';
  choices.forEach(choice => {
    rows += menuRow(`cmd~${target}~${choice.cmd}`,
      indicator(selectedCode == choice.code), choice.label);
  });
  return rows;
}

//// device menus ////

// Opens a dropdown and fills it from fetchRows(), unless another menu took over meanwhile.
function fillDropdown(menuId, fetchRows, drawConfig) {
  if (!openDropdown(menuId)) {
    return;
  }
  const token = beginMenuRequest();
  fetchRows().then((data) => {
    if (!menuRequestCurrent(token)) {
      return;
    }
    drawMenu(data, menuId, drawConfig);
  });
}

// a menu whose rows come out of a file on the device
export function showDynFileMenu(menuId, twoColumnRows = false) {
  fillDropdown(menuId, () => loadMenu(fileURL('read', menuId)), { TwoColumns: twoColumnRows });
}

// a menu whose rows come back from a device API call
export function showDynAPIMenu(menuId) {
  const url = location.protocol + "//" + location.hostname +
    "/api?var=&arg=" + menuId + "&action=main";
  fillDropdown(menuId, () => loadMenu(url));
}

// the ids of a device menu button's label and spinner, for dropdownButtonTemplate()
export function spinnerIds(name) {
  return { textId: `${name}-text`, spinnerId: `${name}-spinner` };
}

// Opens a device menu with the button's spinner turning, asks the device for its state
// and draws the rows build(reply) answers. config: menu, target, cmd, build, optional
// action ('main' by default) and spinner (the name in spinnerIds(), by default the menu).
export function openDeviceMenu(config) {
  if (!openDropdown(config.menu)) {
    return;
  }
  let spinnerName = config.menu;
  if (config.spinner) {
    spinnerName = config.spinner;
  }
  const { textId, spinnerId } = spinnerIds(spinnerName);
  let action = 'main';
  if (config.action) {
    action = config.action;
  }
  setSpinner(textId, spinnerId, true);
  const token = beginMenuRequest();
  sendCmd(action, config.target, config.cmd).then((data) => {
    const menuData = config.build(cleanString(data));
    setSpinner(textId, spinnerId, false);
    // another menu opened meanwhile: drawing would wipe its rows
    if (!menuRequestCurrent(token)) {
      return;
    }
    if (menuData === '') {
      removeDynMenus();
      return;
    }
    drawMenu(menuData, config.menu);
  });
}

// An amp's input menu, green on the live input. config: menu, target, cmd, spinner, as
// openDeviceMenu() takes them, plus offCode (the amp's answer while off) and inputs
// (as inputMenuRows() takes them).
export function showInputMenu(config) {
  openDeviceMenu({
    menu: config.menu,
    target: config.target,
    cmd: config.cmd,
    spinner: config.spinner,
    build: (reply) => {
      if (isNumeric(reply) !== true) {
        return menuErrorRow(config.menu);
      }
      if (Number(reply) === config.offCode) {
        return menuOfflineRow(config.menu);
      }
      return inputMenuRows(config.target, reply, config.inputs);
    }
  });
}

//// power menus ////

// LED strips whose level counts 0 as off: no slider while off, Off and Pause rows while on
const CUSTOM_LEDS = {
  'bed_leds': {
    hostDevice: 'brxmit',
    pauseCmd: 'bed_leds_pause',
    offCmd: 'bed_leds_off'
  }
};

// the Fadecandy pixels, asked through the 'leds' action
const FADECANDY_MENU = 'fc_leds';
const PIXELS_MORE_LABEL = 'More...';

// A power menu's button and dropdown: powerButtonTemplate('brtv', 'fad fa-tv-retro', 'TV')
export function powerButtonTemplate(name, icon, label) {
  return dropdownButtonTemplate({
    buttonId: `btn-power-${name}`,
    icon: icon,
    label: label,
    ...spinnerIds(name),
    menuId: `${name}-menu`
  });
}

// Handlers for power menu buttons of one target, in the shape registerListeners() takes.
// powerListeners('brxmit', ['brtv']) -> { 'btn-power-brtv': () => showPowerMenu('brxmit', 'brtv') }
export function powerListeners(target, names) {
  const listeners = {};
  names.forEach(name => {
    listeners[`btn-power-${name}`] = () => showPowerMenu(target, name);
  });
  return listeners;
}

// The switch and / or slider rows from the device's answer: state, and the level after
// it for a dual menu. '' when there is nothing to draw.
function powerMenuRows(menu, target, state, level) {
  const isDualToggleSlider = menu.endsWith('_dual');
  const isLedSlider = menu.endsWith('_leds');
  let rows = '';
  if (!isLedSlider && (state == '1' || state == '0')) {
    rows += menuRow(`${state}~${target}~${menu}`, 'toggle', '');
  }
  if (isDualToggleSlider) {
    rows += menuRow(`${level}~${target}~${menu}`, 'slider', '');
    return rows;
  }
  if (menu === FADECANDY_MENU) {
    rows += menuRow(`${state}~leds~fc`, 'slider', '');
    return rows;
  }
  if (!isLedSlider) {
    return rows;
  }
  const customLeds = CUSTOM_LEDS[menu];
  // a standard slider-only LED, 0-255
  if (!customLeds) {
    rows += menuRow(`${state}~${target}~${menu}`, 'slider', '');
    return rows;
  }
  // custom LED, 0 = off: nothing to dim or stop while off
  const ledsAreOff = Number(state) <= 0;
  if (ledsAreOff === true) {
    return rows;
  }
  // the brightness slider and its buttons
  rows += menuRow(`${state}~${target}~${menu}`, 'slidercust', '');
  rows += menuRow(`main~${customLeds.hostDevice}~${customLeds.pauseCmd}`, 'pause', 'Pause');
  rows += menuRow(`main~${customLeds.hostDevice}~${customLeds.offCmd}`, 'off', 'Off');
  return rows;
}

// Opens a device's power menu: asks `target` for the state of `menu` and draws a switch,
// a slider or both from the answer. With no menu named, the target is the menu.
export function showPowerMenu(target, menu = target) {
  let action = 'main';
  if (menu == FADECANDY_MENU) {
    action = 'leds';
  }
  openDeviceMenu({
    menu: `${menu}-menu`,
    spinner: menu,
    action: action,
    target: target,
    cmd: menu,
    build: (reply) => {
      let menuData = '';
      const [state, level] = splitByDelimiter(reply, '~');
      if (isNumeric(state) === false) {
        menuData += menuErrorRow(menu);
      } else {
        menuData += powerMenuRows(menu, target, state, level);
      }
      // there even when no host answered: the Pixels window shows which are down
      if (menu == FADECANDY_MENU && MENU_HOOKS.openPixels !== null) {
        menuData += menuRow(FADECANDY_MENU, 'pixelsmore', PIXELS_MORE_LABEL);
      }
      return menuData;
    }
  });
}
