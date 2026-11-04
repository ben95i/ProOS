// Automate - page entry. Draws the page for this device model, runs every button
// through one delegated click listener, and owns the page-level windows and teardown.
// by Ben Provenzano III

import { classDisplay, cleanString, base64URLSafeEncode, sendCmd,
  TIMEOUT_INTERVAL, setSpinner, setDisplayById, menuRow, registerListeners,
  dispatchRegisteredListener, gridButtonTemplate, dropdownButtonTemplate,
  GoToExtPage, onDemandModule, singleFlight } from './utils.js';
import { clearVolumePopupTimer } from './volctrl.js';
import { clearPopupTimer, installErrorHandlers } from './error.js';
import { configureMenus, removeDropdowns, openDropdown, drawMenu,
  destroyActiveSwitches, beginMenuRequest, menuRequestCurrent,
  showDynFileMenu, showPowerMenu, sendLeds } from './dropdowns.js';
import { LIVING_ROOM_CONFIG } from './livingrm.js';
import { BEDROOM_CONFIG } from './bedrm.js';
import { BATHROOM_CONFIG, closeBathroomSettings } from './bathrm.js';
import { serverOptionsTemplate, logFormTemplate, showLogWindow,
  closeLogWindow } from './server.js';
import RoomStateManager from './roomstate.js';
import AnimatedBackground from './stars.js';
import MainMenu from './mainmenu.js';
import WeatherWidget from './weather.js';

// The feature windows, loaded by their first use. A server page loads them once it is idle
// (preloadFeatureWindows()), so a press does not wait on the network; a Pi loads only what
// it opens.
const loadThermo = onDemandModule(() => import('./thermo.js'), 'the temperature window');
const loadPixels = onDemandModule(() => import('./pixels.js'), 'the Pixels window');
const loadColorPrompt = onDemandModule(() => import('./colorprompt.js'), 'the colour picker');
const loadLists = onDemandModule(() => import('./lists.js'), 'the lists window');
const loadBookmarks = onDemandModule(() => import('./bookmarks.js'), 'the bookmarks menu');
const loadCamera = onDemandModule(() => import('./camera.js'), 'the camera window');
const SERVER_PRELOADS = [loadThermo, loadPixels, loadColorPrompt, loadLists, loadBookmarks, loadCamera];
// the longest a server page waits for an idle moment before preloading them
const PRELOAD_WAIT_MS = 2000;

// openings in progress, so a second press while the module loads is not a second open
const openingWindows = { lists: null, bookmarks: null };
// bumped by closePopup(), so a camera still loading does not open over what came after
let cameraOpenCount = 0;

// The rooms, in room menu order; a browser with nothing saved opens on the first.
const ROOM_CONFIGS = {
  [LIVING_ROOM_CONFIG.key]: LIVING_ROOM_CONFIG,
  [BEDROOM_CONFIG.key]: BEDROOM_CONFIG,
  [BATHROOM_CONFIG.key]: BATHROOM_CONFIG
};

let serverMode = false;
window.App = {
  starBackground: null,
  roomStateManager: null,
  weatherWidget: null,
  uploader: null,
  lists: null,
  bookmarks: null,
  cameraView: null,
  colorPrompt: null,
  bathroomSettings: null,
  thermoWindow: null,
  pixelsWindow: null,
  mainMenu: null,
  activeSwitches: [],
  dynMenuNodes: [],
  sysModel: null,
  activityTimeout: null,
  ledScrollData: [
    '00FF00', '8x8', '12.0'
  ]
};

// entry point
window.addEventListener("DOMContentLoaded", () => {
  installErrorHandlers();
  loadPage();
  loadFeatures();
});

// reads the model from the title and draws its markup
function loadPage() {
  App.sysModel = document.getElementById('title').textContent;
  document.body.insertAdjacentHTML('afterbegin', buildPageHTML(App.sysModel));
}

// starts what the page runs: the server's services, the main menu, stars and timers
function loadFeatures() {
  if (serverMode) serverFeatures();
  App.mainMenu = new MainMenu({ isServer: serverMode });
  App.mainMenu.load();
  App.starBackground = new AnimatedBackground('star-canvas', starsAnimationEnabled());
  App.starBackground.activate();
  document.addEventListener('click', handleClicks);
  resetActivityTimer();
}

// starts the server page's own services: the last room, its status, the weather
function serverFeatures() {
  selectRoom(localStorage.getItem("ctls-mode"));
  App.roomStateManager = new RoomStateManager('status_anchor');
  App.weatherWidget = new WeatherWidget();
  App.weatherWidget.activate();
  // the Pixels window reaches every LED host through the server's API
  configureMenus({ openPixels: openPixelsWindow });
  preloadFeatureWindows();
}

// loads the server page's windows ahead of their first use, once the page has gone idle
function preloadFeatureWindows() {
  const preloadAll = () => {
    SERVER_PRELOADS.forEach(loadModule => loadModule.preload());
  };
  if (window.requestIdleCallback) {
    window.requestIdleCallback(preloadAll, { timeout: PRELOAD_WAIT_MS });
  } else {
    setTimeout(preloadAll, PRELOAD_WAIT_MS);
  }
}

// opens the Pixels window: the More... row of the Control menu
async function openPixelsWindow() {
  const pixelsModule = await loadPixels();
  if (pixelsModule) {
    pixelsModule.openPixelsWindow();
  }
}

// Index.html is served as written, so the server's title is SERVER_MODEL. A Pi's installer
// swaps it for its brand.txt name: a LED Pi when that holds "led", a plain Pi otherwise.
const SERVER_MODEL = 'Automate';

// the whole page's markup for a model
function buildPageHTML(model) {
  const modelName = model.trim();
  serverMode = modelName === SERVER_MODEL;
  const ledPi = !serverMode && modelName.toLowerCase().includes('led');
  const select = {
    isServer: serverMode,
    isLedCapable: serverMode || ledPi
  };
  return [
    '<canvas id="star-canvas"></canvas>',
    topNavTemplate(select),
    navDropdownsTemplate(),
    '<p class="pg-top"></p>',
    mainGridTemplate(select),
    serverOptionsTemplate(select),
    textScrollTemplate(select),
    logFormTemplate(),
    '<div id="color-popup"></div>',
    cameraPopupTemplate(),
    '<p class="pg-bottom"></p>'
  ].filter(Boolean).join('\n');
}

// Every click: restarts the activity timer, closes menus unless it landed on a button,
// a bookmark or a .nohide element, then runs the button's handler. While a bookmark is
// being edited, only its window takes clicks.
function handleClicks(event) {
  resetActivityTimer();
  if (App.bookmarks && App.bookmarks.isEditMode()) {
    if (!event.target.closest(".editFav__win")) {
      return;
    }
  } else if (!event.target.closest('.button, .bookmarked__item, .nohide')) {
    removeDropdowns();
  }
  dispatchRegisteredListener(event.target);
}

//// stars animation setting ////

// on unless explicitly "off", so a browser with nothing saved gets the animation
function starsAnimationEnabled() {
  return localStorage.getItem("stars-anim") !== "off";
}

// saves the stars setting and applies it
function setStarsAnimation(on) {
  let saved = "off";
  if (on === true) {
    saved = "on";
  }
  localStorage.setItem("stars-anim", saved);
  if (App.starBackground) {
    App.starBackground.setEnabled(on === true);
  }
}

// Browser-side settings for 'chk' menu rows, by the name in the row's first column.
const WWW_SETTINGS = {
  'stars': {
    get: () => starsAnimationEnabled(),
    set: (on) => setStarsAnimation(on)
  }
};

//// activity timeout ////

// restarts the timer that pauses the page's services when idle
function resetActivityTimer() {
  activityBegin();
  clearTimeout(App.activityTimeout);
  App.activityTimeout = setTimeout(activityStop, TIMEOUT_INTERVAL * 1000);
}

// resumes the services on activity
function activityBegin() {
  if (App.starBackground) {
    App.starBackground.resume();
  }
  if (App.roomStateManager) {
    App.roomStateManager.showRoomState();
  }
}

// pauses the services when idle
function activityStop() {
  if (App.starBackground) {
    App.starBackground.pause();
  }
  if (App.roomStateManager) {
    App.roomStateManager.hideRoomState();
  }
}

//// dropdowns ////

// closes the menus other modules own, when removeDropdowns() closes the rest
function closePageMenus() {
  if (App.bookmarks) {
    App.bookmarks.close();
  }
  if (App.weatherWidget) {
    App.weatherWidget.closeMenu();
  }
}

// a room menu row was picked: draws the room, reads its devices and any open temperature window
function pickRoom(roomKey) {
  selectRoom(roomKey);
  if (App.roomStateManager) {
    App.roomStateManager.restartRoomState();
  }
  // not loaded means not open
  const thermoModule = loadThermo.loaded();
  if (thermoModule) {
    thermoModule.refreshThermoWindow();
  }
}

// what dropdowns.js's rows and close need from the page
configureMenus({
  closePageMenus: closePageMenus,
  roomConfig: (roomKey) => ROOM_CONFIGS[roomKey],
  onRoomPicked: pickRoom,
  openTextScroll: openTextScroll,
  browserSetting: (settingName) => WWW_SETTINGS[settingName]
});

// the room selector: a menu of the other rooms
function openRoomDropdown() {
  const menuname = 'main-room-dropdown';
  if (!openDropdown(menuname)) {
    return;
  }
  let menuDataString = "";
  for (const [roomKey, config] of Object.entries(ROOM_CONFIGS)) {
    if (roomKey !== App.currentRoom) {
      menuDataString += menuRow(roomKey, 'room', config.name);
    }
  }
  drawMenu(menuDataString, menuname);
}

// Puts a room's icon and name on an icon and a label, either of which may be missing.
function showRoomIdentity(iconId, labelId, config) {
  const icon = document.getElementById(iconId);
  if (icon) {
    icon.className = config.icon || '';
    icon.style.display = config.icon ? 'inline-block' : 'none';
  }
  const label = document.getElementById(labelId);
  if (label) label.textContent = config.name;
}

// draws a room's grid and saves the choice; an unknown room falls back to the first
function selectRoom(_mode) {
  if (!ROOM_CONFIGS[_mode]) {
    _mode = Object.keys(ROOM_CONFIGS)[0];
  }
  const config = ROOM_CONFIGS[_mode];
  const container = document.getElementById('room-grid-container');
  if (container) {
    // closes the outgoing room's menus (their switches hold window listeners)
    removeDropdowns();
    container.innerHTML = config.template();
  }
  showRoomIdentity('current-room-icon', 'current-room-label', config);
  showRoomIdentity('room-group-icon', 'room-group-label', config);
  App.currentRoom = _mode;
  localStorage.setItem("ctls-mode", _mode);
}

// the Pi menu's Home row: back to the server's page
function GoToHomePage() {
  window.location = 'https://automate.home';
}

//// page templates ////

// the top bar
function topNavTemplate(f) {
  return `
<div class="topnav nohide">
  <div id="topnav-left">
    <a id="btn-mainmenu">
      <i class="fad fa-list"></i>
    </a>
    ${f.isServer ? `
    <a id="btn-show-bookmarks">
      <i class="fad fa-star"></i>
    </a>
    <a id="status_anchor">
    </a>` : ''}
  </div>
  ${f.isServer ? `
  <div id="room_center">
    <a id="room_anchor">
      <span id="current-room-label"></span>
      <i id="current-room-icon"></i>
    </a>
  </div>
  <div id="topnav-right">
    <a id="weather_anchor">
    </a>
    <a id="nav_stats_menu">
      <i id="navstats-icon" class="fad fa-chart-network"></i>
      <i id="navstats-spinner"></i>
    </a>
  </div>` : ''}
</div>`;
}
registerListeners({
  'btn-mainmenu':        () => App.mainMenu && App.mainMenu.showMenu(),
  'btn-show-bookmarks':  () => showBookmarks(),
  'room_anchor':         () => openRoomDropdown(),
  'btn-add-bookmark':    () => App.bookmarks && App.bookmarks.addBookmark(),
  'nav_stats_menu':      () => showStatusMenu()
});

// The menus' containers; MainMenu fills mainmenu with its own rows.
function navDropdownsTemplate() {
  return `
<div class="nav_dd_left">
  <div class="dd-content dd-menu" id="mainmenu"></div>
  <div class="dd-content dd-menu" id="bookmarks"></div>
  <div class="dd-content dd-menu" id="roomstatus"></div>
  <div class="dd-content dd-menu" id="main-room-dropdown"></div>
</div>
<div class="nav_dd_right">
  <div class="dd-content dd-menu ind-button" id="statsmenu"></div>
</div>`;
}
registerListeners({
  'btn-about':   () => App.mainMenu && App.mainMenu.aboutPrompt(),
  'btn-log-pi':  () => openLogWindow(),
  'btn-home-pi': () => GoToHomePage()
});

// the main grid (server + LED Pi): Pixels, then the server-only APT and room groups
function mainGridTemplate(f) {
  if (!f.isLedCapable) return '';
  return `
<div>
  <div class="grid">
    <fieldset class="btn-group">
      <div class="btn-group-label"><i class="fad fa-stars"></i><span>Pixels</span></div>
      ${gridButtonTemplate({
        buttonId: 'btn-ledcolorprompt',
        icon: 'fad fa-palette',
        label: 'Colors'
      })}
      ${dropdownButtonTemplate({
        buttonId: 'btn-power-fc_leds',
        icon: 'fad fa-stars',
        label: 'Control',
        textId: 'fc_leds-text',
        spinnerId: 'fc_leds-spinner',
        menuId: 'fc_leds-menu',
        menuContent: `
          <a id="btn-cmd-leds-pause"> Pause<i class="fad fa-pause"></i></a>
          <a id="btn-cmd-leds-off"> Off<i class="fad fa-toggle-off"></i></a>`
      })}
      ${dropdownButtonTemplate({
        buttonId: 'btn-ledeffectmenu',
        icon: 'fad fa-film',
        label: 'Animated',
        menuId: 'ledeffectmenu',
        menuClass: 'dd-button'
      })}
    </fieldset>
    ${f.isServer ? roomControlsTemplate() : ''}
    ${f.isServer ? `
    <fieldset class="btn-group" id="room-group">
      <div class="btn-group-label"><i id="room-group-icon"></i><span id="room-group-label"></span></div>
      <div id="room-grid-container"></div>
    </fieldset>` : ''}
  </div>
</div>`;
}
registerListeners({
  'btn-ledeffectmenu':     () => showDynFileMenu('ledeffectmenu', true),
  'btn-ledcolorprompt':    () => openPixelColors(),
  'btn-power-fc_leds':     () => showPowerMenu('fc_leds'),
  'btn-cmd-leds-off':      () => sendLeds('stop'),
  'btn-cmd-leds-pause':    () => sendLeds('pause')
});

// the Colors button: the colour prompt for the LED pixels
async function openPixelColors() {
  const colorPromptModule = await loadColorPrompt();
  if (colorPromptModule) {
    colorPromptModule.openColorPrompt('leds', 'hex', 'Pixels', true);
  }
}

// the On and Off rows of the three APT menus
function onOffRowsTemplate(onRowId, offRowId) {
  return `
    <a id="${onRowId}"> On<i class="fad fa-toggle-on"></i></a>
    <a id="${offRowId}"> Off<i class="fad fa-toggle-off"></i></a>`;
}

// The room the temperature window reads: the one on screen when it has a sensor, or
// else the living room.
function climateRoomConfig() {
  const roomConfig = ROOM_CONFIGS[App.currentRoom];
  if (roomConfig && roomConfig.climate) {
    return roomConfig;
  }
  return LIVING_ROOM_CONFIG;
}

// the server-only APT group
function roomControlsTemplate() {
  return `
<div>
  <p class="grid-sep"></p>
  <fieldset class="btn-group">
  <div class="btn-group-label"><i class="fad fa-home"></i><span>APT</span></div>
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-allmenu',
    icon: 'fad fa-plug',
    label: 'Room',
    menuId: 'allmenu',
    menuClass: 'dd-button',
    menuContent: onOffRowsTemplate('btn-room-on', 'btn-room-off')
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-lightmenu',
    icon: 'fad fa-lightbulb-on',
    label: 'Lights',
    menuId: 'lightmenu',
    menuClass: 'dd-button',
    menuContent: onOffRowsTemplate('btn-cmd-lightson', 'btn-cmd-lightsoff')
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-pwrmenu',
    icon: 'fad fa-home',
    label: 'Power',
    menuId: 'pwrmenu',
    menuClass: 'dd-button',
    menuContent: onOffRowsTemplate('btn-power-on', 'btn-power-off')
  })}
  <p class="grid-sep"></p>
  ${gridButtonTemplate({
    buttonId: 'btn-yt-link',
    icon: 'fab fa-youtube',
    label: 'YouTube'
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-show-temphumid',
    label: `<i class="fad fa-temperature-hot large__icon"></i>
      <i class="fad fa-humidity large__icon"></i>`
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-media-link',
    icon: 'fad fa-mp3-player',
    label: 'Media'
  })}
  <p class="grid-sep"></p>
  ${gridButtonTemplate({
    buttonId: 'btn-open-cam',
    icon: 'fad fa-door-open',
    label: 'Doorway'
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-open-log-svr',
    icon: 'fad fa-terminal',
    label: 'Server'
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-lists',
    icon: 'fad fa-list-ol',
    label: 'Lists'
  })}
  </fieldset>
</div>`;
}
registerListeners({
  'btn-media-link':     () => GoToExtPage('media.home'),
  'btn-yt-link':        () => GoToExtPage('youtube.com'),
  'btn-lists':          () => showLists(),
  'btn-menu-allmenu':   () => openDropdown('allmenu'),
  'btn-menu-pwrmenu':   () => openDropdown('pwrmenu'),
  'btn-power-on':       () => sendCmd('main', 'allon'),
  'btn-power-off':      () => sendCmd('main', 'alloff'),
  'btn-room-on':        () => sendCmd('main', `${App.currentRoom}on`),
  'btn-room-off':       () => sendCmd('main', `${App.currentRoom}off`),
  'btn-show-temphumid': () => openTemperatureWindow(),
  'btn-menu-lightmenu': () => openDropdown('lightmenu'),
  'btn-cmd-lightson':   () => sendCmd('main', 'lightson'),
  'btn-cmd-lightsoff':  () => sendCmd('main', 'lightsoff'),
  'btn-open-cam':       () => openCamWindow(),
  'btn-open-log-svr':   () => openLogWindow()
});

// the temperature / humidity button
async function openTemperatureWindow() {
  const thermoModule = await loadThermo();
  if (thermoModule) {
    thermoModule.openThermoWindow(climateRoomConfig);
  }
}

// the text scroller's menus: [row id suffix, label, the value a row sets]
const TEXT_SCROLL_COLORS = [['1', 'Green', '00FF00'], ['2', 'Red', 'FF0000'], ['3', 'Blue', '0000FF'],
  ['4', 'Orange', 'FFA500'], ['5', 'Purple', 'A020F0'], ['6', 'Yellow', 'FFCC00'], ['7', 'White', 'FFFFFF']];
const TEXT_SCROLL_FONTS = [['8x8', 'Large', '8x8'], ['5x7', 'Small', '5x7'], ['3x5', 'Narrow', '3x5']];
const TEXT_SCROLL_SPEEDS = [['slowest', 'Slowest', '3.0'], ['slow', 'Slow', '9.0'], ['normal', 'Normal', '12.0'],
  ['fast', 'Fast', '15.0'], ['fastest', 'Fastest', '20.0']];
const TEXT_SCROLL_BUTTON_CLASS = 'button__sml';

// one of the text scroller's three small menu buttons, with its rows
function textScrollMenuTemplate(name, label, rowPrefix, rows) {
  let rowsMarkup = '';
  rows.forEach(([suffix, rowLabel]) => {
    rowsMarkup += `<a id="${rowPrefix}-${suffix}"> ${rowLabel}</a>`;
  });
  return dropdownButtonTemplate({
    buttonId: `btn-menu-textscroll${name}`,
    label: label,
    buttonClass: TEXT_SCROLL_BUTTON_CLASS,
    menuId: `textscroll${name}-menu`,
    menuClass: 'dd-button',
    menuContent: rowsMarkup
  });
}

// the handlers for one text scroller menu, whose rows set field `field` of ledScrollData
function textScrollMenuListeners(name, rowPrefix, rows, field) {
  const listeners = { [`btn-menu-textscroll${name}`]: () => openDropdown(`textscroll${name}-menu`) };
  rows.forEach(([suffix, , value]) => {
    listeners[`${rowPrefix}-${suffix}`] = () => setTextScrollField(field, value);
  });
  return listeners;
}

// the LED text scroller window (server + LED Pi)
function textScrollTemplate(f) {
  if (!f.isLedCapable) return '';
  return `
<div>
  <div class="textscroll__prompt prompt_win">
    <div id="textScrollTitle">TextScroll</div>
    <textarea class="scroll-bar-style" id="textScrollBox" spellcheck="false"></textarea>
    ${textScrollMenuTemplate('color', 'Colors', 'btn-tscolor', TEXT_SCROLL_COLORS)}
    ${textScrollMenuTemplate('font', 'Font', 'btn-tsfont', TEXT_SCROLL_FONTS)}
    ${textScrollMenuTemplate('speed', 'Speed', 'btn-tsspeed', TEXT_SCROLL_SPEEDS)}
    ${gridButtonTemplate({
      buttonId: 'btn-ts-close',
      icon: 'fad fa-times-octagon',
      buttonClass: TEXT_SCROLL_BUTTON_CLASS
    })}
    ${gridButtonTemplate({
      buttonId: 'btn-ts-clear',
      icon: 'fad fa-trash-alt',
      buttonClass: TEXT_SCROLL_BUTTON_CLASS
    })}
    ${gridButtonTemplate({
      buttonId: 'btn-ts-send',
      icon: 'fad fa-upload',
      buttonClass: TEXT_SCROLL_BUTTON_CLASS
    })}
  </div>
</div>`;
}
registerListeners({
  ...textScrollMenuListeners('color', 'btn-tscolor', TEXT_SCROLL_COLORS, 0),
  ...textScrollMenuListeners('font', 'btn-tsfont', TEXT_SCROLL_FONTS, 1),
  ...textScrollMenuListeners('speed', 'btn-tsspeed', TEXT_SCROLL_SPEEDS, 2),
  'btn-ts-close':             () => closeTextScroll(),
  'btn-ts-clear':             () => clearLEDsMessage(),
  'btn-ts-send':              () => sendTextScroll()
});

// the camera window, on the page from the start; CameraView fills and shows it
function cameraPopupTemplate() {
  return `
<div class="popup-win" id="cam-popup">
  <div class="popup-ctn">
    <div id="cam-stage">
      <video id="cam-video" muted autoplay playsinline disablepictureinpicture></video>
      <div id="cam-status"></div>
    </div>
    <div class="popup-actions">
      ${gridButtonTemplate({
        buttonId: 'btn-cam-close',
        icon: 'fad fa-times-octagon',
        buttonClass: 'button_left_txtwin'
      })}
      ${gridButtonTemplate({
        buttonId: 'btn-cam-fullscreen',
        label: 'Fullscreen',
        buttonClass: 'button_right_txtwin'
      })}
    </div>
  </div>
</div>`;
}

// the log and camera windows close together, as closePopup() closes both
registerListeners({
  'btn-log-close':      () => closePopup(),
  'btn-cam-close':      () => closePopup(),
  'btn-cam-fullscreen': () => App.cameraView.toggleFullscreen()
});

/////// LEDScroll ///////

// writes the scroller's settings into its title, in its colour
function setTextScrollTitle() {
  const title = document.getElementById('textScrollTitle');
  const [color, font, speed] = App.ledScrollData;
  title.style.color = '#' + color;
  title.textContent = "Textscroll " + font + ' ' + speed + 'x';
}

// empties the scroller's box
function clearTextScrollBox() {
  const textbox = document.getElementById('textScrollBox');
  textbox.placeholder = "Enter message";
  textbox.value = '';
}

// opens the scroller
function openTextScroll() {
  classDisplay("textscroll__prompt", "block");
  clearTextScrollBox();
  setTextScrollTitle();
}

// closes the scroller
function closeTextScroll() {
  classDisplay("textscroll__prompt", "none");
  clearTextScrollBox();
  removeDropdowns();
}

// empties the box and stops the LEDs
function clearLEDsMessage() {
  clearTextScrollBox();
  sendLeds('stop');
}

// a menu row: sets one of colour, font, speed
function setTextScrollField(field, value) {
  App.ledScrollData[field] = value;
  setTextScrollTitle();
}

// sends the box's text to the LEDs as base64 of message|color|font|speed
function sendTextScroll() {
  const textbox = document.getElementById('textScrollBox');
  const message = cleanString(textbox.value.replaceAll("|", "-"));
  if (message === null || message == "") {
    textbox.placeholder = "Message cannot be empty";
    return;
  }
  textbox.placeholder = "Sending message..";
  textbox.value = '';
  const [color, font, speed] = App.ledScrollData;
  const commands = `${message}|${color}|${font}|${speed}`;
  sendLeds('textscroll', base64URLSafeEncode(commands)).then((data) => {
    if (data === null || data == "") {
      textbox.placeholder = "Error sending message!";
    } else {
      textbox.placeholder = "Message sent";
    }
    setTimeout(function() {
      textbox.value = message;
    }, 1000);
  });
}

//// Lists Window ////

// opens the lists window, or closes it when open
function showLists() {
  removeDropdowns();
  singleFlight(openingWindows, 'lists', async () => {
    const listsModule = await loadLists();
    if (!listsModule) {
      return;
    }
    if (!App.lists) {
      App.lists = new listsModule.default();
    }
    App.lists.activate();
  });
}

//// Bookmarks Menu ////

// star button: toggles the bookmarks menu
function showBookmarks() {
  singleFlight(openingWindows, 'bookmarks', async () => {
    // a menu opened or a press elsewhere while it loaded stands
    const token = beginMenuRequest();
    const bookmarksModule = await loadBookmarks();
    if (!bookmarksModule || !menuRequestCurrent(token)) {
      return;
    }
    if (!App.bookmarks) {
      App.bookmarks = new bookmarksModule.default();
    }
    App.bookmarks.toggle();
  });
}

// the stats button: probes the network's hosts into its menu
function showStatusMenu() {
  const _menu = 'statsmenu';
  const iconId = 'navstats-icon';
  const spinnerId = 'navstats-spinner';
  const spinnerClass = 'right-nav-spinner';
  if (!openDropdown(_menu)) {
    setSpinner(iconId, spinnerId, false, spinnerClass);
    return;
  }
  setSpinner(iconId, spinnerId, true, spinnerClass);
  const token = beginMenuRequest();
  sendCmd('main', 'status').then((data) => {
    setSpinner(iconId, spinnerId, false, spinnerClass);
    if (!menuRequestCurrent(token)) { return; }
    drawMenu(data, _menu);
  });
}

// Server button, or System on the Pi models: the log window, over nothing else
function openLogWindow() {
  closePopup();
  showLogWindow();
}

// Doorway button: the camera's live stream
async function openCamWindow() {
  closePopup();
  const openCount = cameraOpenCount;
  const cameraModule = await loadCamera();
  if (!cameraModule || openCount !== cameraOpenCount) {
    return;
  }
  if (!App.cameraView) {
    App.cameraView = new cameraModule.default('doorway');
  }
  App.cameraView.open();
}

// closes the log and camera windows
function closePopup() {
  cameraOpenCount += 1;
  closeLogWindow();
  setDisplayById("cam-popup", "none");
  if (App.cameraView) {
    App.cameraView.close();
  }
}

//// teardown ////

// ends one service on App and drops it, if it is running
function endService(serviceName, endMethod) {
  const service = App[serviceName];
  if (service) {
    service[endMethod]();
    App[serviceName] = null;
  }
}

// Ends every running service, handing back any bookmark edit lock first.
App.destroyAll = function() {
  console.log("shutting down...");
  clearTimeout(App.activityTimeout);
  clearPopupTimer();
  clearVolumePopupTimer();
  if (App.bookmarks) App.bookmarks.unloadBeacon();
  destroyActiveSwitches();
  endService('roomStateManager', 'destroy');
  endService('starBackground', 'destroy');
  endService('lists', 'dispose');
  endService('uploader', 'destroy');
  endService('colorPrompt', 'destroy');
  closeBathroomSettings();
  // not loaded means not open
  const thermoModule = loadThermo.loaded();
  if (thermoModule) {
    thermoModule.closeThermoWindow();
  }
  const pixelsModule = loadPixels.loaded();
  if (pixelsModule) {
    pixelsModule.closePixelsWindow();
  }
  endService('bookmarks', 'close');
  endService('cameraView', 'close');
  endService('weatherWidget', 'remove');
};

window.addEventListener('beforeunload', () => {
  App.destroyAll();
});
