// Automate Website - menus
//
// Everything a dropdown menu needs, in one place: opening and closing the
// dropdowns, drawing a menu's rows from text (drawMenu / createListItem),
// the request token that keeps a late reply out of the wrong menu, and the
// device menus the room grids open (power menus, input menus, file and API
// menus).
//
// Rows are text, one per line, as HOST|TYPE|NAME; menuRow() in utils.js
// builds one. The menu state stays on window.App (activeSwitches, fileData,
// dynMenuNodes, dynMenuActive), where mainmenu.js also reads the drawn rows.
//
// This module sits below main.js and the room modules, so it cannot import
// what only the page knows -- the rooms, the LED text scroller, the browser
// settings, the bookmarks and weather menus. main.js hands those in once
// through configureMenus().

import { classDisplay, cleanString, splitMenuData, isNumeric, splitByDelimiter,
  sendCmd, showPopup, loadMenu, fileURL, makeElement, setSpinner, menuRow,
  indicator, requestThrottle, dropdownButtonTemplate, subwooferVolumeEnabled,
  subwooferVolumeMode, SUBWOOFER_CLASS } from './utils.js';
import ToggleSwitch, { toggleSwitchCallback } from './switch.js';

//// page hooks ////

// What the page supplies. Each one is left null until configureMenus() sets
// it, and a row or a close that needs a missing hook simply does without.
const MENU_HOOKS = {
  // () => void: closes the menus other modules own (bookmarks, weather)
  closePageMenus: null,
  // (roomKey) => that room's config, or undefined
  roomConfig: null,
  // (roomKey) => void: a row of the room menu was picked
  onRoomPicked: null,
  // () => void: the TextScroll row of the LED effects menu was picked
  openTextScroll: null,
  // (settingName) => { get(), set(on) } for a 'chk' row, or undefined
  browserSetting: null
};

// Sets the page hooks above. Called once by main.js while it loads.
export function configureMenus(hooks) {
  for (const hookName of Object.keys(MENU_HOOKS)) {
    if (typeof hooks[hookName] === 'function') {
      MENU_HOOKS[hookName] = hooks[hookName];
    }
  }
}

// Runs one page hook with the arguments given and answers what it returns,
// or undefined when the page has not supplied it.
function callMenuHook(hookName, ...hookArguments) {
  const hook = MENU_HOOKS[hookName];
  if (typeof hook !== 'function') {
    return undefined;
  }
  return hook(...hookArguments);
}

//// menu request token ////

// Every menu request stamps this before it goes out and checks it when it
// comes back. Opening another menu -- or closing the menus outright --
// invalidates whatever is still in the air, so a slow reply can no longer
// wipe the rows of the menu that is actually on screen.
let menuDrawToken = 0;

// Stamps a new request and answers its token.
export function beginMenuRequest() {
  menuDrawToken += 1;
  return menuDrawToken;
}

// Is this token still the newest one, so that its reply may draw?
export function menuRequestCurrent(token) {
  return token === menuDrawToken;
}

//// dropdowns ////

// Closes every dropdown on the page and takes down what was drawn in them.
export function removeDropdowns() {
  beginMenuRequest(); // anything still in flight is no longer allowed to draw itself
  classDisplay("dd-content", "none");
  removeDynMenus();
  destroyActiveSwitches();
  callMenuHook('closePageMenus');
}

// roomstate.js, weather.js and mainmenu.js reach these two through window
window.removeDropdowns = removeDropdowns;

// Opens a dropdown, or shuts it when it is already open. Answers true when
// the caller should go on and fill it.
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

// toggle switches own pointer listeners, so they are destroyed rather than
// just dropped whenever their markup goes away
export function destroyActiveSwitches() {
  if (App.activeSwitches && App.activeSwitches.length > 0) {
    App.activeSwitches.forEach(toggleSwitch => {
      if (typeof toggleSwitch.destroy === 'function') {
        toggleSwitch.destroy();
      }
    });
    App.activeSwitches = [];
  }
}

//// drawing a menu ////

// Draws each menu item from pipe-delimited rows: HOST|TYPE|NAME.
// config.TwoColumns reads the rows as HOST|NAME, with the menu's own id as
// every row's type. config.MenuScroll is only ever passed false by a caller
// redrawing a menu that is already on screen (the room status refresh): the
// step at the bottom measures a fixed-position menu against the page offset,
// which is right the once on opening and walks the page down if it is run on
// every refresh.
export function drawMenu(data, menuId, config = undefined) {
  let scrollIntoView = true;  // menu scrolls into view by default
  let twoColumnRows = false;  // 3-column data is the default
  if (config !== undefined) {
    scrollIntoView = config.MenuScroll;
    twoColumnRows = config.TwoColumns;
  }
  removeDynMenus();
  if (!data || typeof data !== 'string') {
    showPopup("[drawMenu] no data to parse.");
    return;
  }
  const menuElement = document.getElementById(menuId);
  if (!menuElement) {
    return;
  }
  const fragment = document.createDocumentFragment();
  while (App.fileData.length) {
    App.fileData.pop();
  }
  const rowLines = splitMenuData(data);
  if (rowLines == "" || rowLines == null) {
    return;
  }
  for (let rowIndex = 0; rowIndex < rowLines.length; rowIndex++) {
    const rowLine = rowLines[rowIndex];
    if (rowLine == "" || rowLine == null) {
      continue;
    }
    const rowColumns = rowLine.split("|");
    App.fileData.push(rowLine);
    // 0=Host, 1=Type, 2=Name
    const hostColumn = rowColumns[0];
    let rowType = rowColumns[1];
    let rowLabel = '';
    if (twoColumnRows === true) { // 2-column data mode
      rowType = menuId;
      rowLabel = rowColumns[1];
    } else {
      rowLabel = rowColumns[2].trim();
    }
    const rowElement = createListItem(hostColumn, rowType, rowLabel, rowIndex);
    App.dynMenuNodes.push(rowElement);
    fragment.appendChild(rowElement);
    App.dynMenuActive = 1;
  }
  App.fileData.push(menuId); // menu name at the end of the array
  menuElement.appendChild(fragment); // one append for the whole menu
  // a redraw of a menu already on screen stops here
  if (scrollIntoView === false) {
    return;
  }
  scrollMenuIntoView(menuElement);
}

// roomstate.js and mainmenu.js draw their rows through window.drawMenu
window.drawMenu = drawMenu;

// scrolls down to the menu only when it runs past the bottom of the viewport
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

// Takes every drawn row back out of the page, with the switches and the
// pending slider send that belonged to them.
export function removeDynMenus() {
  if (App.dynMenuActive != 1) {
    return;
  }
  throttledSliderSend.cancel();
  destroyActiveSwitches();
  App.dynMenuNodes.forEach(rowElement => rowElement.remove());
  App.dynMenuNodes.length = 0;
  App.dynMenuActive = 0;
}

//// menu rows ////

// the indicator types, and the dot class each one draws
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

// slider rows: the range a level runs over, and the least a 'slidercust' row
// can be set to (its device counts 0 as off)
const SLIDER_MIN = 0;
const SLIDER_CUSTOM_MIN = 1;
const SLIDER_MAX = 255;
const SLIDER_STEP = 1;

// While a slider is dragged it sends its level at most this often. One
// throttle is shared by every slider, as only one can be dragged at a time.
const SLIDER_MS = 75;
const throttledSliderSend = requestThrottle((action, host, command) => {
  sendCmd(action, host, command);
}, SLIDER_MS);

// the switch a 'chk' row draws, smaller than a device toggle
const SETTING_SWITCH_WIDTH = '54px';
const SETTING_SWITCH_HEIGHT = '26px';

// Row types to the function that fills the row in. A type with no entry here
// is drawn as a status row (see buildStatusRow).
const ROW_BUILDERS = {
  'ledeffectmenu': buildLedEffectRow,
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

// Builds one row. hostColumn is the row's first field -- plain data for some
// types, VALUE~HOST~COMMAND for the rows that send something -- rowType is
// its type, rowLabel its text and rowIndex its place in the menu.
function createListItem(hostColumn, rowType, rowLabel, rowIndex) {
  const rowElement = document.createElement('a');
  const [value, host, command] = hostColumn.split("~");
  rowElement.id = "menu-" + rowIndex;
  rowElement.innerText = rowLabel;
  const row = {
    element: rowElement,
    type: rowType,
    label: rowLabel,
    value: value,      // the first part of the first field
    host: host,        // the device the row's command goes to
    command: command   // the command itself
  };
  let buildRow = buildStatusRow;
  if (Object.prototype.hasOwnProperty.call(ROW_BUILDERS, rowType)) {
    buildRow = ROW_BUILDERS[rowType];
  }
  buildRow(row);
  return rowElement;
}

// LED effects menu: a row plays its effect, and TextScroll opens the scroller
function buildLedEffectRow(row) {
  if (row.value == 'shuffle') {
    row.element.appendChild(makeElement('i', 'fad fa-random'));
  }
  row.element.addEventListener("click", function() {
    if (row.value == 'textscroll') {
      callMenuHook('openTextScroll');
    } else {
      sendCmd('leds', row.value);
    }
  });
}

// Room selector menu. Each row carries its room's icon, the same one the
// selector shows, at the right of the row like every other menu's.
function buildRoomRow(row) {
  const roomConfig = callMenuHook('roomConfig', row.value);
  if (roomConfig && roomConfig.icon) {
    row.element.appendChild(makeElement('i', roomConfig.icon));
  }
  row.element.addEventListener("click", function() {
    callMenuHook('onRoomPicked', row.value);
  });
}

// relax sounds menu: a row plays its sound, and the one playing is marked
function buildRelaxRow(row) {
  if (row.type == 'relaxactive') {
    row.element.appendChild(makeElement('i', 'fad fa-play icon-green'));
  }
  row.element.addEventListener("click", function() {
    sendCmd('main', 'relax', row.value);
  });
}

// relax sounds menu: the row that stops whatever is playing
function buildRelaxStopRow(row) {
  row.element.addEventListener("click", function() {
    sendCmd('main', 'relaxstop');
  });
  row.element.appendChild(makeElement('i', 'fad fa-stop icon-red'));
}

// URL menu: the row is a plain link to its first field
function buildLinkRow(row) {
  row.element.classList.add('norm-cursor');
  row.element.href = row.value;
}

// Checkbox row for a browser-side setting. A menu file lives on the device,
// so such a setting has nowhere in it to live: the page supplies its read and
// write, keyed by the name in the row's first field. The label is the icon.
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

// Horizontal slider row; the first field's value is the current level. The
// level is sent while the slider is dragged, throttled, and once more on
// release so the last one always lands.
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

// Subwoofer volume toggle: red while the room's volume buttons control the
// subwoofers, and a press flips the mode it was drawn in.
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

// 'on', 'off' and 'pause' rows: an icon, and the row's own action / host /
// command sent when it is pressed
function buildActionRow(row) {
  row.element.appendChild(makeElement('i', ACTION_ROW_ICONS[row.type]));
  row.element.addEventListener("click", function() {
    sendCmd(row.value, row.host, row.command);
  });
}

// Status row, with an indicator dot in the type's own colour. A type that is
// not an indicator either is drawn as a bare label.
function buildStatusRow(row) {
  if (!Object.prototype.hasOwnProperty.call(INDICATOR_CLASSES, row.type)) {
    return;
  }
  addStatusListener(row);
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

// a row marked 'cmd' sends its command to the device when it is pressed
function addStatusListener(row) {
  if (row.value == 'cmd') {
    row.element.addEventListener("click", function() {
      sendCmd('main', row.host, row.command);
    });
  }
}

//// rows the device menus share ////

// the one red row a menu shows when its device gave an answer it cannot read
export function menuErrorRow(menuId) {
  return menuRow(`${menuId}-error`, 'redind', 'Error');
}

// the one black row a menu shows when its device is switched off
export function menuOfflineRow(menuId) {
  return menuRow(`${menuId}-offline`, 'blkind', 'Offline');
}

// One row per choice, green on the one the device says is selected.
// choices: [{ code, cmd, label }], where code is what the device answers
// with while that choice is the live one.
export function inputMenuRows(target, selectedCode, choices) {
  let rows = '';
  choices.forEach(choice => {
    rows += menuRow(`cmd~${target}~${choice.cmd}`,
      indicator(selectedCode == choice.code), choice.label);
  });
  return rows;
}

//// device menus ////

// a menu whose rows come out of a file on the device
export function showDynFileMenu(menuId, twoColumnRows = false) {
  if (!openDropdown(menuId)) {
    return;
  }
  const token = beginMenuRequest();
  loadMenu(fileURL('read', menuId)).then((data) => {
    if (!menuRequestCurrent(token)) {
      return;
    }
    drawMenu(data, menuId, { TwoColumns: twoColumnRows, MenuScroll: true });
  });
}

// a menu whose rows come back from a device API call
export function showDynAPIMenu(menuId) {
  if (!openDropdown(menuId)) {
    return;
  }
  const url = location.protocol + "//" + location.hostname +
    "/api?var=&arg=" + menuId + "&action=main";
  const token = beginMenuRequest();
  loadMenu(url).then((data) => {
    if (!menuRequestCurrent(token)) {
      return;
    }
    drawMenu(data, menuId);
  });
}

// Opens a device menu, asks the device for its state and draws the rows the
// answer builds. config: menu, target, cmd, textId, spinnerId, build(reply).
export function openDeviceMenu(config) {
  if (!openDropdown(config.menu)) {
    return;
  }
  setSpinner(config.textId, config.spinnerId, true);
  const token = beginMenuRequest();
  sendCmd('main', config.target, config.cmd).then((data) => {
    const menuData = config.build(cleanString(data));
    // stop the spinner whether or not the rows are still wanted
    setSpinner(config.textId, config.spinnerId, false);
    // another menu was opened while this was in the air -- drawing now would
    // wipe the rows that menu just put on screen
    if (!menuRequestCurrent(token)) {
      return;
    }
    drawMenu(menuData, config.menu);
  });
}

// An amp's input menu: one row per input, green on the one that is live.
// config: menu, target, cmd, textId, spinnerId, as openDeviceMenu() takes
// them, plus offCode (what the amp answers while it is off) and inputs (the
// choices, as inputMenuRows() takes them).
export function showInputMenu(config) {
  openDeviceMenu({
    menu: config.menu,
    target: config.target,
    cmd: config.cmd,
    textId: config.textId,
    spinnerId: config.spinnerId,
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

// LED strips whose level counts 0 as off: their menu hides the slider while
// they are off and adds Off and Pause rows while they are on
const CUSTOM_LEDS = {
  'bed_leds': {
    hostDevice: 'brxmit',
    pauseCmd: 'bed_leds_pause',
    offCmd: 'bed_leds_off'
  }
};

// the Fadecandy pixels, which are asked through the 'leds' action
const FADECANDY_MENU = 'fc_leds';

// The button of a power menu and the dropdown under it, with the ids
// showPowerMenu(target, name) looks for.
// powerButtonTemplate('brtv', 'fad fa-tv-retro', 'TV')
export function powerButtonTemplate(name, icon, label) {
  return dropdownButtonTemplate({
    buttonId: `btn-power-${name}`,
    icon: icon,
    label: label,
    textId: `${name}-text`,
    spinnerId: `${name}-spinner`,
    menuId: `${name}-menu`
  });
}

// Builds the toggle and / or slider rows for one power menu from the device's
// answer -- state, and for a dual menu the level after it. Returns "" when
// there is nothing to draw.
function powerMenuRows(menu, target, state, level) {
  const isDualToggleSlider = menu.endsWith('_dual');
  const isSingleLedSlider = menu.endsWith('_leds');
  const isFadeCandyLeds = (menu === FADECANDY_MENU);
  const isDefinedMenu = (isDualToggleSlider || isSingleLedSlider || isFadeCandyLeds);
  let rows = '';
  if (isDefinedMenu === false || isDualToggleSlider === true) {
    if (state == '1' || state == '0') {
      rows += menuRow(`${state}~${target}~${menu}`, 'toggle', '');
    }
  }
  if (isDualToggleSlider) {
    rows += menuRow(`${level}~${target}~${menu}`, 'slider', '');
    return rows;
  }
  if (isFadeCandyLeds) {
    rows += menuRow(`${state}~leds~fc`, 'slider', '');
    return rows;
  }
  if (!isSingleLedSlider) {
    return rows;
  }
  const customLeds = CUSTOM_LEDS[menu];
  // standard slider-only LED 0-255
  if (!customLeds) {
    rows += menuRow(`${state}~${target}~${menu}`, 'slider', '');
    return rows;
  }
  // custom LED 0=OFF 1-255
  const ledsAreOff = Number(state) <= 0;
  // hide off button and slider when off
  if (ledsAreOff === true) {
    return rows;
  }
  // append brightness slider and buttons
  rows += menuRow(`${state}~${target}~${menu}`, 'slidercust', '');
  rows += menuRow(`main~${customLeds.hostDevice}~${customLeds.offCmd}`, 'off', 'Off');
  rows += menuRow(`main~${customLeds.hostDevice}~${customLeds.pauseCmd}`, 'pause', 'Pause');
  return rows;
}

// Opens a device's power menu: asks `target` for the state of `menu` and
// draws a switch, a slider or both from the answer. With no menu named, the
// target is the menu.
export function showPowerMenu(target, menu = null) {
  if (menu == '' || menu == null) {
    menu = target;
  }
  const menuId = menu + '-menu';
  if (!openDropdown(menuId)) {
    return;
  }
  const textId = menu + '-text';
  const spinnerId = menu + '-spinner';
  setSpinner(textId, spinnerId, true);
  let action = 'main';
  if (menu == FADECANDY_MENU) {
    action = 'leds';
  }
  const token = beginMenuRequest();
  sendCmd(action, target, menu).then((data) => {
    let menuData = '';
    const [state, level] = splitByDelimiter(cleanString(data), '~');
    if (isNumeric(state) === false || data == null) {
      menuData += menuErrorRow(menu);
    } else {
      menuData += powerMenuRows(menu, target, state, level);
    }
    setSpinner(textId, spinnerId, false);
    if (!menuRequestCurrent(token)) {
      return;
    }
    if (menuData === '') {
      removeDynMenus();
      return;
    }
    drawMenu(menuData, menuId);
  });
}
