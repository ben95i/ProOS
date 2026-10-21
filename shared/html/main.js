// Automate Website - JavaScript Frontend
// by Ben Provenzano III
//
// Draws the page for this device model, wires every button through one
// delegated click listener, and owns the page-level windows, the popups and
// the page teardown.
//
// The rooms live in their own modules (livingrm.js, bedrm.js, bathrm.js),
// the Server window in server.js, the dropdown menus in dropdowns.js, the
// volume buttons the amp rooms share in volctrl.js, and what they share in
// utils.js.

import { classDisplay, cleanString, base64URLSafeEncode, sendCmd,
  timeoutInterval, html, promptWindow, onButtons,
  setSpinner, setDisplayById, menuRow, registerListeners,
  dispatchRegisteredListener, gridButtonTemplate,
  dropdownButtonTemplate } from './utils.js';
import { clearVolumePopupTimer } from './volctrl.js';
import { clearPopupTimer, installErrorHandlers } from './error.js';
import { configureMenus, removeDropdowns, openDropdown, drawMenu,
  destroyActiveSwitches, beginMenuRequest, menuRequestCurrent,
  showDynFileMenu, showPowerMenu, sendLeds } from './dropdowns.js';
import { LIVING_ROOM_CONFIG } from './livingrm.js';
import { BEDROOM_CONFIG } from './bedrm.js';
import { BATHROOM_CONFIG, closeBathroomSettings } from './bathrm.js';
import { openPixelsWindow, closePixelsWindow } from './pixels.js';
import { serverOptionsTemplate, logFormTemplate, showLogWindow,
  closeLogWindow } from './server.js';
import RoomStateManager from './roomstate.js';
import AnimatedBackground from './stars.js';
import { openColorPrompt } from './colorprompt.js';
import MainMenu from './mainmenu.js';
import WeatherWidget from './weather.js';
import Lists from './lists.js';
import Bookmarks from './bookmarks.js';
import CameraView from './camera.js';

// The rooms, in the order the room menu lists them; the first is the one a
// browser with nothing saved opens on. Each room's module owns its config:
// its key, name, icon, volume target and grid template.
const ROOM_CONFIGS = {
  [LIVING_ROOM_CONFIG.key]: LIVING_ROOM_CONFIG,
  [BEDROOM_CONFIG.key]: BEDROOM_CONFIG,
  [BATHROOM_CONFIG.key]: BATHROOM_CONFIG
};

let serverMode = false;
window.App = {
  starBackground: null,
  roomStateManager: null,
  uploader: null,
  lists: null,
  bookmarks: null,
  cameraView: null,
  colorPrompt: null,
  bathroomSettings: null,
  pixelsWindow: null,
  mainMenu: null,
  activeSwitches: [],
  fileData: [],
  dynMenuNodes: [],
  dynMenuActive: 0,
  sysModel: null,
  activityTimeout: null,
  listenersReady: false,
  ledScrollData: [
    '00FF00', '8x8', '12.0'
  ]
};

// main entry point
window.addEventListener("DOMContentLoaded", () => {
  installErrorHandlers();
  loadPage();
  loadFeatures();
});

function loadPage() {
  App.sysModel = document.getElementById('title').textContent;
  destroyPageServices();
  renderPage(App.sysModel);
}

function loadFeatures() {
  if (serverMode) serverFeatures();
  App.mainMenu = new MainMenu({ isServer: serverMode });
  App.mainMenu.load();
  App.starBackground = new AnimatedBackground('star-canvas', starsAnimationEnabled());
  App.starBackground.activate();
  setupEventListeners();
  resetActivityTimer();
}

function serverFeatures() {
  selectRoom(localStorage.getItem("ctls-mode"));
  App.roomStateManager = new RoomStateManager('status_anchor');
  window.weatherWidget = new WeatherWidget();
  window.weatherWidget.activate();
  // the Pixels window reaches every LED host through the server's API, so
  // only the server's Control menu offers it
  configureMenus({ openPixels: openPixelsWindow });
}

// injects the markup for the given model, replacing any previous render
function renderPage(model) {
  removeInjectedPage();
  document.body.insertAdjacentHTML('afterbegin', buildPageHTML(model));
}

// The server serves index.html as written, so its title is SERVER_MODEL.
// Every Pi's installer swaps that title for its brand.txt name ("Bedroom",
// "LEDwall", "RaspberryPi" without one), so any other title is a Pi: a LED
// Pi when it holds "led", a plain Pi otherwise -- whatever else it says.
const SERVER_MODEL = 'Automate';

function buildPageHTML(model) {
  const modelName = model.trim();
  serverMode = modelName === SERVER_MODEL;
  const ledPi = !serverMode && modelName.toLowerCase().includes('led');
  const select = {
    isServer: serverMode,
    isLedCapable: serverMode || ledPi,
    isPiLike: !serverMode
  };
  return [
    '<canvas id="star-canvas" data-app-injected></canvas>',
    topNavTemplate(select),
    navDropdownsTemplate(select),
    '<p data-app-injected></p>',
    mainGridTemplate(select),
    serverOptionsTemplate(select),
    textScrollTemplate(select),
    logFormTemplate(),
    '<div id="color-popup" data-app-injected></div>',
    camPopupTemplate(),
    '<p class="pg-bottom" data-app-injected></p>'
  ].filter(Boolean).join('\n');
}

function setupEventListeners() {
  if (App.listenersReady) return;
  App.listenersReady = true;
  document.addEventListener('click', handleClicks, false);
  // button dispatcher for every group registered next to its template
  document.addEventListener('click', (event) => {
    dispatchRegisteredListener(event.target);
  });
}

// runs on each click anywhere on the page
function handleClicks(event) {
  resetActivityTimer();
  // no click events while the bookmark edit window is up
  if (App.bookmarks && App.bookmarks.isEditMode()) {
    // closest() rather than a className compare: drawPrompt() adds
    // "prompt_win" after setting the class, so an equality test never matched
    if (!event.target.closest(".editFav__win")) {
      // the button dispatcher is a second listener on document, and
      // stopPropagation() does not stop listeners on the same node -- so
      // delegated buttons used to keep firing right through edit mode
      event.stopImmediatePropagation();
    }
    return;
  }
  // don't hide menus when clicking these elements
  const NO_HIDE_ELEMENTS = ['.button', '.bookmarked__item', '.nohide'].join(', ');
  if (event.target.closest(NO_HIDE_ELEMENTS)) return;
  removeDropdowns();
}

// removes every node injected by renderPage() -- full DOM cleanup
function removeInjectedPage() {
  document.querySelectorAll('[data-app-injected]').forEach((node) => {
    node.remove();
  });
}

//// stars animation setting ////

// anything other than an explicit "off" is on, so a browser with nothing
// saved yet gets the animation
function starsAnimationEnabled() {
  return localStorage.getItem("stars-anim") !== "off";
}

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

// Checkbox rows drawn by drawMenu(). A menu file lives on the device, so a
// browser-side setting has nowhere in it to live -- these are keyed by the
// name in the row's first column and supply their own read and write.
const WWW_SETTINGS = {
  'stars': {
    get: () => starsAnimationEnabled(),
    set: (on) => setStarsAnimation(on)
  }
};

//// activity timeout ////

function resetActivityTimer() {
  activityBegin();
  clearTimeout(App.activityTimeout);
  App.activityTimeout = setTimeout(() => {
    activityStop();
  }, timeoutInterval() * 1000);
}

function activityBegin() {
  if (App.starBackground) {
    App.starBackground.resume();
  }
  if (serverMode && App.roomStateManager) {
    App.roomStateManager.showRoomState();
  }
}

function activityStop() {
  if (App.starBackground) {
    App.starBackground.pause();
  }
  if (serverMode && App.roomStateManager) {
    App.roomStateManager.hideRoomState();
  }
}

async function refreshRoomState() {
  if (serverMode && App.roomStateManager) {
    App.roomStateManager.restartRoomState();
  }
}

//// dropdowns ////

// Closes the menus other modules own, when removeDropdowns() closes the rest.
function closePageMenus() {
  if (!serverMode) {
    return;
  }
  if (App.bookmarks) {
    App.bookmarks.close();
  }
  if (window.weatherWidget && typeof window.weatherWidget.closeMenu === 'function') {
    window.weatherWidget.closeMenu();
  }
}

// a row of the room menu was picked: draw that room and read its devices
function pickRoom(roomKey) {
  selectRoom(roomKey);
  refreshRoomState();
}

// dropdowns.js sits below this module and cannot import from it, so what its
// rows and its close need from the page is handed over here
configureMenus({
  closePageMenus: closePageMenus,
  roomConfig: (roomKey) => ROOM_CONFIGS[roomKey],
  onRoomPicked: pickRoom,
  openTextScroll: openTextScroll,
  browserSetting: (settingName) => WWW_SETTINGS[settingName]
});

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

// Puts a room's icon and name on an icon <i> and a label element, either
// of which may be missing; a room without an icon hides the <i>.
// textContent rather than innerText: it sets the text without making the
// browser lay the page out first.
function showRoomIdentity(iconId, labelId, config) {
  const icon = document.getElementById(iconId);
  if (icon) {
    icon.className = config.icon || '';
    icon.style.display = config.icon ? 'inline-block' : 'none';
  }
  const label = document.getElementById(labelId);
  if (label) label.textContent = config.name;
}

function selectRoom(_mode) {
  if (!ROOM_CONFIGS[_mode]) {
    _mode = Object.keys(ROOM_CONFIGS)[0]; // load the first if not defined
  }
  const config = ROOM_CONFIGS[_mode];
  const container = document.getElementById('room-grid-container');
  if (container) {
    // the outgoing room's markup is about to go: any of its menus that is
    // open is torn down first (a toggle switch in one holds listeners on
    // window until destroyed), and a request still in flight for one of
    // them is stopped from drawing when it lands
    removeDropdowns();
    // all of the grid's buttons (volume included) are handled by the
    // delegated dispatcher, so rendering the markup is all that's needed
    container.innerHTML = config.template();
  }
  // the nav and the room group's label show the same icon and name
  showRoomIdentity('current-room-icon', 'current-room-label', config);
  showRoomIdentity('room-group-icon', 'room-group-label', config);
  App.currentRoom = _mode;
  localStorage.setItem("ctls-mode", _mode);
}

// open URL in new tab
function GoToExtPage(_path) {
  window.open("https://" + _path, "_blank");
}

// back to home page
function GoToHomePage() {
  if (serverMode) {
    removeInjectedPage();
    loadPage();
  } else {
    window.location = 'https://automate.home';
  }
}

//// page templates ////

function topNavTemplate(f) {
  return `
<div class="topnav nohide" data-app-injected>
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

// mainmenu is left empty here: its rows are written out in MainMenu.MENU_HTML
// (server) and PI_MENU_HTML (Pi), in the order they are drawn, so each menu
// can be reordered in one place. The handlers for those ids are still
// registered below.
function navDropdownsTemplate(f) {
  return `
<div class="nav_dd_left" data-app-injected>
  <div class="dd-content dd-menu" id="mainmenu"></div>
  <div class="dd-content dd-menu" id="bookmarks"></div>
  <div class="dd-content dd-menu" id="roomstatus"></div>
  <div class="dd-content dd-menu" id="main-room-dropdown"></div>
</div>
<div class="nav_dd_right" data-app-injected>
  <div class="dd-content dd-menu ind-button" id="statsmenu"></div>
</div>`;
}
registerListeners({
  'btn-about':   () => App.mainMenu && App.mainMenu.aboutPrompt(),
  'btn-log-pi':  () => openLogWindow(),
  'btn-home-pi': () => GoToHomePage()
});

// main control grid (server + LEDpi): the groups stack in one centred column,
// Pixels first, then the server-only APT and room groups under it
function mainGridTemplate(f) {
  if (!f.isLedCapable) return '';
  return `
<div data-app-injected>
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
  'btn-ledcolorprompt':    () => openColorPrompt('leds', 'hex', 'Pixels', true),
  'btn-power-fc_leds':     () => showPowerMenu('fc_leds'),
  'btn-cmd-leds-off':      () => sendLeds('stop'),
  'btn-cmd-leds-pause':    () => sendLeds('pause')
});

// the On and Off rows the three APT menus hold from the start
function onOffRowsTemplate(onRowId, offRowId) {
  return `
    <a id="${onRowId}"> On<i class="fad fa-toggle-on"></i></a>
    <a id="${offRowId}"> Off<i class="fad fa-toggle-off"></i></a>`;
}

// The temperature / humidity button is written out here: it is the one
// button with two icons and no label, which gridButtonTemplate() does not draw.
function temperatureButtonTemplate() {
  return `
  <button type="button" class="button" id="btn-show-temphumid">
    <span class="button__text">
      <i class="fad fa-temperature-hot large__icon"></i>
      <i class="fad fa-humidity large__icon"></i>
    </span>
  </button>`;
}

// server-only APT group, between Pixels and the room group in the main grid
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
  ${temperatureButtonTemplate()}
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
  'btn-show-temphumid': () => showTempHumidity(),
  'btn-menu-lightmenu': () => openDropdown('lightmenu'),
  'btn-cmd-lightson':   () => sendCmd('main', 'lightson'),
  'btn-cmd-lightsoff':  () => sendCmd('main', 'lightsoff'),
  'btn-open-cam':       () => openCamWindow(),
  'btn-open-log-svr':   () => openLogWindow()
});

// the text scroller's six buttons are the small size
const TEXT_SCROLL_BUTTON_CLASS = 'button__sml';

// LED text scroller prompt (server + LEDpi)
function textScrollTemplate(f) {
  if (!f.isLedCapable) return '';
  return `
<div data-app-injected>
  <div class="textscroll__prompt prompt_win">
    <div id="textScrollTitle">TextScroll</div>
    <textarea class="scroll-bar-style" id="textScrollBox" spellcheck="false"></textarea>
    ${dropdownButtonTemplate({
      buttonId: 'btn-menu-textscrollcolor',
      label: 'Colors',
      buttonClass: TEXT_SCROLL_BUTTON_CLASS,
      menuId: 'textscrollcolor-menu',
      menuClass: 'dd-button',
      menuContent: `
        <a id="btn-tscolor-1"> Green</a>
        <a id="btn-tscolor-2"> Red</a>
        <a id="btn-tscolor-3"> Blue</a>
        <a id="btn-tscolor-4"> Orange</a>
        <a id="btn-tscolor-5"> Purple</a>
        <a id="btn-tscolor-6"> Yellow</a>
        <a id="btn-tscolor-7"> White</a>`
    })}
    ${dropdownButtonTemplate({
      buttonId: 'btn-menu-textscrollfont',
      label: 'Font',
      buttonClass: TEXT_SCROLL_BUTTON_CLASS,
      menuId: 'textscrollfont-menu',
      menuClass: 'dd-button',
      menuContent: `
        <a id="btn-tsfont-8x8"> Large</a>
        <a id="btn-tsfont-5x7"> Small</a>
        <a id="btn-tsfont-3x5"> Narrow</a>`
    })}
    ${dropdownButtonTemplate({
      buttonId: 'btn-menu-textscrollspeed',
      label: 'Speed',
      buttonClass: TEXT_SCROLL_BUTTON_CLASS,
      menuId: 'textscrollspeed-menu',
      menuClass: 'dd-button',
      menuContent: `
        <a id="btn-tsspeed-slowest"> Slowest</a>
        <a id="btn-tsspeed-slow"> Slow</a>
        <a id="btn-tsspeed-normal"> Normal</a>
        <a id="btn-tsspeed-fast"> Fast</a>
        <a id="btn-tsspeed-fastest"> Fastest</a>`
    })}
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
  'btn-menu-textscrollcolor': () => openDropdown('textscrollcolor-menu'),
  'btn-tscolor-1':            () => setTextScrollColor('00FF00'),
  'btn-tscolor-2':            () => setTextScrollColor('FF0000'),
  'btn-tscolor-3':            () => setTextScrollColor('0000FF'),
  'btn-tscolor-4':            () => setTextScrollColor('FFA500'),
  'btn-tscolor-5':            () => setTextScrollColor('A020F0'),
  'btn-tscolor-6':            () => setTextScrollColor('FFCC00'),
  'btn-tscolor-7':            () => setTextScrollColor('FFFFFF'),
  'btn-menu-textscrollfont':  () => openDropdown('textscrollfont-menu'),
  'btn-tsfont-8x8':           () => setTextScrollFont('8x8'),
  'btn-tsfont-5x7':           () => setTextScrollFont('5x7'),
  'btn-tsfont-3x5':           () => setTextScrollFont('3x5'),
  'btn-menu-textscrollspeed': () => openDropdown('textscrollspeed-menu'),
  'btn-tsspeed-slowest':      () => setTextScrollSpeed('3.0'),
  'btn-tsspeed-slow':         () => setTextScrollSpeed('9.0'),
  'btn-tsspeed-normal':       () => setTextScrollSpeed('12.0'),
  'btn-tsspeed-fast':         () => setTextScrollSpeed('15.0'),
  'btn-tsspeed-fastest':      () => setTextScrollSpeed('20.0'),
  'btn-ts-close':             () => closeTextScroll(),
  'btn-ts-clear':             () => clearLEDsMessage(),
  'btn-ts-send':              () => sendTextScroll()
});

// the log window's close button closes the camera window with it, so it is
// handled here beside the camera's own
registerListeners({
  'btn-log-close': () => closePopup()
});

// the camera window's markup lives with the class that plays it (camera.js)
function camPopupTemplate() {
  return CameraView.POPUP_HTML;
}
registerListeners({
  'btn-cam-close':      () => closePopup(),
  'btn-cam-fullscreen': () => toggleCamFullscreen()
});

/////// LEDScroll ///////

function setTextScrollTitle() {
  const title = document.getElementById('textScrollTitle');
  const [color, font, speed] = App.ledScrollData;
  title.style.color = '#' + color;
  title.textContent = "Textscroll " + font + ' ' + speed + 'x';
}

function clearTextScrollBox() {
  const textbox = document.getElementById('textScrollBox');
  textbox.placeholder = "Enter message";
  textbox.value = '';
}

function openTextScroll() {
  classDisplay("textscroll__prompt", "block");
  clearTextScrollBox();
  setTextScrollTitle();
}

function closeTextScroll() {
  classDisplay("textscroll__prompt", "none");
  clearTextScrollBox();
  removeDropdowns();
}

function clearLEDsMessage() {
  clearTextScrollBox();
  sendLeds('stop');
}

function setTextScrollColor(color) {
  App.ledScrollData[0] = color;
  setTextScrollTitle();
}

function setTextScrollFont(font) {
  App.ledScrollData[1] = font;
  setTextScrollTitle();
}

function setTextScrollSpeed(speed) {
  App.ledScrollData[2] = speed;
  setTextScrollTitle();
}

// sends the box's text to the LED grid as message|color|font|speed, base64
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

// opens the lists window (clicking again closes it); the window keeps its own
// state and talks to the same read / write file API the bookmarks use
function showLists() {
  removeDropdowns();
  if (!App.lists) {
    App.lists = new Lists();
  }
  App.lists.activate();
}

//// Bookmarks Menu ////

// star button: the Bookmarks class owns the menu, its buttons, the add/edit
// window and the file
function showBookmarks() {
  if (!App.bookmarks) {
    App.bookmarks = new Bookmarks({
      removeDropdowns: removeDropdowns
    });
  }
  App.bookmarks.toggle();
}

/// Temperature & Humidity ///

// thermometer limits
const TEMP_MIN = 25;
const TEMP_MAX = 100;
const HUMIDITY_MIN = 5;
const HUMIDITY_MAX = 100;

// the temperature window's two buttons
const TEMP_CLOSE_BUTTON_ID = 'temp__btn-close';
const TEMP_REFRESH_BUTTON_ID = 'temp__btn-refresh';

function showTempHumidity() {
  const _winid = "temp__prompt";
  if (document.getElementById(_winid)) return;
  // banner text, thermometer grid, buttons row
  const tempprompt = promptWindow(_winid, html`
    <div id="temp_top_text">Temperature/Humidity</div>
    <div id="temp_therm_grid">
      <div class="thermometer">
        <div class="temperature" id="thermo__1" data-value="--&#176;"></div>
      </div>
      <div class="thermometer">
        <div class="temperature" id="thermo__2" data-value="--%"></div>
      </div>
    </div>
    <div>
      <button class="button temp__btn fad fa-times-octagon" type="button"
        id="${TEMP_CLOSE_BUTTON_ID}"></button>
      <button class="button temp__btn fad fa-sync" type="button"
        id="${TEMP_REFRESH_BUTTON_ID}"></button>
    </div>
  `);
  getTemperatureData();
  onButtons(tempprompt, (button) => {
    if (button.id === TEMP_CLOSE_BUTTON_ID) {
      tempprompt.remove();
    }
    if (button.id === TEMP_REFRESH_BUTTON_ID) {
      getTemperatureData();
    }
  });
}

// the API answers "72~41" -- temperature then humidity
function getTemperatureData() {
  sendCmd("main", "brxmit", "roomth")
    .then((data) => {
      const resp_arr = cleanString(data).split("~");
      const temp_elm = document.getElementById("thermo__1");
      const humd_elm = document.getElementById("thermo__2");
      if (resp_arr.length == 2) {
        pushTempDataToThermos(resp_arr, temp_elm, humd_elm);
      } else {
        pushTempErrorThermos(temp_elm, humd_elm);
      }
    })
    .catch(() => {
      pushTempErrorThermos(
        document.getElementById("thermo__1"),
        document.getElementById("thermo__2")
      );
    });
}

// how much of the tube one reading fills, as a percentage of its own range
function thermometerFill(value, minValue, maxValue) {
  let held = value;
  if (held >= maxValue) { held = maxValue; }
  if (held <= minValue) { held = minValue; }
  const aboveFloor = held - minValue;
  const fullRange = maxValue - minValue;
  return (aboveFloor / fullRange) * 100;
}

function pushTempDataToThermos(resp_arr, temp_elm, humd_elm) {
  if (!(temp_elm && humd_elm)) {
    return;
  }
  if (resp_arr.length != 2) {
    pushTempErrorThermos(temp_elm, humd_elm);
    return;
  }
  const tvalue = resp_arr[0];
  const hvalue = resp_arr[1];
  temp_elm.dataset.value = `${tvalue}\u00B0F`;
  temp_elm.style.height = `${thermometerFill(tvalue, TEMP_MIN, TEMP_MAX)}%`;
  humd_elm.dataset.value = `${hvalue}%`;
  humd_elm.style.height = `${thermometerFill(hvalue, HUMIDITY_MIN, HUMIDITY_MAX)}%`;
}

function pushTempErrorThermos(temp_elm, humd_elm) {
  if (!(temp_elm && humd_elm)) {
    return;
  }
  temp_elm.style.height = "0%";
  temp_elm.dataset.value = "--";
  humd_elm.style.height = "0%";
  humd_elm.dataset.value = "--";
}

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

// Server button (and System on the Pi models): the log window, opened over
// nothing else
function openLogWindow() {
  closePopup();
  showLogWindow();
}

// Doorway button: the camera's live stream, relayed by the API
// (api/camstream.js) and played by CameraView
function openCamWindow() {
  closePopup();
  if (!App.cameraView) {
    App.cameraView = new CameraView('doorway');
  }
  App.cameraView.open();
}

function toggleCamFullscreen() {
  if (App.cameraView) {
    App.cameraView.toggleFullscreen();
  }
}

// close all popup windows
function closePopup() {
  closeLogWindow();
  setDisplayById("cam-popup", "none");
  if (App.cameraView) {
    App.cameraView.close();
  }
}

//// runs before exit ////

window.addEventListener('beforeunload', () => {
  App.destroyAll();
});

App.destroyAll = function() {
  console.log("shutting down...");
  if (App.activityTimeout) clearTimeout(App.activityTimeout);
  clearPopupTimer();
  clearVolumePopupTimer();
  if (App.bookmarks) App.bookmarks.unloadBeacon();
  destroyActiveSwitches();
  destroyPageServices();
};

function destroyPageServices() {
  if (App.roomStateManager) {
    App.roomStateManager.destroy();
    App.roomStateManager = null;
  }
  if (App.starBackground) {
    App.starBackground.destroy();
    App.starBackground = null;
  }
  if (App.lists) {
    App.lists.dispose();
    App.lists = null;
  }
  if (App.uploader) {
    App.uploader.destroy();
    App.uploader = null;
  }
  if (App.colorPrompt) {
    App.colorPrompt.destroy();
    App.colorPrompt = null;
  }
  closeBathroomSettings();
  closePixelsWindow();
  if (App.bookmarks) {
    App.bookmarks.close();
    App.bookmarks = null;
  }
  if (App.cameraView) {
    App.cameraView.destroy();
    App.cameraView = null;
  }
  if (window.weatherWidget && typeof window.weatherWidget.remove === 'function') {
    window.weatherWidget.remove();
    window.weatherWidget = null;
  }
}
