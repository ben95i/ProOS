// Automate Website - JavaScript Frontend
// by Ben Provenzano III

import { requestThrottle, setTheme, classDisplay, checkElemIsVisibleByID,
  addClassToElem, removeWindow, cleanString, base64URLSafeEncode, splitMenuData, 
  isNumeric, mapNumber, splitByDelimiter, sendCmd, timeoutInterval } from './utils.js';
import ToggleSwitch, { toggleSwitchCallback } from './switch.js';
import RoomStateManager from './roomstate.js';
import AnimatedBackground from './stars.js';
import ColorPrompt from './colorprompt.js';
import WeatherWidget from './weather.js';
import { deviceType } from './device.js';
import { SSIDpwd } from './wifi.js';

const MAIN_SITE = "Automate";
const BKM_INACTIVE = 0;
const BKM_OPEN_MODE = 1;
const BKM_EDIT_MODE = 2;
const ROOM_CONFIGS = {
  'lr':   { 
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
const SUBWOOFER_CLASS = 
  'red_btn_menu';
const VOLUME_ACTIONS = {
  'voldwnbtn': 'dwn',
  'volmutebtn': 'mute',
  'volupbtn': 'up'
};

const DELEGATED_LISTENERS = {};

window.App = {
  starBackground: null,
  roomStateManager: null,
  activeSwitches: [],
  fileData: [],
  serverCmdData: null,
  messagePopupTimer: null,
  dynMenuActive: 0,
  bookmarkState: 0,
  volPopupTimer: null,
  sysModel: null,
  activityTimeout: null,
  listenersReady: false,
  ledScrollData: [
    '00FF00', '8x8', '12.0'
  ]
};

// runs on page load
window.addEventListener("DOMContentLoaded", () => {
  loadPage();
});

// Each template below registers its own button handlers with
// registerListeners() directly beneath its markup; the groups merge into
// this one map, which the delegated dispatcher in
// registerDelegatedListeners() looks up on click. Delegation means handlers
// keep working for markup rendered later (room grids, bookmark buttons,
// svropt panels) and never need re-attaching after a re-render.
function registerListeners(listeners) {
  for (const [id, handler] of Object.entries(listeners)) {
    if (!(id in DELEGATED_LISTENERS)) {
      DELEGATED_LISTENERS[id] = handler;
    }
  }
}

function setupEventListeners() {
  if (!App.listenersReady) {
    App.listenersReady = true;
    // click/touch listener
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

// injects the markup for the given model, replacing any previous render;
// injected sections are visible by default -- no marker-class reveal needed
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
  // read device type
  App.sysModel = deviceType(); 
  destroyPageServices();
  renderPage(App.sysModel);
  if (App.sysModel === MAIN_SITE) {
    // load control menu saved state
    const room = localStorage.getItem("ctls-mode")
    selectRoom(room);
    // room status title-bar icon
    App.roomStateManager = new RoomStateManager('status_anchor');
    // weather title-bar icon
    window.weatherWidget = new WeatherWidget();
    window.weatherWidget.activate();
  }
  // set theme
  let currentTheme;
  let _theme = localStorage.getItem("main-color")
  if (_theme === null || _theme === undefined || _theme === "") {
    currentTheme = "#1f2051"; // dark blue
  } else {
    currentTheme = localStorage.getItem("main-color");
  }
  setTheme(currentTheme);
  // setup listeners
  setupEventListeners();
  // initialize background 
  App.starBackground = new AnimatedBackground('star-canvas');
  App.starBackground.activate();
  // activate timed services
  resetActivityTimer();
}

// runs on-each mouse click
function handleClicks(event) {
  resetActivityTimer();
  // disable click events when in bookmark edit mode
  if (App.bookmarkState === BKM_EDIT_MODE) {
    if (event.target.className !== "editFav__win") {
      event.stopPropagation();
    }
    return;
  }
  // don't hide menus when clicking these elements
  const NO_HIDE_ELEMENTS = [
      '.button',
      '.bookmarked__item',
      '.nohide',
    ].join(', ');
  if (event.target.closest(NO_HIDE_ELEMENTS)) return;
  removeDropdowns();
}

function resetActivityTimer() {
  activityBegin();
  clearTimeout(App.activityTimeout);
  App.activityTimeout = setTimeout(() => {
    activityStop();
  }, timeoutInterval() * 1000);
}

function activityBegin() {
  if (typeof App.starBackground !== 'undefined' && App.starBackground) {
    App.starBackground.resume();
  }
  if (App.sysModel === MAIN_SITE && App.roomStateManager) {
    App.roomStateManager.showRoomState();
  }
}

function activityStop() {
  if (typeof App.starBackground !== 'undefined' && App.starBackground) { 
    App.starBackground.pause();
  }
  if (App.sysModel === MAIN_SITE && App.roomStateManager) {
    App.roomStateManager.hideRoomState();
  }  
}

async function refreshRoomState() {
  if (App.sysModel === MAIN_SITE && App.roomStateManager) {
    App.roomStateManager.hideRoomState();
    App.roomStateManager.showRoomState();
  }  
}

window.removeDropdowns = function() {
  classDisplay("dd-content", "none");
  removeDynMenus();
  if (App.sysModel === MAIN_SITE) {
    removeBookmarks();
    if (App.activeSwitches && App.activeSwitches.length > 0) {
      App.activeSwitches.forEach(toggle => {
        if (typeof toggle.destroy === 'function') toggle.destroy();
      });
      App.activeSwitches = [];
    }
    if (window.weatherWidget && typeof window.weatherWidget.closeMenu === 'function') {
      window.weatherWidget.closeMenu();
    }
  }
}

function removeBookmarks() {
  const elem = document.getElementById("bookmarks");
  if (elem) elem.classList.remove("bookmark-editmode");
  const bkmButtons = document.getElementById("bookmark-buttons");
  if (bkmButtons) bkmButtons.remove();
  removeBookmarkSearch();
  closeBookmarkPrompt();
  App.bookmarkState = BKM_INACTIVE;
}

function openRoomDropdown() {
  const menuname = 'main-room-dropdown';
  let _elem = document.getElementById(menuname);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    removeDropdowns();
    // draw menu
    let menuDataString = ""; 
    for (const [roomKey, config] of Object.entries(ROOM_CONFIGS)) {
      if (roomKey !== App.currentRoom) {
        menuDataString += `${roomKey}|room|${config.name}\n`;
      }
    }
    drawMenu(menuDataString, menuname);
    // show dropdown
    _elem.style.display = 'block';
  }
}

function selectRoom(_mode) {
  // load configuration
  if (!ROOM_CONFIGS[_mode]) {
    // load first if not defined
    _mode = Object.keys(ROOM_CONFIGS)[0];
  }
  const config = ROOM_CONFIGS[_mode];
  const container = document.getElementById('room-grid-container');
  if (container) {
    // all of the grid's buttons (volume included) are handled by the
    // delegated dispatcher, so rendering the markup is all that's needed
    container.innerHTML = config.template();
  }
  // update dropdown
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
  // save state
  App.currentRoom = _mode;
  localStorage.setItem("ctls-mode", _mode);
};

// open URL in new tab
function GoToExtPage(_path) {
  let url = "https://" + _path;   
  window.open(url, "_blank");
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

// toggle dropdown menu's
function showMenu(_menu) {
  let _elem = document.getElementById(_menu);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
    return;
  }
  removeDropdowns();
  _elem.style.display = 'block';
}

//// page templates ////

function topNavTemplate(f) {
  return `
<div class="topnav nohide" data-app-injected>
  ${f.isServer ? `
  <a id="btn-dyn-mainmenu">
    <i class="fad fa-list"></i>
  </a>` : ''}
  ${f.isPiLike ? `
  <a id="btn-menu-pimainmenu">
    <i class="fad fa-list"></i>
  </a>` : ''}
  <a id="btn-dyn-thememenu">
    <i class="fad fa-palette"></i>
  </a>
  ${f.isServer ? `
  <a id="btn-show-bookmarks">
    <i class="fad fa-star"></i>
  </a>
  <a id="status_anchor">
  </a>
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
  'btn-dyn-mainmenu':    () => showDynFileMenu('mainmenu'),
  'btn-menu-pimainmenu': () => showMenu('pi-mainmenu'),
  'btn-dyn-thememenu':   () => showDynFileMenu('thememenu'),
  'btn-show-bookmarks':  () => showBookmarks(),
  'nav_stats_menu':      () => showStatusMenu()
});

// bookmark edit/add buttons -- rendered on demand by showBookmarks() right
// after the bookmarks star, and removed by removeBookmarks(); same
// render-only pattern as the room grids, no show/hide toggling
function bookmarkButtonsTemplate() {
  return `
  <div class="bookmark-buttons" id="bookmark-buttons">
    <a id="btn-edit-bookmark">
      <i class="fad fa-edit"></i>
    </a>
    <a id="btn-add-bookmark">
      <i class="fad fa-folder-plus"></i>
    </a>
  </div>`;
}
registerListeners({
  'btn-edit-bookmark': () => editBookmark(),
  'btn-add-bookmark':  () => addBookmark()
});

function navDropdownsTemplate(f) {
  // mainmenu / pi-mainmenu are direct children of .nav_dd_left, exactly like
  // thememenu / bookmarks / roomstatus always were (their old wrapper divs
  // only existed to carry the removed marker classes)
  return `
<div class="nav_dd_left" data-app-injected>
  ${f.isServer ? `
  <div class="dd-content dd-menu" id="mainmenu">
    <a id="btn-about-main"> About<i class="fad fa-info"></i></a>
  </div>` : ''}
  ${f.isPiLike ? `
  <div class="dd-content dd-menu" id="pi-mainmenu">
    <a id="btn-about-pi"> About<i class="fad fa-info"></i></a>
    <a id="btn-log-pi"> System<i class="fad fa-wrench"></i></a>
    <a id="btn-home-pi"> Home<i class="fad fa-home"></i></a>
  </div>` : ''}
  <div class="dd-content dd-menu" id="thememenu"></div>
  <div class="dd-content dd-menu" id="bookmarks"></div>
  <div class="dd-content dd-menu" id="roomstatus"></div>
</div>
<div class="nav_dd_right" data-app-injected>
  <div class="dd-content dd-menu ind-button" id="statsmenu"></div>
</div>`;
}
registerListeners({
  'btn-about-main': () => aboutPrompt(),
  'btn-about-pi':   () => aboutPrompt(),
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
    <!-- the active room's grid is rendered into this container on demand by
         selectRoom(); inactive rooms have no markup in the DOM at all -->
    <div class="column-right" id="room-grid-container"></div>
  </div>
</div>`;
}
registerListeners({
  'btn-dyn-ledcolormenu':  () => showDynFileMenu('ledcolormenu'),
  'btn-fc-color-prompt':   () => openColorPrompt('leds', 'hex', '3args'),
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
  <div class="dropdown">
    <button type="button" class="button dark_btn" id="main-room-btn">
      <i id="current-room-icon"></i>
      <span class="button__text" id="current-room-label"></span>
    </button>
    <div class="dd-content dd-button" id="main-room-dropdown"></div>
  </div>
  <button type="button" class="button" id="btn-open-log-svr">
    <span class="button__text"><i class="fad fa-terminal"></i> Server</span>
  </button>
</div>`;
}
registerListeners({
  'btn-media-link':     () => GoToExtPage('media.home'),
  'btn-yt-link':        () => GoToExtPage('youtube.com'),
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
  'main-room-btn':      () => openRoomDropdown(),
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
  <button type="button" class="button">
  </button>
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
  'bed_leds-colorcycle':  () => sendCmd('main', 'brxmit', 'bed_leds_cs'),
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
    <button type="button" class="button svropt_button" id="btn-menu-pve">
      <span class="button__text"><i class="fad fa-microchip"></i> PVE</span>
    </button>
    <div class="dd-content dd-button" id="pve-menu">
      <a id="btn-ext-pve"><i class="fad fa-link"></i> Open</a>
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
      <a id="btn-svr-restorexana"><i class="fad fa-eraser"></i> Erase</a>
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
  'btn-svr-restorexana':  () => serverAction('restorexana'),
  'btn-menu-pve':         () => showMenu('pve-menu'),
  'btn-ext-pve':          () => GoToExtPage('pve:8006'),
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
}, 250); // in (ms)

const throttledBedDown = requestThrottle(() => {
  sendCmd('main', 'brxmit', 'bed_adj_dwn');
}, 250); // in (ms)

async function bedPrompt() {
  const _winid = 'bed__prompt';
  if (document.getElementById(_winid)) return;
  const promptHTML = `
  <div id="${_winid}" class="prompt_win">
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
  </div>
  `;
  document.body.insertAdjacentHTML('beforeend', promptHTML);
  const bedprompt = document.getElementById(_winid);
  bedprompt.addEventListener('click', function handleButtonClicks(btn_evnt) {
    const clickedButton = btn_evnt.target.closest('button');
    if (!clickedButton) return;
    switch (clickedButton.id) {
      case 'bed__btn-up':
        throttledBedUp();
        break;
      case 'bed__btn-down':
        throttledBedDown();
        break;
      case 'bed__btn-close':
        bedprompt.removeEventListener('click', handleButtonClicks);
        bedprompt.remove();
        break;
    }
  });
}

/// server popup window ///

async function serverSend() {
  if (App.serverCmdData === null) {
    // load log data
    document.getElementById("logTextBox").value = "select an option.";
  } else {
    // send command
    sendCmd('main','server',App.serverCmdData);
    // display command sent
    document.getElementById("logTextBox").value += "\n" + App.serverCmdData + " command sent.";
    // scroll to bottom of page
    let txtArea = document.getElementById("logTextBox");
    txtArea.scrollTop = txtArea.scrollHeight;
  }
  sendBtnAlert("off");
  App.serverCmdData = null;
}

function sendBtnAlert(state) {
  let _class = "selected_btn";
  let _elem = "sendButton";
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

function closeServerOptions(){
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

function setTextScrollTitle(){
  let title = document.getElementById('textScrollTitle');
  const color = App.ledScrollData[0];
  const font = App.ledScrollData[1];
  const speed = App.ledScrollData[2];
  title.style.color = '#' + color;
  title.textContent = "Textscroll " + font + ' ' + speed + 'x';
}

function clearTextScrollBox(){
  let textbox = document.getElementById('textScrollBox');
  textbox.placeholder = "Enter message";
  textbox.value = '';
}

function openTextScroll(){
  classDisplay("textscroll__prompt", "block");
  clearTextScrollBox();
  setTextScrollTitle();
}

function closeTextScroll(){
  classDisplay("textscroll__prompt", "none");
  clearTextScrollBox();
  removeDropdowns();
}

function clearLEDsMessage(){
  clearTextScrollBox();
  sendCmd('leds', 'stop');
}

function setTextScrollColor(color){
  App.ledScrollData[0] = color;
  setTextScrollTitle();
}

function setTextScrollFont(font){
  App.ledScrollData[1] = font;
  setTextScrollTitle();
}

function setTextScrollSpeed(speed){
  App.ledScrollData[2] = speed;
  setTextScrollTitle();
}

function sendTextScroll(){
  let textbox = document.getElementById('textScrollBox');
  const message = cleanString(textbox.value.replaceAll("|", "-"));
  if (message === null || message == "") {
    textbox.placeholder = "Message cannot be empty";
    return;
  }
  textbox.placeholder = "Sending message..";
  textbox.value = '';
  const color = App.ledScrollData[0];
  const font = App.ledScrollData[1];
  const speed = App.ledScrollData[2];
  const commands = `${message}|${color}|${font}|${speed}`;
  const encoded = base64URLSafeEncode(commands);
  sendCmd('leds', 'textscroll', encoded).then((data) => {
    if (data === null || data == "" ) {
      textbox.placeholder = "Error sending message!"; 
    } else {
      textbox.placeholder = "Message sent";
    }
    setTimeout(function(){
      textbox.value = message;
    }, 1000); // in ms
  });
}

/////// About Window ///////

async function aboutPrompt() {
  const _winid = 'about__prompt';
  if (document.getElementById(_winid)) return;
  const curDate = new Date();
  const curYear = curDate.getFullYear();
  const curQtr = Math.ceil((curDate.getMonth() + 1) / 3);
  const promptHTML = `
  <div id="${_winid}" class="prompt_win">
    <div class="about__title">${App.sysModel}</div>
    <img src="img/automate.png" id="about__img" />
    <div class="about__text">by Ben Provenzano III</div>
    <div class="about__text">${curYear}.${curQtr} Release</div>
    <button type="button" class="button" id="about__btn">
      <i class="fad fa-times-octagon right-padding"></i>
      <span class="button__text">Close</span>
    </button>
  </div>
  `;
  document.body.insertAdjacentHTML('beforeend', promptHTML);
  const aboutprompt = document.getElementById(_winid);
  aboutprompt.addEventListener('click', function handleButtonClicks(btn_evnt) { 
    const clickedButton = btn_evnt.target.closest('button');
    if (!clickedButton) return; 
    aboutprompt.removeEventListener('click', handleButtonClicks); 
    aboutprompt.remove();
  });
}

/////// Volume Controls ///////

function sendVol(_cmd) {
  const room = App.currentRoom;
  const config = ROOM_CONFIGS[room];
  const submode = subwooferVolumeEnabled();
  if (submode === true) {
    sendCmd('main', config.voltarget, 'sub' + _cmd); // living room subwoofers
  } else {
    setAmpVolume(_cmd, config.voltarget, config.volmax); // living room system
  }
}

function subwooferVolumeEnabled() {
  const volKeys = Object.keys(VOLUME_ACTIONS);
  if (volKeys.length === 0) return false;
  const firstKey = volKeys[0]; 
  const elementId = `${App.currentRoom}-${firstKey}`; 
  const element = document.getElementById(elementId);
  if (element) {
    return element.classList.contains(SUBWOOFER_CLASS);
  }
  return false;
}

function subwooferVolumeMode(enable) {
  const volKeys = Object.keys(VOLUME_ACTIONS);
  volKeys.forEach(key => {
    const elementId = `${App.currentRoom}-${key}`;
    const element = document.getElementById(elementId);
    if (element) {
      if (enable === true) {
        element.classList.add(SUBWOOFER_CLASS);
      } else {
        element.classList.remove(SUBWOOFER_CLASS);
      }
    }
  });
}

function setAmpVolume(_cmd, _device, _maxvol) {
  sendCmd('main', _device, 'vol' + _cmd).then((data) => { // GET request
    let amp_vol = cleanString(data);
    if (isNumeric(amp_vol) === true) {
      // re-map volume data to 0-100%, show volume pop-up
      let display_value;
      if (_maxvol == 100) {
        display_value = Number(amp_vol);
      } else {
        display_value = Number(Math.round(mapNumber(amp_vol, 0, _maxvol, 0, 100)));
      }
      showVolumePopup(display_value); 
    } else {
      if (amp_vol == "") {
        // empty response = power off
        showVolumePopup(-1);
      }
    }
  });
}

// add HTTPs prefix if not defined
function addHTTPtoURL(linkin) {
  let linkout;
  if (linkin === "" || linkin === null) {
    linkout = "";
  } else {  
    if (linkin.toLowerCase().startsWith('http://', 0) || 
        linkin.toLowerCase().startsWith('https://', 0)) {
      linkout = linkin;
    } else {
      linkout = "https://" + linkin;
    }
  }
  return linkout;
}

//// Bookmarks Menu ////

// close add / edit window
function closeBookmarkPrompt() {
  // re-enable click outside of window
  document.querySelectorAll("*:not(#editFav__prompt)").forEach(elem1 => {
    elem1.style.pointerEvents = "";
  }); 
  // un-highlight selected item
  for (let idx = 0; idx <= App.fileData.length; idx++) {
    const _menuid = "menu-" + idx;
    const elem2 = document.getElementById(_menuid);
    if (elem2) {
      if(elem2.classList.contains('dd-selected')) {
        elem2.classList.remove("dd-selected");
      }
    }
  }
  const favPrompt = document.getElementById("editFav__prompt");
  if (favPrompt) {
    favPrompt.remove();
  }  
}

function showBookmarks() {
  // hide menu if clicked while open
  if (App.bookmarkState !== BKM_INACTIVE) {
    removeDropdowns();
    return;
  }
  // draw bookmarks
  showDynFileMenu("bookmarks");
  // render the edit/add buttons next to the bookmarks star on demand;
  // removeBookmarks() removes them again (no show/hide toggling)
  renderBookmarkButtons();
  // link open mode
  App.bookmarkState = BKM_OPEN_MODE;
}

function renderBookmarkButtons() {
  if (document.getElementById("bookmark-buttons")) return;
  const starBtn = document.getElementById("btn-show-bookmarks");
  if (!starBtn) return;
  starBtn.insertAdjacentHTML("afterend", bookmarkButtonsTemplate());
}

function showBookmarkSearch() {
  const elem = document.getElementById("bookmarks");
  if (elem) {
    // draw menu container with search icon + text input box
    const searchMenuItem = document.createElement("a");
    searchMenuItem.id = "bookmark__search";
    searchMenuItem.classList.add("nohide");
    searchMenuItem.innerHTML = html`
      <input type="text" id="bookmark__search_txt" class="nohide" placeholder="Search here..." autocorrect="off" autocapitalize="none">
    `;
    elem.appendChild(searchMenuItem);
    const searchBox = document.getElementById("bookmark__search_txt");
    // focus keyboard on search text box
    searchBox.focus();
    // filter list as the user types
    searchMenuItem.addEventListener("input", function searchInputListener() {
      filterBookmarkItems(searchBox.value);
    });
  }
}

function filterBookmarkItems(termIn) {
  const searchTerm = termIn.toLowerCase();
  const bookmarks = document.getElementById("bookmarks");
  if (bookmarks) {
    [...bookmarks.getElementsByClassName("bookmarked__item")].forEach((elem) => {
      const bookmarkText = elem.textContent.toLowerCase();
      elem.style.display = bookmarkText.includes(searchTerm) ? "list-item" : "none";
    });
  }
}

function removeBookmarkSearch() {
  const search = document.getElementById("bookmark__search");
  if (search) search.remove();
  const search_txt = document.getElementById("bookmark__search_txt");
  if (search_txt) search_txt.remove();
}

async function lookupURL() {
  const urlBoxElem = document.getElementById("editFav__urlbox");
  const nameBoxElem = document.getElementById("editFav__namebox");
  if (urlBoxElem && nameBoxElem) {
    nameBoxElem.value = "Processing...";
    // read URL box
    let urlin = urlBoxElem.value;
    if (urlin === null || urlin == "") {
      // read from clipboard if URL empty
      urlin = await navigator.clipboard.readText();
    }
    // add HTTPs to URL
    let url = addHTTPtoURL(urlin);
    urlBoxElem.value = url;
    let encoded_url = base64URLSafeEncode(url);
    // search for URLs title
    sendCmd('main', 'sitelookup', encoded_url).then((data) => {
      if (data === null || data == "") {
        // URL lookup failed actions
        nameBoxElem.value = "Not Found";
        setTimeout(function() {
          if (urlBoxElem && nameBoxElem) {
            nameBoxElem.value = null;
            urlBoxElem.value = null;
          }
        }, 2000);
      } else { 
        // remove invalid characters, write to name box
        nameBoxElem.value = cleanString(data);
      }
    });
  }
}

function shiftMenuUp(elem) {
  if (elem) {
    if(elem.previousElementSibling)
      elem.parentNode.insertBefore(elem, elem.previousElementSibling);
  }
}

function shiftMenuDown(elem) {
  if (elem) {
    if(elem.nextElementSibling)
      elem.parentNode.insertBefore(elem.nextElementSibling, elem);
  }
}

function saveBookmarks() {
  let _file = "";
  const bookmarks = document.getElementById("bookmarks");
  if (!bookmarks) {
    return;
  }
  // loop through edited bookmarks
  [...bookmarks.getElementsByClassName('bookmarked__item')].forEach(elem => {
    if (elem) {
      const url = Object.values(elem.url).join("");
      const name = elem.innerText;
      // build output file
      _file += url + "|bkmrk|" + name + "\n";
    }
  });
  // transmit file
  savePOST('bookmarks', [_file]);
}

function editBookmark() {
  // hide menu if clicked while open
  if (App.bookmarkState === BKM_EDIT_MODE) {
    removeDropdowns();
    return;
  }
  enableEditAddMode();
}

function addBookmark() {
  enableEditAddMode();
  // create new unique menu ID (0 -> 0, 1 -> 1, n>1 -> n-1)
  const _datalen = App.fileData.length;
  const _newid = _datalen <= 1 ? _datalen : _datalen - 1;
  // add temporary entry to menu array
  App.fileData.push("placeholder");
  // create placeholder menu item
  const navElement = document.getElementById("bookmarks");
  const _elm = document.createElement("a");
  _elm.id = `menu-${_newid}`;
  _elm.innerText = "New Bookmark";
  _elm.classList.add("bookmarked__item");
  // store URL
  Object.defineProperty(_elm, "url", {
    enumerable: false,
    writable: true,
    value: "about:blank"
  });
  // define click action
  _elm.addEventListener("click", function () {
    clickBookmark(_newid);
  });
  // highlight selected item
  _elm.classList.add("dd-selected");
  // insert item below
  navElement.insertBefore(_elm, navElement.firstChild);
  // add new window
  drawBookmarkPrompt(true, null, null, _elm);
}

function enableEditAddMode() {
  const elem = document.getElementById("bookmarks");
  if (elem) { // change color of menu
    elem.classList.add("bookmark-editmode");
  }
  // remove
  removeBookmarkSearch();
  App.bookmarkState = BKM_EDIT_MODE;
  closeBookmarkPrompt();
}

function clickBookmark(id) {
  const elem = document.getElementById(`menu-${id}`);
  if (!elem) return;
  const url = elem.url; // Object.values(str).join("") returned the same string
  const name = elem.innerText;
  if (App.bookmarkState === BKM_OPEN_MODE) {
    if (!(url === null || url == "" || url == "about:blank")) {
      // open URL in new tab
      window.open(url, "_blank");
    }
  }
  if (App.bookmarkState === BKM_EDIT_MODE) {
    // edit bookmark item
    closeBookmarkPrompt();
    elem.classList.add("dd-selected"); // highlight selected item
    drawBookmarkPrompt(false, url, name, elem);
  }
}

async function drawBookmarkPrompt(add, url, name, elem) {
  // create window: banner text, name box, URL box, six action buttons
  const editFavPrompt = document.createElement("div");
  editFavPrompt.className = "editFav__win";
  editFavPrompt.id = "editFav__prompt";
  editFavPrompt.classList.add("prompt_win");
  const bannerText = add === true ? "Add Bookmark" : "Edit Bookmark";
  editFavPrompt.innerHTML = html`
    <div class="editFav__toptext">${bannerText}</div>
    <input type="text" id="editFav__namebox" class="editFav__textbox"
           autocorrect="off" autocapitalize="none">
    <input type="text" id="editFav__urlbox" class="editFav__textbox"
           autocorrect="off" autocapitalize="none">
    <button class="editFav__button button fad fa-arrow-up" type="button"></button>
    <button class="editFav__button button fad fa-search" type="button"></button>
    <button class="editFav__button button fad fa-arrow-down" type="button"></button>
    <button class="editFav__button button fad fa-save" type="button"></button>
    <button class="editFav__button button fad fa-times-octagon" type="button"></button>
    <button class="editFav__button button fad fa-trash-alt" type="button"></button>
  `;
  // element references (buttons in DOM order: row1 up/lookup, row2 down/save,
  // row3 cancel/delete)
  const editFavText = editFavPrompt.querySelector(".editFav__toptext");
  const nameBox = editFavPrompt.querySelector("#editFav__namebox");
  const urlBox = editFavPrompt.querySelector("#editFav__urlbox");
  const [editFavUpBtn, editFavLookupBtn, editFavDownBtn,
         editFavSaveBtn, editFavCancelBtn, editFavDeleteBtn] =
    editFavPrompt.querySelectorAll("button");
  // set values via properties (null coerces to "" exactly as before)
  nameBox.value = name;
  urlBox.value = url;
  if (add === true) { // add new mode placeholders
    nameBox.placeholder = "Enter Name";
    urlBox.placeholder = "Enter URL";
  }
  // display window on page
  document.body.appendChild(editFavPrompt);
  // handle button actions
  editFavPrompt.addEventListener("click", function handleButtonClicks(btn_event) {
    if (btn_event.target.tagName !== "BUTTON") { return; }
    // move up button action
    if (btn_event.target === editFavUpBtn) {
      shiftMenuUp(elem);
    }
    // move down button action
    if (btn_event.target === editFavDownBtn) {
      shiftMenuDown(elem);
    }
    // cancel button action
    if (btn_event.target === editFavCancelBtn) {
      editFavPrompt.removeEventListener("click", handleButtonClicks);
      removeDropdowns();
    }
    // lookup URL action
    if (btn_event.target === editFavLookupBtn) {
      lookupURL();
    }
    // save button action
    if (btn_event.target === editFavSaveBtn) {
      // do not allow empty URL or name
      let stopsave = false;
      if (nameBox.value === null || nameBox.value == "") {
        nameBox.placeholder = "Name cannot be empty";
        stopsave = true;
      }
      if (urlBox.value === null || urlBox.value == "") {
        urlBox.placeholder = "URL cannot be empty";
        stopsave = true;
      }
      if (stopsave === true) {
        return;
      }
      // update changed values
      if (elem) {
        // save name to menu object
        const _boxname = nameBox.value.replaceAll("|", "-");
        // name length limiter
        const maxNameLength = 42;
        if (_boxname.length >= maxNameLength) {
          elem.innerText = `${_boxname.substring(0, maxNameLength)}...`;
        } else {
          elem.innerText = _boxname;
        }
        // save URL to menu object
        const _boxurl = addHTTPtoURL(urlBox.value.replaceAll("|", "-"));
        elem.url = _boxurl;
        urlBox.value = _boxurl;
        editFavText.innerHTML = "Changes Saved";
      }
      // save to file
      saveBookmarks();
      // close dropdown
      editFavPrompt.removeEventListener("click", handleButtonClicks);
      closeBookmarkPrompt();
    }
    // delete button action
    if (btn_event.target === editFavDeleteBtn) {
      elem.remove();
      // save to file
      saveBookmarks();
      // close dropdown
      editFavPrompt.removeEventListener("click", handleButtonClicks);
      closeBookmarkPrompt();
    }
  });
}

// Tag function for building DOM markup with template literals.
// Strips whitespace between tags so the resulting DOM contains no stray
// text nodes -- output is identical to element-by-element createElement()
// construction (whitespace text nodes between inline-block elements would
// otherwise render as visible gaps).
function html(strings, ...values) {
  return strings
    .reduce((out, str, i) => out + str + (i < values.length ? values[i] : ""), "")
    .replace(/>\s+</g, "><")
    .trim();
}

function showPopup(message) {
  const win_name = "msg-popup";
  const text_name = "msg-text";
  let msgpopwin = document.getElementById(win_name);
  if (!msgpopwin) { // window does not exist
    // draw window (popup container + message text box)
    msgpopwin = document.createElement("div");
    msgpopwin.classList.add("popup-alert");
    msgpopwin.id = win_name;
    msgpopwin.innerHTML = html`<div id="${text_name}"></div>`;
    // add to page
    document.body.appendChild(msgpopwin);
  }
  msgpopwin.classList.remove("fade-out");
  // set message text
  const msgtext = document.getElementById(text_name);
  msgtext.innerHTML = message;
  // reset hide window timeout
  clearTimeout(App.messagePopupTimer);
  // hide window after delay
  App.messagePopupTimer = setTimeout(() => {
    // fade out window
    msgpopwin.classList.add("fade-out");
    // wait for the transition to end before removing window
    msgpopwin.addEventListener("animationend", () => {
      if (msgpopwin.isConnected) {
        msgpopwin.remove();
      }
    }, { once: true });
  }, 4000); // hide delay in (ms)
}

/// Temperature & Humidity ///

async function showTempHumidity() {
  const _winid = "temp__prompt";
  if (document.getElementById(_winid)) return;
  // create window: banner text, thermometer grid, buttons row
  const tempprompt = document.createElement("div");
  tempprompt.id = _winid;
  tempprompt.classList.add("prompt_win");
  tempprompt.innerHTML = html`
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
  `;
  // add window to DOM
  document.body.appendChild(tempprompt);
  // grab button references (cancel first, refresh second)
  const [tempcancelb, temprefreshb] = tempprompt.querySelectorAll("button");
  // call API for data
  getTemperatureData();
  // button actions
  tempprompt.addEventListener("click", function handleButtonClicks(btn_evnt) {
    if (btn_evnt.target.tagName !== "BUTTON") { return; }
    // close button
    if (btn_evnt.target === tempcancelb) {
      tempprompt.removeEventListener("click", handleButtonClicks);
      tempprompt.remove();
    }
    // refresh button
    if (btn_evnt.target === temprefreshb) {
      getTemperatureData();
    }
  });
}

function getTemperatureData() {
  sendCmd("main", "brxmit", "roomth")
    .then((data) => {
      const resp = cleanString(data);
      // extract numerics
      const resp_arr = resp.split("~");
      const temp_elm = document.getElementById("thermo__1");
      const humd_elm = document.getElementById("thermo__2");
      if (resp_arr.length == 2) {
        // valid response
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

function pushTempDataToThermos(resp_arr, temp_elm, humd_elm) {
  // thermometer limits
  const minTemp = 25;
  const maxTemp = 100;
  const minHumidity = 5;
  const maxHumidity = 100;
  // verify elements exist
  if (!(temp_elm && humd_elm)) {
    return;
  }
  // validate response
  if (resp_arr.length == 2) {
    let tvalue = resp_arr[0];
    let hvalue = resp_arr[1];
    // set temperature
    temp_elm.dataset.value = `${tvalue}\u00B0F`;
    if (tvalue >= maxTemp) { tvalue = maxTemp; }
    if (tvalue <= minTemp) { tvalue = minTemp; }
    temp_elm.style.height = `${(tvalue - minTemp) / (maxTemp - minTemp) * 100}%`;
    // set humidity
    humd_elm.dataset.value = `${hvalue}%`;
    if (hvalue >= maxHumidity) { hvalue = maxHumidity; }
    if (hvalue <= minHumidity) { hvalue = minHumidity; }
    humd_elm.style.height = `${(hvalue - minHumidity) / (maxHumidity - minHumidity) * 100}%`;
  } else {
    pushTempErrorThermos(temp_elm, humd_elm);
  }
}

function pushTempErrorThermos(temp_elm, humd_elm) {
  // verify elements exist
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

function piWiFiPrompt(_winid) {
  return new Promise((resolve) => {
    // create window: SSID label/box, password label/box, cancel + save buttons
    const pinetprompt = document.createElement("div");
    pinetprompt.id = _winid;
    pinetprompt.classList.add("prompt_win");
    pinetprompt.innerHTML = html`
      <div>Network (SSID):</div>
      <input id="pinet__ssidbox">
      <div>Password:</div>
      <input id="pinet__passbox" type="password">
      <button class="button pinet__btn" type="button">Cancel</button>
      <button class="button pinet__btn" type="button">Save</button>
    `;
    document.body.appendChild(pinetprompt);
    // element references
    const pinettext1 = pinetprompt.querySelector("div"); // SSID label
    const pinetssidbox = document.getElementById("pinet__ssidbox");
    const pinetpassbox = document.getElementById("pinet__passbox");
    const [pinetcancelb, pinetokbutton] = pinetprompt.querySelectorAll("button");
    // focus on SSID text box
    pinetssidbox.focus();
    // control logic
    function cancelWiFi() {
      pinetssidbox.value = "";
      pinetpassbox.value = "";
      pinetprompt.remove();
      resolve(null); // settle the promise on cancel
    }
    function sendWiFiData() {
      if (pinetssidbox.value === "") {
        pinettext1.innerHTML = "enter SSID!";
      } else if (pinetpassbox.value === "") {
        pinettext1.innerHTML = "enter password!";
      } else {
        // concat and convert to base64
        const _wifidata = base64URLSafeEncode(`${pinetssidbox.value}|$|${pinetpassbox.value}`);
        resolve(_wifidata);
        cancelWiFi();
      }
    }
    // button clicks
    pinetprompt.addEventListener("click", function handleButtonClicks(e) {
      if (e.target.tagName !== "BUTTON") return;
      if (e.target === pinetokbutton) {
        sendWiFiData();
      }
      if (e.target === pinetcancelb) {
        cancelWiFi();
      }
    });
    // key focused on SSID box
    pinetssidbox.addEventListener("keyup", function handleSSID(ssid_evnt) {
      if (ssid_evnt.key === "Enter") {
        pinetpassbox.focus();
      } else if (ssid_evnt.key === "Escape") {
        cancelWiFi();
      }
    });
    // key focused on password box
    pinetpassbox.addEventListener("keyup", function handlePass(pass_evnt) {
      if (pass_evnt.key === "Enter") {
        sendWiFiData();
      } else if (pass_evnt.key === "Escape") {
        cancelWiFi();
      }
    });
  });
}

async function wifiPrompt() {
  const _winid = "wifi__prompt";
  if (document.getElementById(_winid)) return;
  // create window: banner text, QR image, password text, close button
  const wifiprompt = document.createElement("div");
  wifiprompt.id = _winid;
  wifiprompt.classList.add("prompt_win");
  wifiprompt.innerHTML = html`
    <div>Scan for WiFi Access</div>
    <img src="img/wifi.png" id="wifi__img">
    <div id="wifi__pwdtxt"></div>
    <button class="button" type="button" id="wifipmt__btn">Close</button>
  `;
  document.body.appendChild(wifiprompt);
  // password text set separately (value is data, not markup)
  document.getElementById("wifi__pwdtxt").innerHTML = atob(SSIDpwd());
  wifiprompt.addEventListener("click", function handleButtonClicks(btn_evnt) {
    if (btn_evnt.target.tagName !== "BUTTON") { return; }
    wifiprompt.removeEventListener("click", handleButtonClicks);
    wifiprompt.remove();
  });
}

async function getPassword(_type) {
  try {
    removeDropdowns();
    const result = await passwordPrompt();
    if (result !== null && result !== "" && result !== undefined) {
      savePOST(_type, [result]);
    }
  } catch (e) {
    // user cancelled -- nothing to do
  }
}

function passwordPrompt() {
  const _winid = "pass__prompt_win";
  if (document.getElementById(_winid)) return null;
  const pwprompt = document.createElement("div");
  pwprompt.id = _winid;
  pwprompt.classList.add("prompt_win");
  pwprompt.innerHTML = html`
    <div>Enter password:</div>
    <input id="pass__textbox" type="password">
    <div id="pass__prmbtnctr">
      <button class="button pass__prompt_btn" type="button">Cancel</button>
      <button class="button pass__prompt_btn" type="button">Send</button>
    </div>
  `;
  document.body.appendChild(pwprompt);
  // element references
  const pwinput = document.getElementById("pass__textbox");
  const [pwcancelb, pwokbutton] = pwprompt.querySelectorAll("button");
  pwinput.focus();
  return new Promise(function (resolve, reject) {
    pwprompt.addEventListener("click", function handleButtonClicks(click_evnt) {
      if (click_evnt.target.tagName !== "BUTTON") return;
      pwprompt.removeEventListener("click", handleButtonClicks);
      if (click_evnt.target === pwokbutton) {
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

function showVolumePopup(vol) {
  const win_name = "vol-popup";
  const text_name = "vol-text";
  const grid_name = "vol-grid";
  const bar_name = "vol-bar";
  let volpopwin = document.getElementById(win_name);
  if (!volpopwin) { // window does not exist
    // draw window: volume text box + progress bar grid
    volpopwin = document.createElement("div");
    volpopwin.classList.add("popup-alert");
    volpopwin.id = win_name;
    volpopwin.innerHTML = html`
      <div id="${text_name}"></div>
      <div class="progress_grid" id="${grid_name}">
        <div class="bar_progress" id="${bar_name}"></div>
      </div>
    `;
    // add to page
    document.body.appendChild(volpopwin);
  }
  volpopwin.classList.remove("fade-out");
  // select elements in window
  const voltext = document.getElementById(text_name);
  const volgrid = document.getElementById(grid_name);
  const volbar = document.getElementById(bar_name);
  // set volume display
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
  // reset hide window timeout
  clearTimeout(App.volPopupTimer);
  // hide window after delay
  App.volPopupTimer = setTimeout(() => {
    // fade out window
    volpopwin.classList.add("fade-out");
    // wait for the transition to end before removing window
    volpopwin.addEventListener("animationend", () => {
      if (volpopwin.isConnected) {
        volpopwin.remove();
      }
    }, { once: true });
  }, 3000); // hide delay in (ms)
}

function showHiFiMoreMenu() {
  const offcode = -1;
  const target = 'lrxmit';
  const menuname = 'hfmoremenu';
  let _elem = document.getElementById(menuname);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    removeDropdowns();
    _elem.style.display = 'block';
    // start spinner animation
    let btnText = document.getElementById(menuname + '-text');
    let btnSpinner = document.getElementById(menuname + '-spinner');
    btnText.style.visibility = 'hidden';
    btnSpinner.classList.add('btn-spinner');
    sendCmd('main', target, 'curhpfvollim').then((data) => { // GET request
      const resp = cleanString(data); // remove newlines
      let _indtype = 'noind'; // indicator type
      let _menudata = '';
      if (isNumeric(resp) === true || resp.includes('~')) {
        if (resp == offcode) {
          _menudata += menuname + '-offline|blkind|Offline\n';
        } else if (resp == 0) { 
          _menudata += menuname + '-muted|noind|Muted\n';
        } else {
          const field = resp.split("~"); // volLimitState ~ hpfState
          const limit_state = field[0]; 
          const hpf_state = field[1];
          // subwoofer volume toggle
          _menudata += 'subs|subs|Subs Gain\n';       
          // low-cut filter toggle
          if (hpf_state == '0') { _indtype = 'blkind'; }
          if (hpf_state == '1') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~togglehpf|' + _indtype + '|Low Cut\n';       
          // volume limiter toggle
          if (limit_state == '0') { _indtype = 'blkind'; }
          if (limit_state == '1') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~vlimit|' + _indtype + '|vLimiter\n';        
        }
      } else {
        _menudata += menuname + '-error|redind|Error\n';
      }
      // stop spinner animation
      btnText.style.visibility = 'visible';
      btnSpinner.classList.remove('btn-spinner');
      // draw menu items
      drawMenu(_menudata, menuname);
    });
  }
}

function showHiFiAmpInput() {
  const offcode = -1;
  const target = 'lrxmit';
  const menuname = 'hfinmenu';
  let _elem = document.getElementById(menuname);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    removeDropdowns();
    _elem.style.display = 'block';
    // start spinner animation
    let btnText = document.getElementById(menuname + '-text');
    let btnSpinner = document.getElementById(menuname + '-spinner');
    btnText.style.visibility = 'hidden';
    btnSpinner.classList.add('btn-spinner');
    sendCmd('main', target, 'curinput').then((data) => { // GET request
      const resp = cleanString(data); // remove newlines
      // draw menu items
      let _menudata = '';
      if (isNumeric(resp) === true) {
        if (resp == offcode) {
          _menudata += menuname + '-offline|blkind|Offline\n';
        } else { // online
          // auto input
          let _indtype = 'blkind'; // indicator type
          if (resp == '6') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~autodac|' + _indtype + '|Auto\n';
          // PC input
          _indtype = 'blkind';
          if (resp == '1') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~usb|' + _indtype + '|USB\n';
          // phono input
          _indtype = 'blkind';
          if (resp == '3') { _indtype = 'grnind'; }      
          _menudata += 'cmd~' + target + '~phono|' + _indtype + '|Phono\n';
          // optical input
          _indtype = 'blkind';
          if (resp == '5') { _indtype = 'grnind'; } 
          _menudata += 'cmd~' + target + '~opt|' + _indtype + '|Optical\n';
          // coaxial input
          _indtype = 'blkind';
          if (resp == '4') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~coax|' + _indtype + '|Coaxial\n';
          // aux analog input
          _indtype = 'blkind';
          if (resp == '2') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~aux|' + _indtype + '|Aux\n';
        }
      } else {
        _menudata += menuname + '-error|redind|Error\n';
      }
      // stop spinner animation
      btnText.style.visibility = 'visible';
      btnSpinner.classList.remove('btn-spinner');
      // draw menu items
      drawMenu(_menudata, menuname);
    });
  }
}

function showBedroomAmpInput() {
  const target = 'brpi';
  const menuname = 'brinpmenu';
  const offcode = 0;
  let _elem = document.getElementById(menuname);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    removeDropdowns();
    _elem.style.display = 'block';
    // start spinner animation
    let btnText = document.getElementById('ampinp-text');
    let btnSpinner = document.getElementById('ampinp-spinner');
    btnText.style.visibility = 'hidden';
    btnSpinner.classList.add('btn-spinner');
    sendCmd('main', target, 'inputstate').then((data) => { // GET request
      const resp = cleanString(data); // remove newlines
      let _menudata = '';
      if (isNumeric(resp) === true) {
        if (resp == offcode) {
          _menudata += menuname + '-offline|blkind|Offline\n';
        } else { // online
          // apple TV input 
          let _indtype = 'blkind';
          if (resp == '2') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~opt-a|' + _indtype + '|USB\n';
          // coaxial input
          _indtype = 'blkind';
          if (resp == '3') { _indtype = 'grnind'; }      
          _menudata += 'cmd~' + target + '~coaxial|' + _indtype + '|Coaxial\n';
          // aux optical input
          _indtype = 'blkind';
          if (resp == '1') { _indtype = 'grnind'; } 
          _menudata += 'cmd~' + target + '~opt-b|' + _indtype + '|Optical\n';
          // aux analog input
          _indtype = 'blkind';
          if (resp == '4') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~aux|' + _indtype + '|Aux\n';
        }
      } else {
        _menudata += menuname + '-error|redind|Error\n';
      }
      // stop spinner animation
      btnText.style.visibility = 'visible';
      btnSpinner.classList.remove('btn-spinner');
      // draw menu items
      drawMenu(_menudata, menuname);
    });
  }
}

function showPowerMenu(target, menu = null) {
  if (menu == '' || menu == null) menu = target;
  let _elem = document.getElementById(menu + '-menu');
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    removeDropdowns();
    // start spinner animation
    let btnText = document.getElementById(menu + '-text');
    let btnSpinner = document.getElementById(menu + '-spinner');
    btnText.style.visibility = 'hidden';
    btnSpinner.classList.add('btn-spinner');
    // show menu
    _elem.style.display = 'block';
    // custom actions
    let action = 'main';
    if (menu == 'fc_leds') action = 'leds';
    // transmit request
    sendCmd(action, target, menu).then((data) => {
      let _menudata = '';
      const [cmd1, cmd2] = splitByDelimiter(cleanString(data), '~');
      if (isNumeric(cmd1) === false || data == null) {
        _menudata += `${menu}-error|redind|Error\n`;
      } else {
        // draw toggle and/or slider
        const isDualToggleSlider = menu.endsWith('_dual');
        const isSingleLedSlider = menu.endsWith('_leds');
        const isFadeCandyLeds = (menu === 'fc_leds');
        const isDefinedMenu = (isDualToggleSlider || isSingleLedSlider || isFadeCandyLeds);
        if (isDefinedMenu === false || isDualToggleSlider === true) {
          if (cmd1 == '1' || cmd1 == '0') {
            _menudata += `${cmd1}~${target}~${menu}|toggle|\n`;
          }
        }
        if (isDualToggleSlider) {
          _menudata += `${cmd2}~${target}~${menu}|slider|\n`;
        } else if (isFadeCandyLeds) {
          _menudata += `${cmd1}~leds~fc|slider|\n`;
        } else if (isSingleLedSlider) {
          _menudata += `${cmd1}~${target}~${menu}|slider|\n`; 
        }
      }
      // stop spinner animation
      btnText.style.visibility = 'visible';
      btnSpinner.classList.remove('btn-spinner');
      // draw menu items
      drawMenu(_menudata, menu + '-menu');
    });
  }
}

function showStatusMenu() {
  const _menu = 'statsmenu';
  let statIcon = document.getElementById('navstats-icon');
  let statSpinner = document.getElementById('navstats-spinner');
  let _elem = document.getElementById(_menu);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
    // stop spinner animation
    statIcon.style.visibility = 'visible';
    statSpinner.classList.remove('right-nav-spinner');
  } else {
    removeDropdowns();
    // start spinner animation
    statIcon.style.visibility = 'hidden';
    statSpinner.classList.add('right-nav-spinner');
    // show menu
    _elem.style.display = 'block';
    sendCmd('main', 'status').then((data) => { // GET request
      // draw menu items
      drawMenu(data, _menu);
      // stop spinner animation
      statIcon.style.visibility = 'visible';
      statSpinner.classList.remove('right-nav-spinner');
    });
  }   
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
    serverSend(0);
  }
}

// load server action
function serverAction(cmd) {
  App.serverCmdData = cmd;
  // change color of send button
  sendBtnAlert("on");
}

function openLogWindow() {
  // open server log window
  closePopup();
  // show log form window
  document.getElementById("logForm").style.display = "block";
  // load log data
  loadLog('sysout');
}

// load entire text file
async function loadLog(file) {
  // build URL / append data
  const url = location.protocol + "//" + location.hostname + "/api/read?file=" + file;
  // read file action
  fetch(url, {
    method: 'GET'
  })
  .then((response) => {
    // check for HTTP errors
    if (!response.ok) {
      // trigger the catch block
      throw new Error;
    }
    // proceed to the next then if successful
    return response.json(); 
  })
  .then(data => {
    // display text on page
    const elmid = "logTextBox";
    document.getElementById(elmid).value = data;
    // scroll to bottom of page
    let txtArea = document.getElementById(elmid);
    txtArea.scrollTop = txtArea.scrollHeight;
  })
  .catch(error => {
    // This catches both the thrown HTTP error and any network errors
    showPopup("[loadLog] error fetching data.");
  });
  App.serverCmdData = null;
}

// save file API POST call
function savePOST(file, data) {
  const url = location.protocol + "//" + location.hostname + "/api/write?file=" + file;
  // submit request
  const xhr = new XMLHttpRequest();
  xhr.open("POST", url);
  xhr.setRequestHeader("Content-Type", "text/plain");
  xhr.onreadystatechange = function() {
    if (xhr.readyState === XMLHttpRequest.DONE) {
      if (xhr.status == 200) {
        // action on successful transmit
        afterPOSTActions(file);
      } else {
        showPopup("[savePOST] failed to send: " + file);
      }
    }
  }
  xhr.send(data);
}

//// Dynamic Menus ////

function showDynFileMenu(menu) {
  let _elem = document.getElementById(menu);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    removeDropdowns();
     _elem.style.display = 'block';
    // build URL / append data
    const url = location.protocol + "//" +location.hostname + "/api/read?file=" + menu;
    loadMenu(url).then((data) => { // wait for response
      if (menu === 'bookmarks') {
        showBookmarkSearch();
      }
      drawMenu(data, menu);
    });  
  }
}

function showDynAPIMenu(menu) {
  let _elem = document.getElementById(menu);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    removeDropdowns();
     _elem.style.display = 'block';
    // build URL / append data
    const url = location.protocol + "//"+ location.hostname + "/api?var=&arg=" + menu + "&action=main";
    loadMenu(url).then((data) => { // wait for response
      drawMenu(data, menu);
    });  
  }
}

async function loadMenu(url) {
  try {
    const response = await fetch(url, {
      method: "GET"
    });
    // return response
    return await response.text();
  } catch (error) {
    showPopup("[loadMenu] failed to fetch: " + error);
  }
}

// draws each menu item
window.drawMenu = function(data, menu) {
  // remove any current dynamic menus
  removeDynMenus();
  if (data && typeof data === 'string') {
    // erase global data
    while (App.fileData.length) App.fileData.pop();
    const splitdat = splitMenuData(data);
    for (let row = 0; row < splitdat.length; row++) {
      const line = splitdat[row];
      if (line != "") {
        const linearr = line.split("|");
        if (linearr.length < 3) { continue; }
        App.fileData.push(line); // write to array
        const navElement = document.getElementById(menu);
        // 0=Host, 1=Type, 2=Name
        const col0 = linearr[0];
        const col1 = linearr[1];
        const col2 = linearr[2].trim();
        // draw menu item
        navElement.appendChild(createListItem(col0, col1, col2, row));
        App.dynMenuActive = 1;
      }
    }
    // store menu name at end of array
    App.fileData.push(menu);
  } else {
    showPopup("[drawMenu] no data to parse.");
  }
}

function createListItem(col0_in, col1, col2, menuid) {
  let _elm = document.createElement('a');
  let _dot = document.createElement('span');
  let field = col0_in.split("~");
  let col0 = field[0];
  let host = field[1];
  let cmd = field[2];
  // assign menu ID
  _elm.id = "menu-" + menuid;
  // set menu text
  _elm.innerText = col2;
  // configure menu item
  switch (col1) {
    case 'ledmenu': 
      // LEDs menu
      const effect_cmd = col0;
      _elm.addEventListener("click", function() {
        if (effect_cmd == 'textscroll') {
          openTextScroll();
        } else {
          sendCmd('leds', effect_cmd);
        }
      });
      break;
    case 'room':
      // room selector menu
      _elm.addEventListener("click", function() {
        selectRoom(col0);
        refreshRoomState();
      });
      break;
    case 'relaxmenu': 
    case 'relaxactive': 
      // relax menu
      _elm.addEventListener("click", function() {
        sendCmd('main', 'relax', col0);
      });
      if (col1 == 'relaxactive') {
        let relaxplay = document.createElement('i');
        relaxplay.classList.add('fad');
        relaxplay.classList.add('fa-play');
        _elm.appendChild(relaxplay);
      }
      break;
    case 'relaxstop': 
      // relax menu
      _elm.addEventListener("click", function() {
        sendCmd('main', 'relaxstop');
      });
      let relaxstop = document.createElement('i');
      relaxstop.classList.add('fad');
      relaxstop.classList.add('fa-stop');
      _elm.appendChild(relaxstop);
      break;
    case 'link': 
      // URL menu
      _elm.href = col0;
      break;
    case 'thm': 
      // theme menu 
      _elm.classList.add('theme-colorbox');
      _elm.style.setProperty('background-color', col0);
      _elm.addEventListener("click", function() {
        setTheme(col0);
      });
      break;
    case 'slider':
      // horizontal slider menu
      const maxRange = 255;
      const slider = document.createElement('input');
      slider.classList.add("slider-wide");
      slider.type = 'range';
      slider.min = 0;
      slider.max = maxRange;
      slider.value = col0;
      slider.step = 1;
      slider.addEventListener('input', (event) => {
        const value = event.target.value;
        throttledSliderSend('main', host, `${cmd}_set_${value}`);
      });
      _elm.appendChild(slider);
      _elm.classList.add('slider-wide_container');
      _elm.classList.add('no-hover');
      break;
    case 'bkmrk': 
      // bookmarks menu 
      _elm.classList.add('bookmarked__item');
      Object.defineProperty(_elm, "url", {
        enumerable: false,
        writable: true,
        value: col0
      });
      _elm.addEventListener("click", function() {
        clickBookmark(menuid);
      });
      break;
    case 'subs':
      // subwoofer volume toggle
      const submode = subwooferVolumeEnabled();
      if (submode === true) {
        _elm.classList.add(SUBWOOFER_CLASS);
        _elm.addEventListener("click", function() {
          subwooferVolumeMode(false);
        });
      } else {
        _elm.classList.remove(SUBWOOFER_CLASS);
        _elm.addEventListener("click", function() {
          subwooferVolumeMode(true);
        });
      }
      break;
    case 'toggle':
      // switch-like toggle menu
      _elm.classList.add('no-hover');
      const tgl_state = col0 === '1' ? true : false;
      const tgl_obj = new ToggleSwitch(_elm, {
        id: `${host}-${cmd}`,
        checked: tgl_state,
        cmdHost: host,
        command: cmd,
        onToggle: toggleSwitchCallback
      });
      tgl_obj.activate();
      App.activeSwitches.push(tgl_obj);
      break;
    case 'noind': 
      // no indicator type status menu
      addStatusListener(_elm, col0, host, cmd);
      break;
    case 'blkind': 
      // black indicator type status menu
      _dot.classList.add('ind_dot');
      _dot.id = "ind-" + col2;
      addStatusListener(_elm, col0, host, cmd);
      _elm.appendChild(_dot);
      break;
    case 'grnind': 
      // green indicator type status menu
      _dot.classList.add('ind_dot');
      _dot.classList.add('ind_dot_green');
      _dot.id = "ind-" + col2;
      addStatusListener(_elm, col0, host, cmd);
      _elm.appendChild(_dot);
      break;
    case 'redind': 
      // red indicator type status menu
      _dot.classList.add('ind_dot');
      _dot.classList.add('ind_dot_red');
      _dot.id = "ind-" + col2;
      addStatusListener(_elm, col0, host, cmd);
      _elm.appendChild(_dot);
      break;
    case 'ylwind': 
      // yellow indicator type status menu
      _dot.classList.add('ind_dot');
      _dot.classList.add('ind_dot_yellow');
      _dot.id = "ind-" + col2;
      addStatusListener(_elm, col0, host, cmd);
      _elm.appendChild(_dot);
      break;
  }
  return _elm;
}

function addStatusListener(elm, col0, host, cmd) {
  if (col0 == 'cmd') { // generic remote API call on click
    elm.addEventListener("click", function() {
      sendCmd('main', host, cmd);
    });
  }
}

const throttledSliderSend = requestThrottle((host, cmd, val) => {
  sendCmd(host, cmd, val);
}, 50); // in (ms)

function removeDynMenus() {
  // only if menu is on-screen 
  if (App.dynMenuActive == 1) {
    // remove dynamic menu elements (II)
    for (let idx = 0; idx <= App.fileData.length; idx++) {
      const menuRemove = document.getElementById('menu-' + idx);
      if (menuRemove) menuRemove.remove();
    }
    App.dynMenuActive = 0;
  }
}

function openColorPrompt(command, argument, cmd_type = '') {
  const colorPrompt = new ColorPrompt('color-popup', (strippedHex) => {
    let cmd;
    let arg1;
    let arg2;
    // 3-argument command type
    if (cmd_type === '3args') {
      cmd = command;
      arg1 = argument;
      arg2 = strippedHex;
    } else { // 2-argument command type 
      cmd = 'main';
      arg1 = command;
      arg2 = `${argument}_${strippedHex}`;
    }
    sendCmd(cmd, arg1, arg2)
      .catch(err => {
        showPopup("[ColorPrompt] failed to send color: " + err);
      });
  });
  colorPrompt.show();
  colorPrompt.updateStateFromHex('#000000', true, false);
}

//// End Dynamic Menus ////

// show camera form window
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
  document.getElementById("logForm").style.display = "none";
  document.getElementById("cam-popup").style.display = "none";
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
  // timers
  if (App.activityTimeout) clearTimeout(App.activityTimeout);
  if (App.messagePopupTimer) clearTimeout(App.messagePopupTimer);
  if (App.volPopupTimer) clearTimeout(App.volPopupTimer);
  // toggle switches
  if (App.activeSwitches && App.activeSwitches.length > 0) {
    App.activeSwitches.forEach(toggle => {
      if (typeof toggle.destroy === 'function') toggle.destroy();
    });
    App.activeSwitches = [];
  }
  destroyPageServices();
};

function destroyPageServices() {
  // room status display
  if (App.roomStateManager) {
    App.roomStateManager.stopRoomStateTimer();
    App.roomStateManager = null;
  }
  // animated background
  if (App.starBackground) {
    App.starBackground.destroy();
    App.starBackground = null;
  }
  // weather widget
  if (window.weatherWidget && typeof window.weatherWidget.remove === 'function') {
    window.weatherWidget.remove();
    window.weatherWidget = null;
  }
}
