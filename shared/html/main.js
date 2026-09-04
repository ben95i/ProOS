// Automate Website - JavaScript Frontend
// by Ben Provenzano III
//
// Draws the page for this device model, wires every button through one
// delegated click listener, and owns the dynamic menus, the popups and the
// page teardown.

import { requestThrottle, classDisplay, addClassToElem, cleanString,
  base64URLSafeEncode, splitMenuData, isNumeric, mapNumber, splitByDelimiter,
  sendCmd, timeoutInterval, html, showPopup, clearPopupTimer, loadMenu,
  savePOST, fileURL, makeElement, promptWindow, onButtons, setSpinner,
  setDisplayById, isBlockDisplayed, menuRow, indicator } from './utils.js';
import ToggleSwitch, { toggleSwitchCallback } from './switch.js';
import RoomStateManager from './roomstate.js';
import AnimatedBackground from './stars.js';
import ColorPrompt from './colorprompt.js';
import MainMenu from './mainmenu.js';
import WeatherWidget from './weather.js';
import Lists from './lists.js';
import Bookmarks from './bookmarks.js';
import { deviceType } from './device.js';
import { SSIDpwd } from './wifi.js';

const MAIN_SITE = "Automate";
const ROOM_CONFIGS = {
  'lr': {
    name: 'Living Room',
    icon: '',
    voltarget: 'lrxmit',
    volmax: 100,
    template: livingRoomGridTemplate
  },
  'br': {
    name: 'Bedroom',
    icon: 'fad fa-bed',
    voltarget: 'brpi',
    volmax: 192,
    template: bedroomGridTemplate
  }
};
const SUBWOOFER_CLASS = 'red_btn_menu';
const VOLUME_ACTIONS = {
  'voldwnbtn': 'dwn',
  'volmutebtn': 'mute',
  'volupbtn': 'up'
};

const DELEGATED_LISTENERS = {};

window.App = {
  starBackground: null,
  roomStateManager: null,
  lists: null,
  bookmarks: null,
  colorPrompt: null,
  mainMenu: null,
  activeSwitches: [],
  fileData: [],
  dynMenuNodes: [],
  serverCmdData: null,
  dynMenuActive: 0,
  volPopupTimer: null,
  sysModel: null,
  activityTimeout: null,
  listenersReady: false,
  ledScrollData: [
    '00FF00', '8x8', '12.0'
  ]
};

// Every menu request stamps this before it goes out and checks it when it
// comes back. Opening another menu -- or closing the menus outright --
// invalidates whatever is still in the air, so a slow reply can no longer
// wipe the rows of the menu that is actually on screen.
let menuDrawToken = 0;

window.addEventListener("DOMContentLoaded", () => {
  loadPage();
});

// Each template below registers its own button handlers with
// registerListeners() directly beneath its markup; the groups merge into this
// one map, which the delegated dispatcher looks up on click. Delegation means
// handlers keep working for markup rendered later (room grids, bookmark
// buttons, svropt panels) and never need re-attaching after a re-render.
function registerListeners(listeners) {
  for (const [id, handler] of Object.entries(listeners)) {
    if (!(id in DELEGATED_LISTENERS)) {
      DELEGATED_LISTENERS[id] = handler;
    }
  }
}

function setupEventListeners() {
  if (App.listenersReady) {
    return;
  }
  App.listenersReady = true;
  document.addEventListener('click', handleClicks, false);
  // button dispatcher for every group registered next to its template
  document.addEventListener('click', (event) => {
    let current = event.target;
    while (current && current !== document) {
      if (current.id && DELEGATED_LISTENERS[current.id]) {
        DELEGATED_LISTENERS[current.id]();
        return;
      }
      current = current.parentNode;
    }
  });
}

function buildPageHTML(model) {
  const f = {
    isServer: model === MAIN_SITE,
    isPi: model === 'Pi',
    isLedPi: model === 'LEDpi'
  };
  f.isPiLike = f.isPi || f.isLedPi;         // Pi-style pages (Pi + LEDpi)
  f.isLedCapable = f.isServer || f.isLedPi; // pages with LED controls
  return [
    '<canvas id="star-canvas" data-app-injected></canvas>',
    topNavTemplate(f),
    navDropdownsTemplate(f),
    '<p data-app-injected></p>',
    mainGridTemplate(f),
    serverOptionsTemplate(f),
    textScrollTemplate(f),
    logFormTemplate(),
    '<div id="color-popup" data-app-injected></div>',
    camPopupTemplate(),
    '<p class="pg-bottom" data-app-injected></p>'
  ].filter(Boolean).join('\n');
}

// injects the markup for the given model, replacing any previous render
function renderPage(model) {
  removeInjectedPage();
  document.body.insertAdjacentHTML('afterbegin', buildPageHTML(model));
}

// removes every node injected by renderPage() -- full DOM cleanup
function removeInjectedPage() {
  document.querySelectorAll('[data-app-injected]').forEach((node) => {
    node.remove();
  });
}

function loadPage() {
  App.sysModel = deviceType();
  destroyPageServices();
  renderPage(App.sysModel);
  if (App.sysModel === MAIN_SITE) {
    // control menu saved state
    selectRoom(localStorage.getItem("ctls-mode"));
    App.roomStateManager = new RoomStateManager('status_anchor');
    window.weatherWidget = new WeatherWidget();
    window.weatherWidget.activate();
  }
  // the list button's menu, the theme colour and the About window; built
  // before the listeners below, which reach into it
  App.mainMenu = new MainMenu();
  App.mainMenu.load();
  setupEventListeners();
  // the saved animation setting is applied before the first frame, so a
  // switched-off field never flickers into motion
  App.starBackground = new AnimatedBackground('star-canvas', starsAnimationEnabled());
  App.starBackground.activate();
  resetActivityTimer();
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
  if (App.sysModel === MAIN_SITE && App.roomStateManager) {
    App.roomStateManager.showRoomState();
  }
}

function activityStop() {
  if (App.starBackground) {
    App.starBackground.pause();
  }
  if (App.sysModel === MAIN_SITE && App.roomStateManager) {
    App.roomStateManager.hideRoomState();
  }
}

async function refreshRoomState() {
  if (App.sysModel === MAIN_SITE && App.roomStateManager) {
    App.roomStateManager.restartRoomState();
  }
}

//// dropdowns ////

window.removeDropdowns = function() {
  // anything still in flight is no longer allowed to draw itself
  beginMenuRequest();
  classDisplay("dd-content", "none");
  removeDynMenus();
  if (App.sysModel === MAIN_SITE) {
    if (App.bookmarks) App.bookmarks.close();
    destroyActiveSwitches();
    if (window.weatherWidget && typeof window.weatherWidget.closeMenu === 'function') {
      window.weatherWidget.closeMenu();
    }
  }
}

// Opens a dropdown, or shuts it when it is already open. Answers true when
// the caller should go on and fill it.
function openDropdown(id) {
  const _elem = document.getElementById(id);
  if (!_elem) {
    return false;
  }
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
    return false;
  }
  removeDropdowns();
  _elem.style.display = 'block';
  return true;
}

// toggle switches own pointer listeners, so they are destroyed rather than
// just dropped whenever their markup goes away
function destroyActiveSwitches() {
  if (App.activeSwitches && App.activeSwitches.length > 0) {
    App.activeSwitches.forEach(toggle => {
      if (typeof toggle.destroy === 'function') toggle.destroy();
    });
    App.activeSwitches = [];
  }
}

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

function selectRoom(_mode) {
  if (!ROOM_CONFIGS[_mode]) {
    _mode = Object.keys(ROOM_CONFIGS)[0]; // load the first if not defined
  }
  const config = ROOM_CONFIGS[_mode];
  const container = document.getElementById('room-grid-container');
  if (container) {
    // all of the grid's buttons (volume included) are handled by the
    // delegated dispatcher, so rendering the markup is all that's needed
    container.innerHTML = config.template();
  }
  const roomLabel = document.getElementById('current-room-label');
  if (roomLabel) roomLabel.innerText = config.name;
  const roomIcon = document.getElementById('current-room-icon');
  if (roomIcon) {
    if (config.icon) {
      roomIcon.className = config.icon;
      roomIcon.style.display = 'inline-block';
    } else {
      roomIcon.className = '';
      roomIcon.style.display = 'none';
    }
  }
  App.currentRoom = _mode;
  localStorage.setItem("ctls-mode", _mode);
}

// open URL in new tab
function GoToExtPage(_path) {
  window.open("https://" + _path, "_blank");
}

// back to home page
function GoToHomePage() {
  if (App.sysModel === MAIN_SITE) {
    removeInjectedPage();
    loadPage();
  } else {
    window.location = 'https://' + MAIN_SITE + '.home';
  }
}

// toggle a static dropdown menu
function showMenu(_menu) {
  openDropdown(_menu);
}

//// page templates ////

function topNavTemplate(f) {
  return `
<div class="topnav nohide" data-app-injected>
  <div id="topnav-left">
    ${f.isServer ? `
    <a id="btn-mainmenu">
      <i class="fad fa-list"></i>
    </a>` : ''}
    ${f.isPiLike ? `
    <a id="btn-pimainmenu">
      <i class="fad fa-list"></i>
    </a>` : ''}
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
      <i id="current-room-icon"></i>
      <span id="current-room-label"></span>
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
  'btn-pimainmenu':      () => App.mainMenu && App.mainMenu.showPiMenu(),
  'btn-show-bookmarks':  () => showBookmarks(),
  'room_anchor':         () => openRoomDropdown(),
  'btn-add-bookmark':    () => App.bookmarks && App.bookmarks.addBookmark(),
  'nav_stats_menu':      () => showStatusMenu()
});

// mainmenu / pi-mainmenu are left empty here: their rows are written out in
// MainMenu.MENU_HTML / PI_MENU_HTML, in the order they are drawn, so each
// menu can be reordered in one place. The handlers for those ids are still
// registered below.
function navDropdownsTemplate(f) {
  return `
<div class="nav_dd_left" data-app-injected>
  ${f.isServer ? `
  <div class="dd-content dd-menu" id="mainmenu"></div>` : ''}
  ${f.isPiLike ? `
  <div class="dd-content dd-menu" id="pi-mainmenu"></div>` : ''}
  <div class="dd-content dd-menu" id="bookmarks"></div>
  <div class="dd-content dd-menu" id="roomstatus"></div>
  <div class="dd-content dd-menu" id="main-room-dropdown"></div>
</div>
<div class="nav_dd_right" data-app-injected>
  <div class="dd-content dd-menu ind-button" id="statsmenu"></div>
</div>`;
}
registerListeners({
  'btn-about-main': () => App.mainMenu && App.mainMenu.aboutPrompt(),
  'btn-about-pi':   () => App.mainMenu && App.mainMenu.aboutPrompt(),
  'btn-log-pi':     () => openLogWindow(),
  'btn-home-pi':    () => GoToHomePage()
});

// main control grid (server + LEDpi); the room power / temperature / lights /
// doorway / room selector block inside column-left is server-only
function mainGridTemplate(f) {
  if (!f.isLedCapable) return '';
  return `
<div data-app-injected>
  <div class="grid">
    <div class="column-left">
      <div class="dropdown">
        <button type="button" class="button" id="btn-dyn-ledcolormenu">
          <span class="button__text"><i class="fad fa-brush"></i> Colors</span>
        </button>
        <div class="dd-content dd-button" id="ledcolormenu">
          <a id="btn-fc-color-prompt"> Picker<i class="fad fa-palette"></i></a>
        </div>
      </div>
      <div class="dropdown">
        <button type="button" class="button" id="btn-power-fc_leds">
          <span class="button__text" id="fc_leds-text"><i class="fad fa-stars"></i> Pixels</span>
          <span class="button__text" id="fc_leds-spinner"></span>
        </button>
        <div class="dd-content ind-button" id="fc_leds-menu">
          <a id="btn-cmd-leds-stop"> Stop<i class="fad fa-stop"></i></a>
          <a id="btn-cmd-leds-pause"> Pause<i class="fad fa-pause"></i></a>
        </div>
      </div>
      <div class="dropdown">
        <button type="button" class="button" id="btn-dyn-ledeffectmenu">
          <span class="button__text"><i class="fad fa-film"></i> Animated</span>
        </button>
        <div class="dd-content dd-button" id="ledeffectmenu">
          <a id="btn-cmd-leds-shuffle"> Shuffle<i class="fad fa-random"></i></a>
        </div>
      </div>
      ${f.isServer ? roomControlsTemplate() : ''}
    </div>
    <div class="column-right" id="room-grid-container"></div>
  </div>
</div>`;
}
registerListeners({
  'btn-dyn-ledcolormenu':  () => showDynFileMenu('ledcolormenu'),
  'btn-fc-color-prompt':   () => openColorPrompt('leds', 'hex', true),
  'btn-dyn-ledeffectmenu': () => showDynFileMenu('ledeffectmenu'),
  'btn-power-fc_leds':     () => showPowerMenu('fc_leds'),
  'btn-cmd-leds-shuffle':  () => sendCmd('leds', 'shuffle'),
  'btn-cmd-leds-stop':     () => sendCmd('leds', 'stop'),
  'btn-cmd-leds-pause':    () => sendCmd('leds', 'pause')
});

// server-only block inside the main grid's left column
function roomControlsTemplate() {
  return `
<div>
  <p class="grid-sep"></p>
  <div class="dropdown">
    <button type="button" class="button" id="btn-menu-allmenu">
      <span class="button__text"><i class="fad fa-plug"></i> Room</span>
    </button>
    <div class="dd-content dd-button" id="allmenu">
      <a id="btn-room-on"> On<i class="fad fa-toggle-on"></i></a>
      <a id="btn-room-off"> Off<i class="fad fa-toggle-off"></i></a>
    </div>
  </div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-menu-lightmenu">
      <span class="button__text"><i class="fad fa-lightbulb-on"></i> Lights</span>
    </button>
    <div class="dd-content dd-button" id="lightmenu">
      <a id="btn-cmd-lightson"> On<i class="fad fa-toggle-on"></i></a>
      <a id="btn-cmd-lightsoff"> Off<i class="fad fa-toggle-off"></i></a>
    </div>
  </div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-menu-pwrmenu">
      <span class="button__text"><i class="fad fa-home"></i> Power</span>
    </button>
    <div class="dd-content dd-button" id="pwrmenu">
      <a id="btn-power-on"> On<i class="fad fa-toggle-on"></i></a>
      <a id="btn-power-off"> Off<i class="fad fa-toggle-off"></i></a>
    </div>
  </div>
  <p class="grid-sep"></p>
  <button type="button" class="button" id="btn-yt-link">
    <span class="button__text"><i class="fab fa-youtube"></i> YouTube</span>
  </button>
  <button type="button" class="button" id="btn-show-temphumid">
    <span class="button__text">
      <i class="fad fa-temperature-hot large__icon"></i>
      <i class="fad fa-humidity large__icon"></i>
    </span>
  </button>
  <button type="button" class="button" id="btn-media-link">
    <span class="button__text"><i class="fad fa-mp3-player"></i> Media</span>
  </button>
  <p class="grid-sep"></p>
  <button type="button" class="button" id="btn-open-cam">
    <span class="button__text"><i class="fad fa-door-open"></i> Doorway</span>
  </button>
  <button type="button" class="button" id="btn-open-log-svr">
    <span class="button__text"><i class="fad fa-terminal"></i> Server</span>
  </button>
  <button type="button" class="button" id="btn-lists">
    <span class="button__text"><i class="fad fa-list-ol"></i> Lists</span>
  </button>
</div>`;
}
registerListeners({
  'btn-media-link':     () => GoToExtPage('media.home'),
  'btn-yt-link':        () => GoToExtPage('youtube.com'),
  'btn-lists':          () => showLists(),
  'btn-menu-allmenu':   () => showMenu('allmenu'),
  'btn-menu-pwrmenu':   () => showMenu('pwrmenu'),
  'btn-power-on':       () => sendCmd('main', 'allon'),
  'btn-power-off':      () => sendCmd('main', 'alloff'),
  'btn-room-on':        () => sendCmd('main', `${App.currentRoom}on`),
  'btn-room-off':       () => sendCmd('main', `${App.currentRoom}off`),
  'btn-show-temphumid': () => showTempHumidity(),
  'btn-menu-lightmenu': () => showMenu('lightmenu'),
  'btn-cmd-lightson':   () => sendCmd('main', 'lightson'),
  'btn-cmd-lightsoff':  () => sendCmd('main', 'lightsoff'),
  'btn-open-cam':       () => openCamWindow(),
  'btn-open-log-svr':   () => openLogWindow()
});

function livingRoomGridTemplate() {
  return `
<div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-power-wl1_dual">
      <span class="button__text" id="wl1_dual-text"><i class="fad fa-lamp-floor"></i> Window</span>
      <span class="button__text" id="wl1_dual-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="wl1_dual-menu"></div>
  </div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-power-wkststate">
      <span class="button__text" id="wkststate-text"><i class="fad fa-desktop-alt"></i> Desktop</span>
      <span class="button__text" id="wkststate-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="wkststate-menu"></div>
  </div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-power-clr1">
      <span class="button__text" id="clr1-text"><i class="fad fa-light-ceiling"></i> Ceiling</span>
      <span class="button__text" id="clr1-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="clr1-menu"></div>
  </div>
  <p class="grid-sep"></p>
  <button type="button" class="button" id="lr-voldwnbtn">
      <span class="button__text"><i class="fad fa-volume-down fa-lg"></i></span>
  </button>
  <button type="button" class="button" id="lr-volmutebtn">
      <span class="button__text"><i class="fad fa-volume-mute fa-lg"></i></span>
  </button>
  <button type="button" class="button" id="lr-volupbtn">
      <span class="button__text"><i class="fad fa-volume-up fa-lg"></i></span>
  </button>
  <p class="grid-sep"></p>
  <button type="button" class="button" id="btn-vol-dwnc">
      <span class="button__text"><i class="fad fa-volume-down"></i> -5%</span>
  </button>
  <div class="dropdown">
    <button type="button" class="button" id="btn-power-hifistate">
      <span class="button__text" id="hifistate-text"><i class="fad fa-speakers"></i> Stereo</span>
      <span class="button__text" id="hifistate-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="hifistate-menu"></div>
  </div>
  <button type="button" class="button" id="btn-vol-upc">
      <span class="button__text"><i class="fad fa-volume-up"></i> +5%</span>
  </button>
  <p class="grid-sep"></p>
  <button type="button" class="button" id="btn-vol-display">
      <span class="button__text"><i class="fad fa-volume-off fa-lg"></i> Volume</span>
  </button>
  <div class="dropdown">
    <button type="button" class="button btm-padding" id="btn-show-hifi-amp">
      <span class="button__text" id="hfinmenu-text"><i class="fad fa-sliders-v"></i> Inputs</span>
      <span class="button__text" id="hfinmenu-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="hfinmenu"></div>
  </div>
  <div class="dropdown">
    <button type="button" class="button btm-padding" id="btn-show-hifi-more">
      <span class="button__text" id="hfmoremenu-text"><i class="fad fa-list-music"></i> More...</span>
      <span class="button__text" id="hfmoremenu-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="hfmoremenu"></div>
  </div>
</div>`;
}
registerListeners({
  'btn-power-wkststate':   () => showPowerMenu('lrxmit', 'wkststate'),
  'btn-power-wl1_dual':    () => showPowerMenu('lrxmit', 'wl1_dual'),
  'btn-power-clr1':        () => showPowerMenu('lrxmit', 'clr1'),
  'lr-voldwnbtn':          () => sendVol(VOLUME_ACTIONS.voldwnbtn),
  'lr-volmutebtn':         () => sendVol(VOLUME_ACTIONS.volmutebtn),
  'lr-volupbtn':           () => sendVol(VOLUME_ACTIONS.volupbtn),
  'btn-vol-dwnc':          () => sendVol('dwnc'),
  'btn-power-hifistate':   () => showPowerMenu('lrxmit', 'hifistate'),
  'btn-vol-upc':           () => sendVol('upc'),
  'btn-vol-display':       () => sendVol('display'),
  'btn-show-hifi-amp':     () => showHiFiAmpInput(),
  'btn-show-hifi-more':    () => showHiFiMoreMenu()
});

function bedroomGridTemplate() {
  return `
<div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-menu-br-bed">
      <span class="button__text" id="bed_leds-text"><i class="fad fa-bed-alt"></i> Bed</span>
      <span class="button__text" id="bed_leds-spinner"></span>
    </button>
    <div class="dd-content dd-button" id="bed_leds-menu">
      <a id="btn-bed-adjprompt"> Adjust<i class="fad fa-sort-circle-up"></i></a>
      <a id="bed_leds-colorprompt"> Picker<i class="fad fa-palette"></i></a>
      <a id="bed_leds-colorcycle"> Cycle</a>
      <a id="bed_leds-sweep"> Sweep</a>
      <a id="bed_leds-rainbow"> Rainbow</a>
      <a id="bed_leds-stop"> Stop<i class="fad fa-stop"></i></a>
    </div>
  </div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-dyn-relaxmenu">
      <span class="button__text"><i class="fad fa-snooze"></i> Relax</span>
    </button>
    <div class="dd-content dd-button" id="relaxmenu"></div>
  </div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-power-brlamp1">
      <span class="button__text" id="brlamp1-text"><i class="fad fa-lamp-desk"></i> Dresser</span>
      <span class="button__text" id="brlamp1-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="brlamp1-menu"></div>
  </div>
  <p class="grid-sep"></p>
  <button type="button" class="button" id="br-voldwnbtn">
    <span class="button__text"><i class="fad fa-volume-down fa-lg"></i></span>
  </button>
  <button type="button" class="button" id="br-volmutebtn">
      <span class="button__text"><i class="fad fa-volume-mute fa-lg"></i></span>
  </button>
  <button type="button" class="button" id="br-volupbtn">
      <span class="button__text"><i class="fad fa-volume-up fa-lg"></i></span>
  </button>
  <p class="grid-sep"></p>
  <div class="dropdown">
    <button type="button" class="button" id="btn-power-brtv">
      <span class="button__text" id="brtv-text"><i class="fad fa-tv-retro"></i> TV</span>
      <span class="button__text" id="brtv-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="brtv-menu"></div>
  </div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-power-ampstate">
      <span class="button__text" id="ampstate-text"><i class="fad fa-waveform"></i> Audio</span>
      <span class="button__text" id="ampstate-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="ampstate-menu"></div>
  </div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-power-brpc">
      <span class="button__text" id="brpc-text"><i class="fad fa-mouse"></i> PC</span>
      <span class="button__text" id="brpc-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="brpc-menu"></div>
  </div>
  <p class="grid-sep"></p>
  <div class="dropdown">
    <button type="button" class="button" id="btn-power-brmacs">
      <span class="button__text" id="brmacs-text"><i class="fad fa-computer-classic"></i> Macs</span>
      <span class="button__text" id="brmacs-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="brmacs-menu"></div>
  </div>
  <div class="dropdown">
    <button type="button" class="button btm-padding" id="btn-show-br-amp">
      <span class="button__text" id="ampinp-text"><i class="fad fa-sliders-v"></i> Inputs</span>
      <span class="button__text" id="ampinp-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="brinpmenu"></div>
  </div>
  <div class="dropdown">
    <button type="button" class="button" id="btn-power-retropi">
      <span class="button__text" id="retropi-text"><i class="fad fa-gamepad"></i> RetroPi</span>
      <span class="button__text" id="retropi-spinner"></span>
    </button>
    <div class="dd-content ind-button" id="retropi-menu"></div>
  </div>
</div>`;
}
registerListeners({
  'btn-menu-br-bed':      () => showPowerMenu('brxmit', 'bed_leds'),
  'btn-bed-adjprompt':    () => bedPrompt(),
  'bed_leds-colorprompt': () => openColorPrompt('brxmit', 'bed_leds_hex_set'),
  'bed_leds-rainbow':     () => sendCmd('main', 'brxmit', 'bed_leds_rb'),
  'bed_leds-colorcycle':  () => sendCmd('main', 'brxmit', 'bed_leds_cy'),
  'bed_leds-sweep':       () => sendCmd('main', 'brxmit', 'bed_leds_sw'),
  'bed_leds-stop':        () => sendCmd('main', 'brxmit', 'bed_leds_off'),
  'btn-dyn-relaxmenu':    () => showDynAPIMenu('relaxmenu'),
  'btn-power-brlamp1':    () => showPowerMenu('brxmit', 'brlamp1'),
  'br-voldwnbtn':         () => sendVol(VOLUME_ACTIONS.voldwnbtn),
  'br-volmutebtn':        () => sendVol(VOLUME_ACTIONS.volmutebtn),
  'br-volupbtn':          () => sendVol(VOLUME_ACTIONS.volupbtn),
  'btn-power-brtv':       () => showPowerMenu('brxmit', 'brtv'),
  'btn-power-ampstate':   () => showPowerMenu('brpi', 'ampstate'),
  'btn-power-brpc':       () => showPowerMenu('brxmit', 'brpc'),
  'btn-power-brmacs':     () => showPowerMenu('brxmit', 'brmacs'),
  'btn-show-br-amp':      () => showBedroomAmpInput(),
  'btn-power-retropi':    () => showPowerMenu('brxmit', 'retropi')
});

function serverOptionsTemplate(f) {
  return `
<div class="svropt__prompt prompt_win" data-app-injected>
  ${f.isServer ? `
  <div id="svropt-panel-container"></div>` : ''}
  ${f.isPiLike ? piOptionsTemplate() : ''}
</div>`;
}

function svrOptMainTemplate() {
  return `
<div class="svropt__main">
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-atalk">
      <span class="button__text"><i class="fab fa-apple"></i> AFP</span>
    </button>
    <div class="dd-content dd-button" id="atalk-menu">
      <a id="btn-svr-startatalk"><i class="fad fa-play"></i> Start</a>
      <a id="btn-svr-stopatalk"><i class="fad fa-stop"></i> Stop</a>
    </div>
  </div>
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-system">
      <span class="button__text"><i class="fad fa-microchip"></i> System</span>
    </button>
    <div class="dd-content dd-button" id="system-menu">
      <a id="btn-ext-system"><i class="fad fa-link"></i> Open</a>
      <a id="btn-open-regions"><i class="fad fa-th"></i> Regions</a>
      <a id="btn-open-backup"><i class="fad fa-sync"></i> Backup</a>
      <a id="btn-svr-syslog"><i class="fad fa-tachometer"></i> Status</a>
    </div>
  </div>
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-legacy">
      <span class="button__text"><i class="fad fa-save"></i> CIFS</span>
    </button>
    <div class="dd-content dd-button" id="legacy-menu">
      <a id="btn-svr-startlegacy"><i class="fad fa-play"></i> Start</a>
      <a id="btn-svr-stoplegacy"><i class="fad fa-stop"></i> Stop</a>
    </div>
  </div>
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-router">
      <span class="button__text"><i class="fad fa-ethernet"></i> Router</span>
    </button>
    <div class="dd-content dd-button" id="router-menu">
      <a id="btn-ext-router"><i class="fad fa-link"></i> Open</a>
      <a id="btn-svr-rebootrouter"><i class="fad fa-redo"></i> Reboot</a>
    </div>
  </div>
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-netwrk">
      <span class="button__text"><i class="fad fa-network-wired"></i> Network</span>
    </button>
    <div class="dd-content dd-button" id="netwrk-menu">
      <a id="btn-wifi-prompt"><i class="fad fa-wifi"></i> Access</a>
      <a id="btn-ext-edgeswitch"><i class="fad fa-sitemap"></i> Switch</a>
      <a id="btn-svr-netscan"><i class="fad fa-chart-network"></i> LAN Scanner</a>
    </div>
  </div>
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-unifi">
      <span class="button__text"><i class="fad fa-router"></i> UniFi</span>
    </button>
    <div class="dd-content dd-button" id="unifi-menu">
      <a id="btn-ext-unifi"><i class="fad fa-link"></i> Open</a>
      <a id="btn-svr-startunifi"><i class="fad fa-play"></i> Start</a>
      <a id="btn-svr-stopunifi"><i class="fad fa-stop"></i> Stop</a>
    </div>
  </div>
  <button type="button" class="button svropt_button" id="btn-close-svr-opt1">
    <span class="button__text"><i class="fad fa-times-octagon"></i> Close</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-svr-clearlog">
    <span class="button__text"><i class="fad fa-trash-alt"></i> Clear</span>
  </button>
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-xana">
      <span class="button__text"><i class="fab fa-linux"></i> Xana</span>
    </button>
    <div class="dd-content dd-button" id="xana-menu">
      <a id="btn-svr-startxana"><i class="fad fa-play"></i> Start</a>
      <a id="btn-svr-stopxana"><i class="fad fa-stop"></i> Stop</a>
    </div>
  </div>
</div>`;
}
registerListeners({
  'btn-menu-atalk':       () => showMenu('atalk-menu'),
  'btn-svr-startatalk':   () => serverAction('startatalk'),
  'btn-svr-stopatalk':    () => serverAction('stopatalk'),
  'btn-menu-legacy':      () => showMenu('legacy-menu'),
  'btn-svr-startlegacy':  () => serverAction('startlegacy'),
  'btn-svr-stoplegacy':   () => serverAction('stoplegacy'),
  'btn-menu-unifi':       () => showMenu('unifi-menu'),
  'btn-ext-unifi':        () => GoToExtPage('unifi:11443/network/ff7gqlig/dashboard'),
  'btn-svr-startunifi':   () => serverAction('startunifi'),
  'btn-svr-stopunifi':    () => serverAction('stopunifi'),
  'btn-menu-xana':        () => showMenu('xana-menu'),
  'btn-svr-startxana':    () => serverAction('startxana'),
  'btn-svr-stopxana':     () => serverAction('stopxana'),
  'btn-menu-system':      () => showMenu('system-menu'),
  'btn-ext-system':       () => GoToExtPage('pve:8006'),
  'btn-open-backup':      () => openServerPanel(svrOptBackupTemplate),
  'btn-open-regions':     () => openServerPanel(svrOptRegionsTemplate),
  'btn-svr-syslog':       () => serverAction('syslog'),
  'btn-menu-netwrk':      () => showMenu('netwrk-menu'),
  'btn-wifi-prompt':      () => wifiPrompt(),
  'btn-ext-edgeswitch':   () => GoToExtPage('edgeswitch.home'),
  'btn-svr-netscan':      () => serverAction('netscan'),
  'btn-close-svr-opt1':   () => closeServerOptions(),
  'btn-svr-clearlog':     () => serverAction('clearlog'),
  'btn-menu-router':      () => showMenu('router-menu'),
  'btn-ext-router':       () => GoToExtPage('router.home'),
  'btn-svr-rebootrouter': () => serverAction('rebootrouter')
});

function svrOptBackupTemplate() {
  return `
<div class="svropt__backup">
  <button type="button" class="button svropt_button" id="btn-svr-last_backup_dates">
    <span class="button__text"><i class="fad fa-history"></i> History</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-svr-backupchk">
    <span class="button__text"> Checksum</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-svr-pve_vmsbkp">
    <span class="button__text"> VMs/LXCs</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-pwd-unlock">
    <span class="button__text"><i class="fad fa-lock-open"></i> Unlock</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-svr-backupstd">
    <span class="button__text"> Standard</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-svr-detach_bkps">
    <span class="button__text"><i class="fad fa-eject"></i> Eject</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-close-svr-opt2">
    <span class="button__text"><i class="fad fa-times-octagon"></i> Close</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-svr-pve_listsnaps">
    <span class="button__text"> Snapshots</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-open-svr-opt1">
    <span class="button__text"><i class="fad fa-arrow-left"></i> Back</span>
  </button>
</div>`;
}
registerListeners({
  'btn-svr-last_backup_dates': () => serverAction('last_backup_dates'),
  'btn-svr-backupchk':         () => serverAction('backupchk'),
  'btn-svr-pve_vmsbkp':        () => serverAction('pve_vmsbkp'),
  'btn-pwd-unlock':            () => getPassword('pwd'),
  'btn-svr-backupstd':         () => serverAction('backupstd'),
  'btn-svr-detach_bkps':       () => serverAction('detach_bkps'),
  'btn-close-svr-opt2':        () => closeServerOptions(),
  'btn-svr-pve_listsnaps':     () => serverAction('pve_listsnaps'),
  'btn-open-svr-opt1':         () => openServerPanel(svrOptMainTemplate)
});

function svrOptRegionsTemplate() {
  return `
<div class="svropt__regions">
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-archive">
      <span class="button__text"> Archive</span>
    </button>
    <div class="dd-content dd-button" id="archive-menu">
      <a id="btn-pwd-fusearch"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-pwd-fusearch_unmnt"><i class="fad fa-eject"></i> Detach</a>
    </div>
  </div>
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-snapshots">
      <span class="button__text"> Snapshots</span>
    </button>
    <div class="dd-content dd-button" id="snapshots-menu">
      <a id="btn-svr-mnt_snap_region"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-svr-unmnt_snap_region"><i class="fad fa-eject"></i> Detach</a>
    </div>
  </div>
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-ramdisk">
      <span class="button__text"> RAM Disk</span>
    </button>
    <div class="dd-content dd-button" id="ramdisk-menu">
      <a id="btn-svr-mnt_ram_region"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-svr-unmnt_ram_region"><i class="fad fa-eject"></i> Detach</a>
    </div>
  </div>
  <button type="button" class="button svropt_button" id="btn-close-svr-opt3">
    <span class="button__text"><i class="fad fa-times-octagon"></i> Close</span>
  </button>
  <button type="button" class="button svropt_button dark_btn" id="btn-svr-unmnt_all">
    <span class="button__text"> Detach All</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-open-svr-opt2">
    <span class="button__text"><i class="fad fa-arrow-left"></i> Back</span>
  </button>
</div>`;
}
registerListeners({
  'btn-menu-archive':          () => showMenu('archive-menu'),
  'btn-pwd-fusearch':          () => getPassword('fusearch'),
  'btn-pwd-fusearch_unmnt':    () => serverAction('unmnt_arch_region'),
  'btn-menu-snapshots':        () => showMenu('snapshots-menu'),
  'btn-svr-mnt_snap_region':   () => serverAction('mnt_snap_region'),
  'btn-svr-unmnt_snap_region': () => serverAction('unmnt_snap_region'),
  'btn-menu-ramdisk':          () => showMenu('ramdisk-menu'),
  'btn-svr-mnt_ram_region':    () => serverAction('mnt_ram_region'),
  'btn-svr-unmnt_ram_region':  () => serverAction('unmnt_ram_region'),
  'btn-close-svr-opt3':        () => closeServerOptions(),
  'btn-svr-unmnt_all':         () => serverAction('unmnt_all'),
  'btn-open-svr-opt2':         () => openServerPanel(svrOptMainTemplate),
  'btn-svr-mnt_ext_region':    () => serverAction('mnt_ext_region'),
  'btn-svr-unmnt_ext_region':  () => serverAction('unmnt_ext_region')
});

function piOptionsTemplate() {
  return `
<div>
  <div class="dropdown">
    <button type="button" class="button svropt_button" id="btn-menu-piwifi">
      <span class="button__text"><i class="fad fa-wifi"></i> WiFi</span>
    </button>
    <div class="dd-content dd-button" id="piwifi-menu">
      <a id="btn-show-piwifiprompt"> Setup WiFi</a>
      <a id="btn-svr-client"> Client Mode</a>
      <a id="btn-svr-apd"> Hotspot Mode</a>
      <a id="btn-svr-wifiscan"> WiFi Scanner</a>
    </div>
  </div>
  <button type="button" class="button svropt_button" id="btn-svr-showwpa">
    <span class="button__text"> Network</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-svr-temp">
    <span class="button__text"> Temperature</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-close-svr-opt4">
    <span class="button__text"><i class="fad fa-times-octagon"></i> Close</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-svr-pi-clearlog">
    <span class="button__text"> Clear Log</span>
  </button>
  <button type="button" class="button svropt_button" id="btn-svr-resetbt">
    <span class="button__text"><i class="fab fa-bluetooth"></i> Reset</span>
  </button>
</div>`;
}
registerListeners({
  'btn-menu-piwifi':       () => showMenu('piwifi-menu'),
  'btn-show-piwifiprompt': () => showPiWiFiPrompt(),
  'btn-svr-client':        () => serverAction('client'),
  'btn-svr-apd':           () => serverAction('apd'),
  'btn-svr-wifiscan':      () => serverAction('wifiscan'),
  'btn-svr-showwpa':       () => serverAction('showwpa'),
  'btn-svr-temp':          () => serverAction('temp'),
  'btn-close-svr-opt4':    () => closeServerOptions(),
  'btn-svr-pi-clearlog':   () => serverAction('clearlog'),
  'btn-svr-resetbt':       () => serverAction('resetbt')
});

// LED text scroller prompt (server + LEDpi)
function textScrollTemplate(f) {
  if (!f.isLedCapable) return '';
  return `
<div data-app-injected>
  <div class="textscroll__prompt prompt_win">
    <div id="textScrollTitle">TextScroll</div>
    <textarea class="scroll-bar-style" id="textScrollBox" spellcheck="false"></textarea>
    <div class="dropdown">
      <button type="button" class="button button__sml" id="btn-menu-textscrollcolor">
        <span class="button__text"> Color</span>
      </button>
      <div class="dd-content dd-button" id="textscrollcolor-menu">
        <a id="btn-tscolor-1"> Green</a>
        <a id="btn-tscolor-2"> Red</a>
        <a id="btn-tscolor-3"> Blue</a>
        <a id="btn-tscolor-4"> Orange</a>
        <a id="btn-tscolor-5"> Purple</a>
        <a id="btn-tscolor-6"> Yellow</a>
        <a id="btn-tscolor-7"> White</a>
      </div>
    </div>
    <div class="dropdown">
      <button type="button" class="button button__sml" id="btn-menu-textscrollfont">
        <span class="button__text"> Font</span>
      </button>
      <div class="dd-content dd-button" id="textscrollfont-menu">
        <a id="btn-tsfont-8x8"> Large</a>
        <a id="btn-tsfont-5x7"> Small</a>
        <a id="btn-tsfont-3x5"> Narrow</a>
      </div>
    </div>
    <div class="dropdown">
      <button type="button" class="button button__sml" id="btn-menu-textscrollspeed">
        <span class="button__text"> Speed</span>
      </button>
      <div class="dd-content dd-button" id="textscrollspeed-menu">
        <a id="btn-tsspeed-slowest"> Slowest</a>
        <a id="btn-tsspeed-slow"> Slow</a>
        <a id="btn-tsspeed-normal"> Normal</a>
        <a id="btn-tsspeed-fast"> Fast</a>
        <a id="btn-tsspeed-fastest"> Fastest</a>
      </div>
    </div>
    <button type="button" class="button button__sml" id="btn-ts-close">
      <span class="button__text"><i class="fad fa-times-octagon"></i></span>
    </button>
    <button type="button" class="button button__sml" id="btn-ts-clear">
      <span class="button__text"><i class="fad fa-trash-alt"></i></span>
    </button>
    <button type="button" class="button button__sml" id="btn-ts-send">
      <span class="button__text"><i class="fad fa-upload"></i></span>
    </button>
  </div>
</div>`;
}
registerListeners({
  'btn-menu-textscrollcolor': () => showMenu('textscrollcolor-menu'),
  'btn-tscolor-1':            () => setTextScrollColor('00FF00'),
  'btn-tscolor-2':            () => setTextScrollColor('FF0000'),
  'btn-tscolor-3':            () => setTextScrollColor('0000FF'),
  'btn-tscolor-4':            () => setTextScrollColor('FFA500'),
  'btn-tscolor-5':            () => setTextScrollColor('A020F0'),
  'btn-tscolor-6':            () => setTextScrollColor('FFCC00'),
  'btn-tscolor-7':            () => setTextScrollColor('FFFFFF'),
  'btn-menu-textscrollfont':  () => showMenu('textscrollfont-menu'),
  'btn-tsfont-8x8':           () => setTextScrollFont('8x8'),
  'btn-tsfont-5x7':           () => setTextScrollFont('5x7'),
  'btn-tsfont-3x5':           () => setTextScrollFont('3x5'),
  'btn-menu-textscrollspeed': () => showMenu('textscrollspeed-menu'),
  'btn-tsspeed-slowest':      () => setTextScrollSpeed('3.0'),
  'btn-tsspeed-slow':         () => setTextScrollSpeed('9.0'),
  'btn-tsspeed-normal':       () => setTextScrollSpeed('12.0'),
  'btn-tsspeed-fast':         () => setTextScrollSpeed('15.0'),
  'btn-tsspeed-fastest':      () => setTextScrollSpeed('20.0'),
  'btn-ts-close':             () => closeTextScroll(),
  'btn-ts-clear':             () => clearLEDsMessage(),
  'btn-ts-send':              () => sendTextScroll()
});

// server log popup window (all models -- Pi opens it via the System menu)
function logFormTemplate() {
  return `
<div class="log-popup popup-win" id="logForm" data-app-injected>
  <div class="popup-ctn">
    <textarea readonly class="scroll-bar-style" id="logTextBox" spellcheck="false"></textarea>
    <div class="popup-actions">
      <button type="button" class="button button_left_txtwin" id="btn-log-close">
        <i class="fad fa-times-octagon"></i>
      </button>
      <button type="button" class="button button_left_txtwin" id="btn-log-open-svr">
        <i class="fad fa-sliders-h"></i>
      </button>
      <button type="button" class="button button_left_txtwin" id="btn-log-load">
        <i class="fad fa-sync-alt"></i>
      </button>
      <button type="button" class="button button_right_txtwin" id="sendButton">
        <i class="fad fa-upload"></i>
      </button>
    </div>
  </div>
</div>`;
}
registerListeners({
  'btn-log-close':    () => closePopup(),
  'btn-log-open-svr': () => openServerPanel(svrOptMainTemplate),
  'btn-log-load':     () => loadLog('sysout'),
  'sendButton':       () => serverSend()
});

function camPopupTemplate() {
  return `
<div class="popup-win" id="cam-popup" data-app-injected>
  <div class="popup-ctn">
    <img id="cam-display">
    <div class="popup-actions">
      <button type="button" class="button button_left_txtwin" id="btn-cam-close">
        <i class="fad fa-times-octagon"></i>
      </button>
      <button type="button" class="button button_right_txtwin" id="btn-cam-fullscreen">
        <span> Fullscreen</span>
      </button>
    </div>
  </div>
</div>`;
}
registerListeners({
  'btn-cam-close':      () => closePopup(),
  'btn-cam-fullscreen': () => goToContextRoot('cam1')
});

/////// Bed Adjustment Window ///////

// separate throttles so up/down each get their own 250ms window
const throttledBedUp = requestThrottle(() => {
  sendCmd('main', 'brxmit', 'bed_adj_up');
}, 250);

const throttledBedDown = requestThrottle(() => {
  sendCmd('main', 'brxmit', 'bed_adj_dwn');
}, 250);

function bedPrompt() {
  const _winid = 'bed__prompt';
  if (document.getElementById(_winid)) return;
  const bedprompt = promptWindow(_winid, `
    <div class="bed__title">Bed Adjustment</div>
    <button type="button" class="button bed__btn" id="bed__btn-up">
      <i class="fad fa-arrow-up bed__icon"></i>
    </button>
    <button type="button" class="button bed__btn" id="bed__btn-down">
      <i class="fad fa-arrow-down bed__icon"></i>
    </button>
    <button type="button" class="button" id="bed__btn-close">
      <i class="fad fa-times-octagon right-padding"></i>
      <span class="button__text">Close</span>
    </button>
  `);
  onButtons(bedprompt, (button) => {
    switch (button.id) {
      case 'bed__btn-up':
        throttledBedUp();
        break;
      case 'bed__btn-down':
        throttledBedDown();
        break;
      case 'bed__btn-close':
        // a trailing press still inside its window would otherwise go out
        // after the window it came from had gone
        throttledBedUp.cancel();
        throttledBedDown.cancel();
        bedprompt.remove();
        break;
    }
  });
}

/// server popup window ///

function serverSend() {
  const txtArea = document.getElementById("logTextBox");
  if (App.serverCmdData === null) {
    txtArea.value = "select an option.";
  } else {
    sendCmd('main', 'server', App.serverCmdData);
    txtArea.value += "\n" + App.serverCmdData + " command sent.";
    txtArea.scrollTop = txtArea.scrollHeight;
  }
  sendBtnAlert("off");
  App.serverCmdData = null;
}

function sendBtnAlert(state) {
  const _class = "selected_btn";
  const _elem = "sendButton";
  if (state === 'on') {
    addClassToElem('add', _elem, _class);
  }
  if (state === 'off') {
    addClassToElem('remove', _elem, _class);
  }
}

function clearPendingCmd() {
  sendBtnAlert("off");
  App.serverCmdData = null;
}

function openServerPanel(panelTemplate) {
  closeServerOptions();
  const container = document.getElementById("svropt-panel-container");
  if (container) {
    container.innerHTML = panelTemplate();
  }
  classDisplay("svropt__prompt", "block");
}

function closeServerOptions() {
  removeDropdowns();
  clearPendingCmd();
  classDisplay("svropt__prompt", "none");
  // remove the rendered panel markup entirely
  const container = document.getElementById("svropt-panel-container");
  if (container) {
    container.innerHTML = "";
  }
}

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
  sendCmd('leds', 'stop');
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
  sendCmd('leds', 'textscroll', base64URLSafeEncode(commands)).then((data) => {
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

/////// Volume Controls ///////

function sendVol(_cmd) {
  const config = ROOM_CONFIGS[App.currentRoom];
  if (subwooferVolumeEnabled() === true) {
    sendCmd('main', config.voltarget, 'sub' + _cmd); // subwoofers
  } else {
    setAmpVolume(_cmd, config.voltarget, config.volmax);
  }
}

// the volume buttons of the room currently on screen
function volumeButtonIds() {
  return Object.keys(VOLUME_ACTIONS).map(key => `${App.currentRoom}-${key}`);
}

function subwooferVolumeEnabled() {
  const ids = volumeButtonIds();
  if (ids.length === 0) return false;
  const element = document.getElementById(ids[0]);
  if (element) {
    return element.classList.contains(SUBWOOFER_CLASS);
  }
  return false;
}

function subwooferVolumeMode(enable) {
  volumeButtonIds().forEach(id => {
    const element = document.getElementById(id);
    if (element) {
      element.classList.toggle(SUBWOOFER_CLASS, enable === true);
    }
  });
}

// Reads the amp back after the command and shows the level as a percentage.
// An empty answer means the amp is off, which the popup shows as "---".
function setAmpVolume(_cmd, _device, _maxvol) {
  sendCmd('main', _device, 'vol' + _cmd).then((data) => {
    const amp_vol = cleanString(data);
    if (isNumeric(amp_vol) === true) {
      let display_value = Number(amp_vol);
      if (_maxvol != 100) {
        const scaled = mapNumber(amp_vol, 0, _maxvol, 0, 100);
        display_value = Number(Math.round(scaled));
      }
      showVolumePopup(display_value);
    } else if (amp_vol == "") {
      showVolumePopup(-1); // empty response = power off
    }
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
      <button class="button temp__btn fad fa-times-octagon" type="button"></button>
      <button class="button temp__btn fad fa-sync" type="button"></button>
    </div>
  `);
  // cancel first, refresh second
  const [tempcancelb, temprefreshb] = tempprompt.querySelectorAll("button");
  getTemperatureData();
  onButtons(tempprompt, (button) => {
    if (button === tempcancelb) {
      tempprompt.remove();
    }
    if (button === temprefreshb) {
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

/////// Pi Wi-Fi Window ///////

async function showPiWiFiPrompt() {
  const winid = "pinet__prompt";
  if (document.getElementById(winid)) return;
  removeDropdowns();
  const result = await piWiFiPrompt(winid);
  if (result !== null && result !== "") {
    sendCmd("main", "confwpa", result);
    document.getElementById("logTextBox").value =
      "Wi-Fi configuration updated, select client mode to apply changes.";
  }
}

// Asks for an SSID and password and resolves with them base64 encoded as
// "ssid|$|password", or null when it was cancelled.
function piWiFiPrompt(_winid) {
  return new Promise((resolve) => {
    const pinetprompt = promptWindow(_winid, html`
      <div>Network (SSID):</div>
      <input id="pinet__ssidbox">
      <div>Password:</div>
      <input id="pinet__passbox" type="password">
      <button class="button pinet__btn" type="button">Cancel</button>
      <button class="button pinet__btn" type="button">Save</button>
    `);
    const pinettext1 = pinetprompt.querySelector("div"); // SSID label
    const pinetssidbox = document.getElementById("pinet__ssidbox");
    const pinetpassbox = document.getElementById("pinet__passbox");
    const [pinetcancelb, pinetokbutton] = pinetprompt.querySelectorAll("button");
    pinetssidbox.focus();
    function cancelWiFi() {
      pinetssidbox.value = "";
      pinetpassbox.value = "";
      pinetprompt.remove();
      resolve(null); // settle the promise on cancel
    }
    function sendWiFiData() {
      if (pinetssidbox.value === "") {
        pinettext1.innerHTML = "enter SSID!";
        return;
      }
      if (pinetpassbox.value === "") {
        pinettext1.innerHTML = "enter password!";
        return;
      }
      resolve(base64URLSafeEncode(`${pinetssidbox.value}|$|${pinetpassbox.value}`));
      cancelWiFi();
    }
    onButtons(pinetprompt, (button) => {
      if (button === pinetokbutton) {
        sendWiFiData();
      }
      if (button === pinetcancelb) {
        cancelWiFi();
      }
    });
    pinetssidbox.addEventListener("keyup", (ssid_evnt) => {
      if (ssid_evnt.key === "Enter") {
        pinetpassbox.focus();
      } else if (ssid_evnt.key === "Escape") {
        cancelWiFi();
      }
    });
    pinetpassbox.addEventListener("keyup", (pass_evnt) => {
      if (pass_evnt.key === "Enter") {
        sendWiFiData();
      } else if (pass_evnt.key === "Escape") {
        cancelWiFi();
      }
    });
  });
}

// the QR code and password for guest WiFi access
function wifiPrompt() {
  const _winid = "wifi__prompt";
  if (document.getElementById(_winid)) return;
  const wifiprompt = promptWindow(_winid, html`
    <div>Scan for WiFi Access</div>
    <img src="img/wifi.png" id="wifi__img">
    <div id="wifi__pwdtxt"></div>
    <button class="button" type="button" id="wifipmt__btn">Close</button>
  `);
  // password text set separately (value is data, not markup)
  document.getElementById("wifi__pwdtxt").innerHTML = atob(SSIDpwd());
  onButtons(wifiprompt, () => wifiprompt.remove());
}

/// password prompt ///

async function getPassword(_type) {
  try {
    removeDropdowns();
    const result = await passwordPrompt();
    if (result !== null && result !== "" && result !== undefined) {
      savePOST(_type, [result], afterPOSTActions);
    }
  } catch (e) {
    // user cancelled -- nothing to do
  }
}

// resolves with the typed password, rejects when it was cancelled
function passwordPrompt() {
  const _winid = "pass__prompt_win";
  if (document.getElementById(_winid)) return null;
  const pwprompt = promptWindow(_winid, html`
    <div>Enter password:</div>
    <input id="pass__textbox" type="password">
    <div id="pass__prmbtnctr">
      <button class="button pass__prompt_btn" type="button">Cancel</button>
      <button class="button pass__prompt_btn" type="button">Send</button>
    </div>
  `);
  const pwinput = document.getElementById("pass__textbox");
  const [pwcancelb, pwokbutton] = pwprompt.querySelectorAll("button");
  pwinput.focus();
  return new Promise(function (resolve, reject) {
    onButtons(pwprompt, (button) => {
      if (button === pwokbutton) {
        resolve(pwinput.value);
      } else {
        reject(new Error("User cancelled"));
      }
      pwprompt.remove();
    });
    pwinput.addEventListener("keyup", function handleEnter(up_events) {
      if (up_events.key === "Enter") {
        resolve(pwinput.value);
        pwprompt.remove();
      } else if (up_events.key === "Escape") {
        pwprompt.remove();
        reject(new Error("User cancelled"));
      }
    });
  });
}

/// volume popup ///

// vol is 0-100, 0 shows "Mute" and -1 shows "---" (the amp is off)
function showVolumePopup(vol) {
  const win_name = "vol-popup";
  const text_name = "vol-text";
  const grid_name = "vol-grid";
  const bar_name = "vol-bar";
  let volpopwin = document.getElementById(win_name);
  if (!volpopwin) {
    volpopwin = document.createElement("div");
    volpopwin.classList.add("popup-alert");
    volpopwin.id = win_name;
    volpopwin.innerHTML = html`
      <div id="${text_name}"></div>
      <div class="progress_grid" id="${grid_name}">
        <div class="bar_progress" id="${bar_name}"></div>
      </div>
    `;
    // Attached once, like the message popup: a volume press arriving mid-fade
    // cancels the animation, so a per-fade {once:true} listener would never
    // fire and never unregister.
    volpopwin.addEventListener("animationend", () => {
      if (volpopwin.classList.contains("fade-out") && volpopwin.isConnected) {
        volpopwin.remove();
      }
    });
    document.body.appendChild(volpopwin);
  }
  volpopwin.classList.remove("fade-out");
  const voltext = document.getElementById(text_name);
  const volgrid = document.getElementById(grid_name);
  const volbar = document.getElementById(bar_name);
  switch (vol) {
    case -1:
      volgrid.style.display = "none";
      voltext.innerHTML = "---";
      break;
    case 0:
      volgrid.style.display = "none";
      voltext.innerHTML = "Mute";
      break;
    default: {
      volgrid.style.display = "block";
      const volstr = `${vol}%`;
      voltext.innerHTML = volstr;
      volbar.style.width = volstr;
    }
  }
  clearTimeout(App.volPopupTimer);
  App.volPopupTimer = setTimeout(() => {
    App.volPopupTimer = null;
    // the listener above removes the window once the fade ends
    volpopwin.classList.add("fade-out");
  }, 3000);
}

//// device menus ////

// Amp input menus: one row per input, green on the one that is selected.
// code is what the device answers with when that input is live.
const HIFI_INPUTS = [
  { code: '6', cmd: 'autodac', label: 'Auto' },
  { code: '1', cmd: 'usb', label: 'USB' },
  { code: '3', cmd: 'phono', label: 'Phono' },
  { code: '5', cmd: 'opt', label: 'Optical' },
  { code: '4', cmd: 'coax', label: 'Coaxial' },
  { code: '2', cmd: 'aux', label: 'Aux' }
];

const BEDROOM_INPUTS = [
  { code: '2', cmd: 'opt-a', label: 'USB' },
  { code: '3', cmd: 'coaxial', label: 'Coaxial' },
  { code: '1', cmd: 'opt-b', label: 'Optical' },
  { code: '4', cmd: 'aux', label: 'Aux' }
];

function inputMenuRows(target, resp, inputs) {
  let rows = '';
  inputs.forEach(input => {
    rows += menuRow(`cmd~${target}~${input.cmd}`,
      indicator(resp == input.code), input.label);
  });
  return rows;
}

// Opens a device menu, asks the device for its state and draws the rows the
// answer builds. config: menu, target, cmd, textId, spinnerId, build(resp).
function openDeviceMenu(config) {
  if (!openDropdown(config.menu)) {
    return;
  }
  setSpinner(config.textId, config.spinnerId, true);
  const token = beginMenuRequest();
  sendCmd('main', config.target, config.cmd).then((data) => {
    const menudata = config.build(cleanString(data));
    // stop the spinner whether or not the rows are still wanted
    setSpinner(config.textId, config.spinnerId, false);
    // another menu was opened while this was in the air -- drawing now would
    // wipe the rows that menu just put on screen
    if (!menuRequestCurrent(token)) { return; }
    drawMenu(menudata, config.menu);
  });
}

function showHiFiMoreMenu() {
  const offcode = -1;
  const target = 'lrxmit';
  const menuname = 'hfmoremenu';
  openDeviceMenu({
    menu: menuname,
    target: target,
    cmd: 'curhpfvollim',
    textId: `${menuname}-text`,
    spinnerId: `${menuname}-spinner`,
    build: (resp) => {
      if (isNumeric(resp) !== true && !resp.includes('~')) {
        return menuRow(`${menuname}-error`, 'redind', 'Error');
      }
      if (resp == offcode) {
        return menuRow(`${menuname}-offline`, 'blkind', 'Offline');
      }
      if (resp == 0) {
        return menuRow(`${menuname}-muted`, 'noind', 'Muted');
      }
      // volLimitState ~ hpfState
      const [limit_state, hpf_state] = resp.split("~");
      let rows = menuRow('subs', 'subs', 'Subs Gain');
      rows += menuRow(`cmd~${target}~togglehpf`, indicator(hpf_state == '1'), 'Low Cut');
      rows += menuRow(`cmd~${target}~vlimit`, indicator(limit_state == '1'), 'vLimiter');
      return rows;
    }
  });
}

function showHiFiAmpInput() {
  const offcode = -1;
  const target = 'lrxmit';
  const menuname = 'hfinmenu';
  openDeviceMenu({
    menu: menuname,
    target: target,
    cmd: 'curinput',
    textId: `${menuname}-text`,
    spinnerId: `${menuname}-spinner`,
    build: (resp) => {
      if (isNumeric(resp) !== true) {
        return menuRow(`${menuname}-error`, 'redind', 'Error');
      }
      if (resp == offcode) {
        return menuRow(`${menuname}-offline`, 'blkind', 'Offline');
      }
      return inputMenuRows(target, resp, HIFI_INPUTS);
    }
  });
}

function showBedroomAmpInput() {
  const offcode = 0;
  const target = 'brpi';
  const menuname = 'brinpmenu';
  openDeviceMenu({
    menu: menuname,
    target: target,
    cmd: 'inputstate',
    textId: 'ampinp-text',
    spinnerId: 'ampinp-spinner',
    build: (resp) => {
      if (isNumeric(resp) !== true) {
        return menuRow(`${menuname}-error`, 'redind', 'Error');
      }
      if (resp == offcode) {
        return menuRow(`${menuname}-offline`, 'blkind', 'Offline');
      }
      return inputMenuRows(target, resp, BEDROOM_INPUTS);
    }
  });
}

// Menus where the slider is a level on something that is either running or
// not: zero is not a setting, it is off. A reading of 0 draws no slider and
// hides the Stop row (there is nothing to stop), and the slider that is drawn
// starts at 1 so it can never be dragged back to off -- Stop does that.
const LEVEL_MENUS = {
  'bed_leds': { min: 1, stopRowID: 'bed_leds-stop' }
};

// Builds the toggle and / or slider rows for one power menu from the device's
// answer. Returns "" when there is nothing to draw.
function powerMenuRows(menu, target, cmd1, cmd2) {
  const isDualToggleSlider = menu.endsWith('_dual');
  const isSingleLedSlider = menu.endsWith('_leds');
  const isFadeCandyLeds = (menu === 'fc_leds');
  const isDefinedMenu = (isDualToggleSlider || isSingleLedSlider || isFadeCandyLeds);
  let rows = '';
  if (isDefinedMenu === false || isDualToggleSlider === true) {
    if (cmd1 == '1' || cmd1 == '0') {
      rows += menuRow(`${cmd1}~${target}~${menu}`, 'toggle', '');
    }
  }
  if (isDualToggleSlider) {
    rows += menuRow(`${cmd2}~${target}~${menu}`, 'slider', '');
    return rows;
  }
  if (isFadeCandyLeds) {
    rows += menuRow(`${cmd1}~leds~fc`, 'slider', '');
    return rows;
  }
  if (!isSingleLedSlider) {
    return rows;
  }
  const level = LEVEL_MENUS[menu];
  if (!level) {
    rows += menuRow(`${cmd1}~${target}~${menu}`, 'slider', ''); // standard 0-255
    return rows;
  }
  // custom single LED: off hides the Stop row and draws no slider
  const leds_off = Number(cmd1) <= 0;
  let stop_row = null;
  if (level.stopRowID) {
    stop_row = document.getElementById(level.stopRowID);
  }
  if (leds_off === true) {
    if (stop_row) stop_row.style.display = 'none';
    return rows;
  }
  if (stop_row) stop_row.style.display = '';
  rows += menuRow(`${cmd1}~${target}~${menu}`, 'slider', '');
  return rows;
}

function showPowerMenu(target, menu = null) {
  if (menu == '' || menu == null) menu = target;
  if (!openDropdown(menu + '-menu')) {
    return;
  }
  const textId = menu + '-text';
  const spinnerId = menu + '-spinner';
  setSpinner(textId, spinnerId, true);
  let action = 'main';
  if (menu == 'fc_leds') action = 'leds';
  const token = beginMenuRequest();
  sendCmd(action, target, menu).then((data) => {
    let _menudata = '';
    const [cmd1, cmd2] = splitByDelimiter(cleanString(data), '~');
    if (isNumeric(cmd1) === false || data == null) {
      _menudata += menuRow(`${menu}-error`, 'redind', 'Error');
    } else {
      _menudata += powerMenuRows(menu, target, cmd1, cmd2);
    }
    setSpinner(textId, spinnerId, false);
    if (!menuRequestCurrent(token)) { return; }
    if (_menudata === '') {
      // nothing to draw -- an off level leaves the menu with its static rows
      // only. Any rows from a previous open still have to go.
      removeDynMenus();
      return;
    }
    drawMenu(_menudata, menu + '-menu');
  });
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

function afterPOSTActions(action) {
  let _send = false;
  if (action === 'pwd') {
    serverAction('attach_bkps');
    _send = true;
  }
  if (action === 'fusearch') {
    serverAction('mnt_arch_region');
    _send = true;
  }
  if (_send === true) {
    serverSend();
  }
}

// arms a server command; the send button then transmits it
function serverAction(cmd) {
  App.serverCmdData = cmd;
  sendBtnAlert("on");
}

function openLogWindow() {
  closePopup();
  document.getElementById("logForm").style.display = "block";
  loadLog('sysout');
}

// loads an entire text file into the log window
function loadLog(file) {
  fetch(fileURL('read', file), { method: 'GET' })
    .then((response) => {
      if (!response.ok) {
        throw new Error(`status ${response.status}`);
      }
      return response.json();
    })
    .then(data => {
      const txtArea = document.getElementById("logTextBox");
      txtArea.value = data;
      txtArea.scrollTop = txtArea.scrollHeight;
    })
    .catch(error => {
      showPopup("[loadLog] error fetching data.");
    });
  App.serverCmdData = null;
}

//// Dynamic Menus ////

function beginMenuRequest() {
  menuDrawToken += 1;
  return menuDrawToken;
}

function menuRequestCurrent(token) {
  return token === menuDrawToken;
}

// a menu whose rows come out of a file on the device
function showDynFileMenu(menu) {
  if (!openDropdown(menu)) {
    return;
  }
  const token = beginMenuRequest();
  loadMenu(fileURL('read', menu)).then((data) => {
    if (!menuRequestCurrent(token)) return;
    drawMenu(data, menu, { TwoColumns: true, MenuScroll: true });
  });
}

// a menu whose rows come back from a device API call
function showDynAPIMenu(menu) {
  if (!openDropdown(menu)) {
    return;
  }
  const url = location.protocol + "//" + location.hostname +
    "/api?var=&arg=" + menu + "&action=main";
  const token = beginMenuRequest();
  loadMenu(url).then((data) => {
    if (!menuRequestCurrent(token)) { return; }
    drawMenu(data, menu);
  });
}

// Draws each menu item from pipe-delimited rows: HOST|TYPE|NAME.
// `scroll` is only ever passed false by a caller redrawing a menu that is
// already on screen (the room status refresh): the step at the bottom
// measures a fixed-position menu against the page offset, which is right the
// once on opening and walks the page down if it is run on every refresh.
window.drawMenu = function(data, menu, config = undefined) {
  let scroll = true;   // menu scrolls into view by default
  let twocols = false; // 3-column data is the default
  if (config !== undefined) {
    scroll = config.MenuScroll;
    twocols = config.TwoColumns;
  }
  removeDynMenus();
  if (!data || typeof data !== 'string') {
    showPopup("[drawMenu] no data to parse.");
    return;
  }
  const navElement = document.getElementById(menu);
  if (!navElement) return;
  const fragment = document.createDocumentFragment();
  while (App.fileData.length) App.fileData.pop();
  const splitdat = splitMenuData(data);
  if (splitdat == "" || splitdat == null) return;
  for (let row = 0; row < splitdat.length; row++) {
    const line = splitdat[row];
    if (line == "" || line == null) continue;
    const linearr = line.split("|");
    App.fileData.push(line);
    // 0=Host, 1=Type, 2=Name
    const col0 = linearr[0];
    let col1 = linearr[1];
    let col2 = '';
    if (twocols === true) { // 2-column data mode
      col1 = menu;
      col2 = linearr[1];
    } else {
      col2 = linearr[2].trim();
    }
    const item = createListItem(col0, col1, col2, row);
    App.dynMenuNodes.push(item);
    fragment.appendChild(item);
    App.dynMenuActive = 1;
  }
  App.fileData.push(menu); // menu name at the end of the array
  navElement.appendChild(fragment); // one append for the whole menu
  // a redraw of a menu already on screen stops here
  if (scroll === false) return;
  scrollMenuIntoView(navElement);
}

// scrolls down to the menu only when it runs past the bottom of the viewport
function scrollMenuIntoView(navElement) {
  const viewportHeight = window.innerHeight || document.documentElement.clientHeight;
  const rect = navElement.getBoundingClientRect();
  if (rect.bottom <= viewportHeight) {
    return;
  }
  const currentTop = window.pageYOffset || document.documentElement.scrollTop;
  const menuTop = rect.top + currentTop;
  // only move down, and never past the top of the menu
  if (menuTop > currentTop) {
    window.scrollTo({ top: menuTop, behavior: 'smooth' });
  }
}

// the indicator types, and the dot class each one draws
const INDICATOR_CLASSES = {
  'noind': '',
  'blkind': '',
  'grnind': 'ind_dot_green',
  'redind': 'ind_dot_red',
  'ylwind': 'ind_dot_yellow'
};

// one row: col0 is HOST~CMD data, col1 the row type, col2 the label
function createListItem(col0_in, col1, col2, menuid) {
  const _elm = document.createElement('a');
  const [col0, host, cmd] = col0_in.split("~");
  _elm.id = "menu-" + menuid;
  _elm.innerText = col2;
  switch (col1) {
    case 'ledcolormenu':
    case 'ledeffectmenu': {
      // LEDs menu
      _elm.addEventListener("click", function() {
        if (col0 == 'textscroll') {
          openTextScroll();
        } else {
          sendCmd('leds', col0);
        }
      });
      break;
    }
    case 'room': {
      // room selector menu
      _elm.addEventListener("click", function() {
        selectRoom(col0);
        refreshRoomState();
      });
      break;
    }
    case 'relaxmenu':
    case 'relaxactive': {
      _elm.addEventListener("click", function() {
        sendCmd('main', 'relax', col0);
      });
      if (col1 == 'relaxactive') {
        _elm.appendChild(makeElement('i', 'fad fa-play'));
      }
      break;
    }
    case 'relaxstop': {
      _elm.addEventListener("click", function() {
        sendCmd('main', 'relaxstop');
      });
      _elm.appendChild(makeElement('i', 'fad fa-stop'));
      break;
    }
    case 'link':
      // URL menu
      _elm.classList.add('norm-cursor');
      _elm.href = col0;
      break;
    case 'chk': {
      // checkbox row for a browser-side setting; col2 is the icon
      const chksettings = WWW_SETTINGS[col0];
      if (!chksettings) break;
      _elm.classList.add('menu-checkbox');
      _elm.classList.add('nohide');
      _elm.classList.add('no-hover');
      _elm.innerText = '';
      _elm.appendChild(makeElement('i', col2));
      const chk_cmd = `${col0}-`;
      const chk_obj = new ToggleSwitch(_elm, {
        id: `setting-${col0}`,
        checked: chksettings.get() === true,
        width: '54px',
        height: '26px',
        cmdHost: 'local',
        command: chk_cmd,
        onToggle: (payload) => chksettings.set(payload.cmd === `${chk_cmd}on`)
      });
      chk_obj.activate();
      // removeDynMenus() destroys these, same as any other menu switch
      App.activeSwitches.push(chk_obj);
      break;
    }
    case 'slider': {
      // horizontal slider row; col0 is the current level
      const customMin = LEVEL_MENUS[cmd];
      const slider = document.createElement('input');
      slider.classList.add("slider-wide");
      slider.type = 'range';
      slider.min = 0;
      if (customMin && customMin.min) {
        slider.min = customMin.min;
      }
      slider.max = 255;
      slider.value = col0;
      slider.step = 1;
      slider.addEventListener('input', (event) => {
        throttledSliderSend('main', host, `${cmd}_set_${event.target.value}`);
      });
      _elm.appendChild(slider);
      _elm.classList.add('slider-wide_container');
      _elm.classList.add('no-hover');
      break;
    }
    case 'subs': {
      // subwoofer volume toggle
      const submode = subwooferVolumeEnabled();
      if (submode === true) {
        _elm.classList.add(SUBWOOFER_CLASS);
      } else {
        _elm.classList.remove(SUBWOOFER_CLASS);
      }
      _elm.addEventListener("click", function() {
        subwooferVolumeMode(!submode);
      });
      break;
    }
    case 'toggle': {
      // switch-like toggle row; col0 is '1' when it is on
      _elm.classList.add('no-hover');
      const tgl_obj = new ToggleSwitch(_elm, {
        id: `${host}-${cmd}`,
        checked: col0 === '1',
        cmdHost: host,
        command: cmd,
        onToggle: toggleSwitchCallback
      });
      tgl_obj.activate();
      App.activeSwitches.push(tgl_obj);
      break;
    }
    default: {
      // status row, with an indicator dot in the type's own colour
      if (!(col1 in INDICATOR_CLASSES)) {
        break;
      }
      addStatusListener(_elm, col0, host, cmd);
      if (col1 === 'noind') {
        break;
      }
      const _dot = makeElement('span', 'ind_dot');
      const dotColor = INDICATOR_CLASSES[col1];
      if (dotColor !== '') {
        _dot.classList.add(dotColor);
      }
      _dot.id = "ind-" + col2;
      _elm.appendChild(_dot);
      break;
    }
  }
  return _elm;
}

// a row marked 'cmd' sends its command to the device when it is pressed
function addStatusListener(elm, col0, host, cmd) {
  if (col0 == 'cmd') {
    elm.addEventListener("click", function() {
      sendCmd('main', host, cmd);
    });
  }
}

const throttledSliderSend = requestThrottle((host, cmd, val) => {
  sendCmd(host, cmd, val);
}, 50);

function removeDynMenus() {
  if (App.dynMenuActive != 1) {
    return;
  }
  // the last value of a drag can still be sitting in the throttle; without
  // this it went out up to 50ms after the slider had left the page
  throttledSliderSend.cancel();
  // A toggle row carries a ToggleSwitch with pointer listeners on it, so the
  // switches go before their markup does rather than being left bound to
  // detached nodes until the next removeDropdowns().
  destroyActiveSwitches();
  // the rows drawMenu() created, removed directly instead of looking up one
  // id per line of the file
  App.dynMenuNodes.forEach(node => node.remove());
  App.dynMenuNodes.length = 0;
  App.dynMenuActive = 0;
}

// Both callers of this are LED colours, so the picker's heading says so.
const LED_PROMPT_NAME = 'LED';

function openColorPrompt(command, argument, three_args = false) {
  // the previous prompt is destroyed rather than dropped: show() wipes the
  // anchor's markup, and an instance left behind mid-drag still holds
  // pointermove / pointerup listeners on window
  if (App.colorPrompt) {
    App.colorPrompt.destroy();
    App.colorPrompt = null;
  }
  const colorPrompt = new ColorPrompt('color-popup', (strippedHex) => {
    let cmd = 'main';
    let arg1 = command;
    let arg2 = `${argument}_${strippedHex}`;
    if (three_args === true) {
      cmd = command;
      arg1 = argument;
      arg2 = strippedHex;
    }
    sendCmd(cmd, arg1, arg2)
      .catch(err => {
        showPopup("[ColorPrompt] failed to send color: " + err);
      });
  }, {
    name: LED_PROMPT_NAME
  });
  colorPrompt.show();
  colorPrompt.updateStateFromHex('#000000', true, false);
  App.colorPrompt = colorPrompt;
}

//// End Dynamic Menus ////

function openCamWindow() {
  closePopup();
  document.getElementById("cam-popup").style.display = "block";
  document.getElementById("cam-display").src = "/cam1";
}

function goToContextRoot(_path) {
  closePopup();
  window.location = location.protocol + "//" + location.hostname + "/" + _path;
}

// close all popup windows
function closePopup() {
  setDisplayById("logForm", "none");
  setDisplayById("cam-popup", "none");
  document.getElementById("cam-display").src = "";
  clearPendingCmd();
  closeServerOptions();
}

//// runs before exit ////

window.addEventListener('beforeunload', () => {
  App.destroyAll();
});

App.destroyAll = function() {
  console.log("Exiting...");
  if (App.activityTimeout) clearTimeout(App.activityTimeout);
  // the message popup's timer lives in utils.js, where it is set
  clearPopupTimer();
  if (App.volPopupTimer) clearTimeout(App.volPopupTimer);
  // Bookmarks, before anything is torn down: an open edit and the lock row go
  // out as beacons, which survive the teardown that cancels the ordinary
  // write destroyPageServices() would otherwise start and never finish. The
  // lists window keeps its own pagehide beacon, which fires on iOS where this
  // handler often does not.
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
  // Lists window. dispose() rather than destroy(): the page is letting the
  // instance go, so its document / window listeners have to come off with it,
  // or the old instance stays reachable from them and answers pagehide beside
  // whichever instance replaced it.
  if (App.lists) {
    App.lists.dispose();
    App.lists = null;
  }
  // colour picker -- holds window-level pointer listeners while dragging
  if (App.colorPrompt) {
    App.colorPrompt.destroy();
    App.colorPrompt = null;
  }
  // bookmarks menu -- the add/edit window is a direct child of <body>, so it
  // outlives removeInjectedPage(); a stale edit mode would also keep
  // handleClicks() swallowing every click on the newly drawn page
  if (App.bookmarks) {
    App.bookmarks.close();
    App.bookmarks = null;
  }
  if (window.weatherWidget && typeof window.weatherWidget.remove === 'function') {
    window.weatherWidget.remove();
    window.weatherWidget = null;
  }
}
