// Automate Website - JavaScript Frontend
// by Ben Provenzano III

import { requestThrottle, setTheme, classDisplay, checkElemIsVisibleByID,
  removeWindow, cleanString, base64URLSafeEncode, isNumeric, mapNumber,
  hsvToRgb, rgbToHsv, hexToRgb, rgbToHex, splitByDelimiter, sendCmd } from './utils.js';
import ToggleSwitch, { toggleSwitchCallback } from './switch.js';
import AnimatedBackground from './stars.js';
import WeatherWidget from './weather.js';
import { deviceType } from './device.js';
import { SSIDpwd } from './wifi.js';

window.App = {
  MAIN_SITE: "Automate",
  BKM_INACTIVE: 0,
  BKM_OPEN_MODE: 1,
  BKM_EDIT_MODE: 2,
  REFRESH_INTERVAL: 5,
  TIMEOUT_INTERVAL: 45,
  starBackground: null,
  activeSwitches: [],
  fileData: [],
  ctlCommand: null,
  serverCmdData: null,
  messagePopupTimer: null,
  dynMenuActive: 0,
  bookmarkState: 0,
  volPopupTimer: null,
  sysModel: null,
  activityTimeout: null,
  roomStateData: {
    timerID: null,
    isRunning: false,
    roomState: null,
    devices: []
  },
  ledScrollData: [
    '00FF00', '8x8', '12.0'
  ],
  PRESET_COLORS: [
    '#FF0000', '#FF7F00', '#FFFF00', '#00FF00', '#00FFFF',
    '#0000FF', '#7F00FF', '#FF00FF', '#FFFFFF', '#000000'
  ],
  currentHSV: { h: 0, s: 1, v: 1 },
  pendingQueuedColor: null,
  isColorPromptInit: false,
  isColorCommandPending: false,
  isColorDragMap: false,
  colorPromptParser: document.createElement('div')
};

function setupEventListeners() {
  const listeners = {
    // Top Nav
    'btn-dyn-mainmenu': () => showDynMenu('mainmenu'),
    'btn-menu-pimainmenu': () => showMenu('pi-mainmenu'),
    'btn-dyn-thememenu': () => showDynMenu('thememenu'),
    'btn-show-bookmarks': () => showBookmarks(),
    'btn-edit-bookmark': () => editBookmark(),
    'btn-add-bookmark': () => addBookmark(),
    'statusDropdownContainer': () => roomStateClick(),
    'nav_stats_menu': () => showStatusMenu(),
    // Menus
    'btn-about-main': () => aboutPrompt(),
    'btn-about-pi': () => aboutPrompt(),
    'btn-log-pi': () => openLogWindow(),
    'btn-home-pi': () => GoToHomePage(),
    // Main Grid Left
    'btn-dyn-ledcolormenu': () => showDynMenu('ledcolormenu'),
    'btn-color-prompt': () => colorPrompt(),
    'btn-dyn-ledeffectmenu': () => showDynMenu('ledeffectmenu'),
    'btn-power-fc_leds': () => showPowerMenu('fc_leds'),
    'btn-cmd-leds-shuffle': () => sendCmd('leds', 'shuffle', ''),
    'btn-cmd-leds-stop': () => sendCmd('leds', 'stop', ''),
    'btn-cmd-leds-pause': () => sendCmd('leds', 'pause', ''),
    'btn-cmd-leds-randcolor': () => sendCmd('leds', 'randcolor', ''),
    'btn-show-temphumid': () => showTempHumidity(),
    'btn-menu-lightmenu': () => showMenu('lightmenu'),
    'btn-cmd-lightson': () => sendCmd('main', 'lightson', ''),
    'btn-cmd-lightsoff': () => sendCmd('main', 'lightsoff', ''),
    'btn-menu-allmenu': () => showMenu('allmenu'),
    'btn-room-on': () => roomOnOff('on'),
    'btn-room-off': () => roomOnOff('off'),
    'btn-open-cam':() => openCamWindow(),
    'btn-open-log-svr': () => openLogWindow(),
    // Main Grid Right (Living Room)
    'btn-power-wkststate': () => showPowerMenu('lrxmit', 'wkststate'),
    'btn-menu-lrmenu': () => showMenu('lr-menu'),
    'btn-ctls-br': () => ctlsMenu('br'),
    'btn-power-wl1_dual': () => showPowerMenu('lrxmit', 'wl1_dual'),
    'voldwnbtn': () => sendVol('dwn'),
    'volmutebtn': () => sendVol('mute'),
    'volupbtn': () => sendVol('up'),
    'btn-vol-dwnc': () => sendVol('dwnc'),
    'btn-power-hifistate': () => showPowerMenu('lrxmit', 'hifistate'),
    'btn-vol-upc': () => sendVol('upc'),
    'btn-vol-display': () => sendVol('display'),
    'btn-show-hifi-amp': () => showHiFiAmpInput(),
    'btn-show-hifi-more': () => showHiFiMoreMenu(),
    // Bedroom Grid
    'btn-dyn-relaxmenu': () => showDynMenu('relaxmenu'),
    'btn-cmd-stop-br': () => sendCmd('main', 'stop-br', ''),
    'btn-menu-brmenu': () => showMenu('br-menu'),
    'btn-ctls-lr': () => ctlsMenu('lr'),
    'btn-power-brlamp1': () => showPowerMenu('brxmit', 'brlamp1'),
    'br-voldwnbtn': () => sendVol('dwn'),
    'br-volmutebtn': () => sendVol('mute'),
    'br-volupbtn': () => sendVol('up'),
    'btn-power-brtv': () => showPowerMenu('brxmit', 'brtv'),
    'btn-power-ampstate': () => showPowerMenu('brpi', 'ampstate'),
    'btn-power-brpc': () => showPowerMenu('brxmit', 'brpc'),
    'btn-power-brmacs': () => showPowerMenu('brxmit', 'brmacs'),
    'btn-show-br-amp': () => showBedroomAmpInput(),
    'btn-power-retropi': () => showPowerMenu('brxmit', 'retropi'),
    // Server Options
    'btn-menu-legacy': () => showMenu('legacy-menu'),
    'btn-svr-startlegacy': () => serverAction('startlegacy'),
    'btn-svr-stoplegacy': () => serverAction('stoplegacy'),
    'btn-menu-xana': () => showMenu('xana-menu'),
    'btn-svr-startxana': () => serverAction('startxana'),
    'btn-svr-stopxana': () => serverAction('stopxana'),
    'btn-svr-restorexana': () => serverAction('restorexana'),
    'btn-menu-unifi': () => showMenu('unifi-menu'),
    'btn-ext-unifi': () => GoToExtPage('unifi:11443/network/ff7gqlig/dashboard'),
    'btn-svr-startunifi': () => serverAction('startunifi'),
    'btn-svr-stopunifi': () => serverAction('stopunifi'),
    'btn-menu-svr': () => showMenu('svr-menu'),
    'btn-ext-pve': () => GoToExtPage('pve:8006'),
    'btn-svr-syslog': () => serverAction('syslog'),
    'btn-menu-files': () => showMenu('files-menu'),
    'btn-open-backup': () => openBackupOptions(),
    'btn-open-regions': () => openRegionsOptions(),
    'btn-menu-netwrk': () => showMenu('netwrk-menu'),
    'btn-wifi-prompt': () => wifiPrompt(),
    'btn-ext-edgeswitch': () => GoToExtPage('edgeswitch.home'),
    'btn-svr-netscan': () => serverAction('netscan'),
    'btn-close-svr-opt1': () => closeServerOptions(),
    'btn-svr-files-clearlog': () => serverAction('files-clearlog'),
    'btn-menu-router': () => showMenu('router-menu'),
    'btn-ext-router': () => GoToExtPage('router.home'),
    'btn-svr-rebootrouter': () => serverAction('rebootrouter'),
    // Backup
    'btn-svr-last_backup_dates': () => serverAction('files-last_backup_dates'),
    'btn-svr-backupchk': () => serverAction('files-backupchk'),
    'btn-svr-pve_vmsbkp': () => serverAction('pve_vmsbkp'),
    'btn-pwd-unlock': () => getPassword('pwd'),
    'btn-svr-backupstd': () => serverAction('files-backupstd'),
    'btn-svr-detach_bkps': () => serverAction('detach_bkps'),
    'btn-close-svr-opt2': () => closeServerOptions(),
    'btn-svr-pve_listsnaps': () => serverAction('pve_listsnaps'),
    'btn-open-svr-opt1': () => openServerOptions(),
    // Archive
    'btn-menu-archive': () => showMenu('archive-menu'),
    'btn-pwd-fusearch': () => getPassword('fusearch'),
    'btn-svr-unmnt_all_fuse': () => serverAction('files-unmnt_all_fuse'),
    'btn-menu-external': () => showMenu('external-menu'),
    'btn-svr-mnt_ext_region': () => serverAction('files-mnt_ext_region'),
    'btn-svr-unmnt_ext_region': () => serverAction('files-unmnt_ext_region'),
    'btn-menu-snapshots': () => showMenu('snapshots-menu'),
    'btn-svr-mnt_snap_region': () => serverAction('files-mnt_snap_region'),
    'btn-svr-unmnt_snap_region': () => serverAction('files-unmnt_snap_region'),
    'btn-menu-ramdisk': () => showMenu('ramdisk-menu'),
    'btn-svr-mnt_ram_region': () => serverAction('files-mnt_ram_region'),
    'btn-svr-unmnt_ram_region': () => serverAction('files-unmnt_ram_region'),
    'btn-svr-unmnt_all': () => serverAction('files-unmnt_all'),
    'btn-close-svr-opt3': () => closeServerOptions(),
    'btn-open-svr-opt2': () => openServerOptions(),
    // Pi Grid
    'btn-menu-piwifi': () => showMenu('piwifi-menu'),
    'btn-show-piwifiprompt': () => showPiWiFiPrompt(),
    'btn-svr-client': () => serverAction('client'),
    'btn-svr-apd': () => serverAction('apd'),
    'btn-svr-wifiscan': () => serverAction('wifiscan'),
    'btn-svr-showwpa': () => serverAction('showwpa'),
    'btn-svr-temp': () => serverAction('temp'),
    'btn-close-svr-opt4': () => closeServerOptions(),
    'btn-svr-clearlog': () => serverAction('clearlog'),
    'btn-svr-resetbt': () => serverAction('resetbt'),
    // Text Scroll
    'btn-menu-textscrollcolor': () => showMenu('textscrollcolor-menu'),
    'btn-tscolor-1': () => setTextScrollColor('00FF00'),
    'btn-tscolor-2': () => setTextScrollColor('FF0000'),
    'btn-tscolor-3': () => setTextScrollColor('0000FF'),
    'btn-tscolor-4': () => setTextScrollColor('FFA500'),
    'btn-tscolor-5': () => setTextScrollColor('A020F0'),
    'btn-tscolor-6': () => setTextScrollColor('FFCC00'),
    'btn-tscolor-7': () => setTextScrollColor('FFFFFF'),
    'btn-menu-textscrollfont': () => showMenu('textscrollfont-menu'),
    'btn-tsfont-8x8': () => setTextScrollFont('8x8'),
    'btn-tsfont-5x7': () => setTextScrollFont('5x7'),
    'btn-tsfont-3x5': () => setTextScrollFont('3x5'),
    'btn-menu-textscrollspeed': () => showMenu('textscrollspeed-menu'),
    'btn-tsspeed-slowest': () => setTextScrollSpeed('3.0'),
    'btn-tsspeed-slow': () => setTextScrollSpeed('9.0'),
    'btn-tsspeed-normal': () => setTextScrollSpeed('12.0'),
    'btn-tsspeed-fast': () => setTextScrollSpeed('15.0'),
    'btn-tsspeed-fastest': () => setTextScrollSpeed('20.0'),
    'btn-ts-close': () => closeTextScroll(),
    'btn-ts-clear': () => clearLEDsMessage(),
    'btn-ts-send': () => sendTextScroll(),
    // Popups
    'btn-log-close': () => closePopup(),
    'btn-log-open-svr': () => openServerOptions(),
    'btn-log-load': () => loadLog('sysout'),
    'sendButton': () => serverSend(),
    'btn-color-close': () => closeColorPrompt(),
    'btn-cam-close': () => closePopup(),
    'btn-cam-fullscreen': () => goToContextRoot('cam1')
  };
  document.addEventListener('click', (event) => {
    let current = event.target;
    while (current && current !== document) {
      if (current.id && listeners[current.id]) {
        listeners[current.id]();
        return;
      }
      current = current.parentNode;
    }
  });
}

// runs before exit
window.addEventListener('beforeunload', () => {
  App.destroyAll();
});

App.destroyAll = function() {
  console.log("Exiting...");
  // 1. Clear timers
  if (App.activityTimeout) clearTimeout(App.activityTimeout);
  if (App.messagePopupTimer) clearTimeout(App.messagePopupTimer);
  if (App.volPopupTimer) clearTimeout(App.volPopupTimer);
  if (App.roomStateData && App.roomStateData.timerID) {
    clearInterval(App.roomStateData.timerID);
  }
  // 2. Clean up toggle switches
  if (App.activeSwitches && App.activeSwitches.length > 0) {
    App.activeSwitches.forEach(toggle => {
      if (typeof toggle.destroy === 'function') toggle.destroy();
    });
    App.activeSwitches = [];
  }
  // 3. Clean up animated background
  if (typeof App.starBackground !== 'undefined' && App.starBackground) {
    App.starBackground.destroy();
  }
  // 4. Clean up the weather Widget
  if (window.weatherApp && typeof window.weatherApp.remove === 'function') {
    window.weatherApp.remove();
  }
};

// runs on page load
window.addEventListener("DOMContentLoaded", () => {
  window.addEventListener('click', handleClicks, false);
  setupEventListeners();
  loadPage();
});

// runs on-each mouse click
function handleClicks(event) {
  resetActivityTimer();
  // disable click events when in bookmark edit mode
  if (App.bookmarkState === App.BKM_EDIT_MODE) {
    if (event.target.className !== "editFav__win") {
      event.stopPropagation();
    }
    return;
  }
  // don't hide menus when clicking these elements
  const NO_HIDE_ELEMENTS = '.button, .topnav, .bookmarked__item, .chkbox, .toggle-switch';
  if (event.target.closest(NO_HIDE_ELEMENTS)) return;
  removeDropdowns();
}

function resetActivityTimer() {
  activityBegin();
  clearTimeout(App.activityTimeout);
  App.activityTimeout = setTimeout(() => {
    activityStop();
  }, App.TIMEOUT_INTERVAL * 1000);
}

function activityBegin() {
  if (typeof App.starBackground !== 'undefined' && App.starBackground) {
    App.starBackground.resume();
  }
  if (App.sysModel === App.MAIN_SITE) {
    showRoomState();
  }
}

function activityStop() {
  if (typeof App.starBackground !== 'undefined' && App.starBackground) { 
    App.starBackground.pause();
  }
  if (App.sysModel === App.MAIN_SITE) {
    hideRoomState();
  }
}

window.removeDropdowns = function() {
  classDisplay("dd-content", "none");
  removeDynMenus();
  if (App.sysModel === App.MAIN_SITE) {
    removeBookmarks();
    if (App.activeSwitches && App.activeSwitches.length > 0) {
      App.activeSwitches.forEach(toggle => {
        if (typeof toggle.destroy === 'function') toggle.destroy();
      });
      App.activeSwitches = [];
    }
    if (window.weatherApp && typeof window.weatherApp.closeMenu === 'function') {
      window.weatherApp.closeMenu();
    }
  }
}

function removeBookmarks() {
  const elem = document.getElementById("bookmarks");
  if (elem) elem.classList.remove("bookmark-editmode");
  classDisplay("bookmark-buttons", "none");
  removeBookmarkSearch();
  closeBookmarkPrompt();
  App.bookmarkState = App.BKM_INACTIVE;
}

function loadPage() {
  // read device type
  App.sysModel = deviceType(); 
  if (App.sysModel === App.MAIN_SITE) {
    // load control menu saved state
    let _mode = localStorage.getItem("ctls-mode")
    if (_mode === null || _mode === undefined || _mode === "") { 
      _mode = 'lr'; 
    } // living room
    ctlsMenu(_mode);
    // weather title-bar icon
    window.weatherWidget = new WeatherWidget();
    window.weatherWidget.activate();
    // server home page
    classDisplay('server-grid', 'block');
  } else if (App.sysModel === 'Pi') {
    classDisplay('pi-grid', 'block');
  } else if (App.sysModel === 'LEDpi') {
    classDisplay('ledpi-grid', 'block');
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
  // initialize background 
  App.starBackground = new AnimatedBackground('star-canvas');
  App.starBackground.init();
  // activate timed services
  resetActivityTimer();
}

// open URL in new tab
function GoToExtPage(_path) {
  let url = "https://" + _path;   
  window.open(url, "_blank");
}

// back to home page 
function GoToHomePage() {
  if (App.sysModel === App.MAIN_SITE) {
    hidePages();
    loadPage();
  } else {
    window.location = 'https://' + App.MAIN_SITE + '.home';   
  }
}

function hidePages() {
  classDisplay('pi-grid', 'none'); 
  classDisplay('ledpi-grid', 'none'); 
  classDisplay('server-grid', 'none');
}

function showPopup(message) {
  let msgtext;
  let win_name = "msg-popup";
  let text_name = "msg-text";
  let msgpopwin = document.getElementById(win_name);
  if (!msgpopwin) { // window does not exist
    // draw window
    msgpopwin = document.createElement("div");
    msgpopwin.classList.add("popup-alert");
    msgpopwin.id = win_name;
    // volume text box
    msgtext = document.createElement("div");
    msgtext.id = text_name;
    msgpopwin.appendChild(msgtext);
    // add to page
    document.body.appendChild(msgpopwin);
  } else {
    // select elements in existing window
    msgtext = document.getElementById(text_name);
  }
  // set error text
  msgtext.innerHTML = message;
  // reset hide window timeout
  clearTimeout(App.messagePopupTimer);
  // hide window after delay
  App.messagePopupTimer = setTimeout(() => {
    if (msgpopwin) { // window exists
      // fade out window
      msgpopwin.classList.add('fade-out');
      // wait for the transition to end before removing window
      msgpopwin.addEventListener("animationend", function (event) {
        if (event && msgpopwin) {
          document.body.removeChild(msgpopwin);
        }
      });
    }
  }, 4000); // hide delay in (ms)
}

async function showRoomState() {
  if (App.roomStateData.isRunning === true) return;
  App.roomStateData.isRunning = true;
  let lightIcon = document.getElementById("trafficLightIcon");
  let container = document.getElementById('statusDropdownContainer');
  container.classList.remove('status-loading');
  let room = null; let cmd = null;
  if (App.ctlCommand == 'lr' || App.ctlCommand == 'subs') {
    room = "living room"; 
    cmd = 'lrstatus';
  }
  if (App.ctlCommand == 'br') {
    room = "bedroom"; 
    cmd = 'brstatus';
  }
  if (cmd === null) {
    hideRoomState();
    return;
  }
  lightIcon.classList.add("status-pending");
  return sendCmd('main', cmd, '').then((resp) => {
    App.roomStateData.devices = [];
    App.roomStateData.roomState = null;
    lightIcon.classList.remove("status-error");
    lightIcon.classList.remove("status-pending");
    if (resp === null) {
      lightIcon.classList.add("status-error");
      hideRoomState();
      return;
    } else {
      App.roomStateData.roomState = resp.roomState;
      App.roomStateData.devices = resp.devices;
      if (App.roomStateData.roomState === true) {
        lightIcon.classList.remove("status-off");
        lightIcon.classList.add("status-on");
      } else if (App.roomStateData.roomState === false) { 
        lightIcon.classList.remove("status-on");
        lightIcon.classList.add("status-off");
      }
    }
    container.style.setProperty('--spin-duration', `${App.REFRESH_INTERVAL}s`);    
    void container.offsetWidth; 
    requestAnimationFrame(() => {
      container.classList.add('status-loading');
      App.roomStateData.timerID = setTimeout(() => {
        App.roomStateData.isRunning = false;
        showRoomState(App.REFRESH_INTERVAL);
      }, App.REFRESH_INTERVAL * 1000);
    });
  });
}

async function roomStateClick() {
  const menu = 'roomstatus';
  let _elem = document.getElementById(menu);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
    return;
  }
  removeDropdowns();
  stopRoomStateTimer();
  await showRoomState();
  let menuData = '';
  if (App.roomStateData.roomState === null) {
    menuData += `${menu}|noind|Error checking devices.\n`;
  } else {
    for (const device of App.roomStateData.devices) {
      const name = device.name;
      const status = device.status;
      let ind;
      if (status === false) {
        ind = 'blkind';
      } else if (status === true) {
        ind = 'grnind';
      } else {
        ind = 'redind';
      }
      menuData += `${menu}|${ind}|${name}\n`;
    }
  }
  drawMenu(menuData, menu);
  _elem.style.display = 'block';
}

function hideRoomState() {
  stopRoomStateTimer();
  let lightIcon = document.getElementById("trafficLightIcon");
  let container = document.getElementById('statusDropdownContainer');
  container.classList.remove('status-loading');
  lightIcon.classList.remove("status-pending");
  lightIcon.classList.remove("status-off");
  lightIcon.classList.remove("status-on");
}

function stopRoomStateTimer() {
  if (App.roomStateData.isRunning === false) 
    return;
  clearTimeout(App.roomStateData.timerID);
  App.roomStateData.isRunning = false;
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
  let _class = "alert_btn";
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

function openServerOptions(){
  closeServerOptions();
  classDisplay("svropt__prompt", "block");
  classDisplay("svropt__main", "block");
}

function openRegionsOptions(){
  closeServerOptions();
  classDisplay("svropt__prompt", "block");
  classDisplay("svropt__regions", "block");
}

function openBackupOptions(){
  closeServerOptions();
  classDisplay("svropt__prompt", "block");
  classDisplay("svropt__backup", "block");
}

function closeServerOptions(){
  removeDropdowns();
  clearPendingCmd();
  classDisplay("svropt__prompt", "none");
  classDisplay("svropt__main", "none");
  classDisplay("svropt__regions", "none");
  classDisplay("svropt__backup", "none");
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
  sendCmd('leds', 'stop', '');
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

/////////////////////////

async function aboutPrompt(){
  const _winid = 'about__prompt';
  // only allow one-instance of the window
  if (document.getElementById(_winid)) {
    return;
  }
  let aboutprompt = document.createElement("div"); 
  aboutprompt.id = _winid;
  aboutprompt.classList.add("prompt_win");
  // title
  let abouttext = document.createElement("div");
  abouttext.className = "about__title";
  abouttext.innerHTML = App.sysModel;
  aboutprompt.appendChild(abouttext); 
  // logo
  let img = document.createElement("img");
  img.src = "img/automate.png";
  img.id = "about__img";
  aboutprompt.appendChild(img);
  // version details
  let aboutdets2 = document.createElement("div"); 
  aboutdets2.innerHTML = "by Ben Provenzano III";
  aboutdets2.className = "about__text";
  aboutprompt.appendChild(aboutdets2);
  let aboutdets1 = document.createElement("div");
  const curDate = new Date();
  const curYear = curDate.getFullYear();
  const curMonth = curDate.getMonth() + 1;
  const curQtr = Math.ceil(curMonth / 3); // round up to the nearest integer
  aboutdets1.innerHTML = curYear + '.' + curQtr + " Release";
  aboutdets1.className = "about__text";
  aboutprompt.appendChild(aboutdets1);
  // cancel button
  let aboutcancelb = document.createElement("button");
  aboutcancelb.type = "button"; 
  aboutcancelb.classList.add("button");
  aboutcancelb.innerHTML = "Close";
  aboutcancelb.id = "about__btn";
  aboutprompt.appendChild(aboutcancelb); //append cancel-button
  document.body.appendChild(aboutprompt); //append the password-prompt so it gets visible
  new Promise(function() {
    aboutprompt.addEventListener('click', function handleButtonClicks(btn_evnt) { //lets handle the buttons
      if (btn_evnt.target.tagName !== 'BUTTON') { return; } //nothing to do - user clicked somewhere else
      aboutprompt.removeEventListener('click', handleButtonClicks); //removes eventhandler on cancel or ok
      document.body.removeChild(aboutprompt);  //as we are done clean up by removing the password-prompt
    });
  });   
}

/// Temperature & Humidity ///

async function showTempHumidity(){
  const _winid = 'temp__prompt';
  // only allow one-instance of the window
  if (document.getElementById(_winid)) {
    return;
  }
  // create window
  let tempprompt = document.createElement("div"); 
  tempprompt.id = _winid;
  tempprompt.classList.add("prompt_win");
  // top window text
  let temptext = document.createElement("div");
  temptext.innerHTML = 'Temperature/Humidity';
  temptext.id = 'temp_top_text';
  tempprompt.appendChild(temptext);
  // thermometer container
  let tempcon = document.createElement("div");
  tempcon.id = 'temp_therm_grid';
  // temperature thermometer 
  let tmeter = document.createElement("div"); 
  tmeter.className = "thermometer";
  let tdisplay = document.createElement("div"); 
  tdisplay.className = "temperature";
  tdisplay.id = "thermo__1";
  tdisplay.dataset.value = "--°";
  tmeter.appendChild(tdisplay); 
  tempcon.appendChild(tmeter); 
  // humidity thermometer 
  let hmeter = document.createElement("div"); 
  hmeter.className = "thermometer";
  let hdisplay = document.createElement("div"); 
  hdisplay.className = "temperature";
  hdisplay.id = "thermo__2";
  hdisplay.dataset.value = "--%";
  hmeter.appendChild(hdisplay);
  tempcon.appendChild(hmeter); 
  // add thermo container to window
  tempprompt.appendChild(tempcon);
  // buttons container
  let btnscon = document.createElement("div");
  // cancel button
  let tempcancelb = document.createElement("button");
  tempcancelb.classList.add("button");
  tempcancelb.classList.add("temp__btn");
  tempcancelb.classList.add("fas");
  tempcancelb.classList.add("fa-times");
  tempcancelb.type = "button";
  btnscon.appendChild(tempcancelb);
  // refresh button
  let temprefreshb = document.createElement("button");
  temprefreshb.classList.add("button");
  temprefreshb.classList.add("temp__btn");
  temprefreshb.classList.add("fad");
  temprefreshb.classList.add("fa-sync");
  temprefreshb.type = "button";
  btnscon.appendChild(temprefreshb);
  // add buttons container to window
  tempprompt.appendChild(btnscon);
  // add window to DOM
  document.body.appendChild(tempprompt);
  // call API for data
  getTemperatureData();
  // button actions
  new Promise(function() {
    tempprompt.addEventListener('click', function handleButtonClicks(btn_evnt) {
      if (btn_evnt.target.tagName !== 'BUTTON') { return; } 
        // close button
        if (btn_evnt.target === tempcancelb) {
          tempprompt.removeEventListener('click', handleButtonClicks); 
          document.body.removeChild(tempprompt);  
        }
        // refresh button
        if (btn_evnt.target === temprefreshb) {
          getTemperatureData();
        }
    });
  });
}

function getTemperatureData() {
  sendCmd('main', 'brxmit', 'roomth').then((data) => {
    const resp = cleanString(data);
    // extract numerics
    const resp_arr = resp.split("~");
    let temp_elm = document.getElementById("thermo__1");
    let humd_elm = document.getElementById("thermo__2");
    if (resp_arr.length == 2) {
      // valid response
      pushTempDataToThermos(resp_arr, temp_elm, humd_elm);
    } else {
      pushTempErrorThermos(temp_elm, humd_elm);
    }
  });
}

function pushTempDataToThermos(resp_arr, temp_elm, humd_elm) {
  // thermometer limits
  const minTemp = 25;
  const maxTemp = 100;
  const minHumidity = 5;
  const maxHumitidy = 100;
  // verify elements exist
  if (!(temp_elm && humd_elm)) {
    return;
  }
  // validate response
  if (resp_arr.length == 2) {
    let tvalue = resp_arr[0];
    let hvalue = resp_arr[1];
    // set temperature
    temp_elm.dataset.value = tvalue + "°F";
    if (tvalue >= maxTemp) { tvalue = maxTemp; }
    if (tvalue <= minTemp) { tvalue = minTemp; }
    temp_elm.style.height = (tvalue - minTemp) / (maxTemp - minTemp) * 100 + "%";
    // set humidity
    humd_elm.dataset.value = hvalue + "%";
    if (hvalue >= maxHumitidy) { hvalue = maxHumitidy; }
    if (hvalue <= minHumidity) { hvalue = minHumidity; }
    humd_elm.style.height = (hvalue - minHumidity) / (maxHumitidy - minHumidity) * 100 + "%";
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

/// END - Temperature & Humidity ///

async function showPiWiFiPrompt() {
  const winid = "pinet__prompt";
  // only allow one-instance of the window
  if (document.getElementById(winid)) {
    return;
  }
  removeDropdowns();
  let result = await piWiFiPrompt(winid);
  if (result !== null) {  
    if (result !== '') {  
      sendCmd('main','confwpa',result);
      document.getElementById("logTextBox").value = "Wi-Fi configuration updated, select client mode to apply changes.";
    }
  }
}

async function piWiFiPrompt(_winid){
  let pinetprompt = document.createElement("div"); 
  pinetprompt.id = _winid;
  pinetprompt.classList.add("prompt_win");
  // SSID text
  let pinettext1 = document.createElement("div"); 
  pinettext1.innerHTML = "Network (SSID):"; 
  pinetprompt.appendChild(pinettext1); 
  // SSID box 
  let pinetssidbox = document.createElement("input"); 
  pinetssidbox.id = "pinet__ssidbox"; 
  pinetprompt.appendChild(pinetssidbox); 
  // password text
  let pinettext2 = document.createElement("div"); 
  pinettext2.innerHTML = "Password:"; 
  pinetprompt.appendChild(pinettext2);   
  // password box
  let pinetpassbox = document.createElement("input"); 
  pinetpassbox.id = "pinet__passbox"; 
  pinetpassbox.type="password";
  pinetprompt.appendChild(pinetpassbox); 
  // save button
  let pinetokbutton = document.createElement("button"); 
  pinetokbutton.innerHTML = "Save";
  pinetokbutton.className ="button pinet__btn"; 
  pinetokbutton.type="button"; 
  // cancel button
  let pinetcancelb = document.createElement("button"); 
  pinetcancelb.innerHTML = "Cancel";
  pinetcancelb.className ="button pinet__btn"; 
  pinetcancelb.type="button"; 
  // create window
  pinetprompt.appendChild(pinetcancelb); 
  pinetprompt.appendChild(pinetokbutton); 
  document.body.appendChild(pinetprompt); 
  // focus on SSID text box
  pinetssidbox.focus();
  // control logic
  return new Promise(function(resolve) {
  	function cancelWiFi(){
  		pinetssidbox.value = "";
  		pinetpassbox.value = "";
        document.body.removeChild(pinetprompt);
    }
  	function sendWiFiData(){
	    if (pinetssidbox.value === "") {
	      pinettext1.innerHTML = "enter SSID!";
	    } else {
	      if (pinetpassbox.value === "") {
	        pinettext1.innerHTML = "enter password!";
	      } else {
	      	// concat and convert to base64
          let _wifidata = base64URLSafeEncode(pinetssidbox.value + "|$|" + pinetpassbox.value);
	        resolve(_wifidata);
	        cancelWiFi();
	      }
	    }
    }
    // button clicks
    pinetprompt.addEventListener('click', function handleButtonClicks(e) { 
      if (e.target.tagName !== 'BUTTON') return; 
        if (e.target === pinetokbutton) {
          sendWiFiData();
        }
        if (e.target === pinetcancelb) {
          cancelWiFi();
        }        
    });
    // key focused on SSID box
    pinetssidbox.addEventListener('keyup', function handleSSID(ssid_evnt){ 
        if(ssid_evnt.key === 'Enter') {
          pinetpassbox.focus();
        } else if (ssid_evnt.key === 'Escape') { 
          cancelWiFi();
        }
    });
    // key focused on password box
    pinetpassbox.addEventListener('keyup', function handlePass(pass_evnt){ 
        if(pass_evnt.key === 'Enter') { 
          sendWiFiData();
        } else if (pass_evnt.key === 'Escape') {
          cancelWiFi();
        }
    });
  }); 
}

async function wifiPrompt(){
  const _winid = "wifi__prompt";
  // only allow one-instance of the window
  if (document.getElementById(_winid)) return;
  let wifiprompt = document.createElement("div");
  wifiprompt.id = _winid;
  wifiprompt.classList.add("prompt_win");
  let wifitext = document.createElement("div");
  wifitext.innerHTML = "Scan for WiFi Access"; 
  wifiprompt.appendChild(wifitext);
  let img = document.createElement("img");
  img.src = "img/wifi.png";
  img.id = "wifi__img";
  wifiprompt.appendChild(img);
  let wifipwd = document.createElement("div");
  wifipwd.id = "wifi__pwdtxt";
  wifipwd.innerHTML = atob(SSIDpwd());
  wifiprompt.appendChild(wifipwd);
  let wificancelb = document.createElement("button");
  wificancelb.innerHTML = "Close";
  wificancelb.className = "button"; 
  wificancelb.type = "button"; 
  wificancelb.id = "wifipmt__btn";
  wifiprompt.appendChild(wificancelb);
  document.body.appendChild(wifiprompt);
  new Promise(function() {
    wifiprompt.addEventListener('click', function handleButtonClicks(btn_evnt) {
      if (btn_evnt.target.tagName !== 'BUTTON') { return; }
        wifiprompt.removeEventListener('click', handleButtonClicks);
        document.body.removeChild(wifiprompt);
    });
  });   
}

async function getPassword(_type){
  let result;
  try{
    removeDropdowns();
    result = await passwordPrompt();
    if (result !== null) {  
      if (result !== '') {
        savePOST(_type, [result]);
      }
    }
    result = "";
  } catch(e){
    result = "";
  }
}

function passwordPrompt(){
  const _winid = "pass__prompt_win";
  if (document.getElementById(_winid)) return;
  let pwprompt = document.createElement("div");
  pwprompt.id = _winid;
  pwprompt.classList.add("prompt_win");
  let pwtextdiv = document.createElement("div");
  pwtextdiv.innerHTML = "Enter password:";
  pwprompt.appendChild(pwtextdiv);
  let pwinput = document.createElement("input");
  pwinput.id = "pass__textbox";
  pwinput.type="password";
  pwprompt.appendChild(pwinput);
  // buttons 
  let pwbtndiv = document.createElement("div");
  pwbtndiv.id = "pass__prmbtnctr";
  let pwokbutton = document.createElement("button");
  pwokbutton.innerHTML = "Send";
  pwokbutton.className ="button pass__prompt_btn"; 
  pwokbutton.type="button"; 
  let pwcancelb = document.createElement("button");
  pwcancelb.innerHTML = "Cancel";
  pwcancelb.className = "button pass__prompt_btn"; 
  pwcancelb.type = "button"; 
  pwbtndiv.appendChild(pwcancelb);
  pwbtndiv.appendChild(pwokbutton);
  pwprompt.appendChild(pwbtndiv);
  document.body.appendChild(pwprompt); 
  pwinput.focus();
  return new Promise(function(resolve, reject) {
    pwprompt.addEventListener('click', function handleButtonClicks(click_evnt) {
      if (click_evnt.target.tagName !== 'BUTTON') return;
      pwprompt.removeEventListener('click', handleButtonClicks);
      if (click_evnt.target === pwokbutton) {
        resolve(pwinput.value);
      } else {
        reject(new Error('User cancelled'));
      }
      document.body.removeChild(pwprompt);
    });
    pwinput.addEventListener('keyup',function handleEnter(up_events) {
      if(up_events.key === 'Enter') {
        resolve(pwinput.value);
        document.body.removeChild(pwprompt);
      } else if (up_events.key === 'Escape') {
        document.body.removeChild(pwprompt);
        reject(new Error("User cancelled"));
      }
    });
  }); 
}

/// Color Prompt ///

function initColorGrid() {
  if (App.isColorPromptInit) return;
  const grid = document.getElementById('preset-color-grid');
  if (!grid) return;
  grid.innerHTML = ''; 
  App.PRESET_COLORS.forEach(color => {
    const swatch = document.createElement('div');
    swatch.className = 'color-swatch';
    swatch.style.backgroundColor = color;
    swatch.addEventListener('click', () => updateStateFromHex(color, true, true));
    grid.appendChild(swatch);
  });
  App.isColorPromptInit = true;
}

function colorPrompt(initialColor) {
  initColorGrid();
  let formattedColor = String(initialColor);
  if (!formattedColor.startsWith('#')) {
    formattedColor = '#' + formattedColor;
  }
  const pendingLock = App.isColorCommandPending;
  App.isColorCommandPending = true; 
  updateStateFromHex(formattedColor, true, false);
  App.isColorCommandPending = pendingLock; 
  document.getElementById('hex-color').addEventListener('input', handleHexInput);
  document.getElementById('slider-hue').addEventListener('input', handleHueInput);
  document.getElementById('color-map').addEventListener('pointerdown', startMapDrag);
  document.getElementById('color-popup').style.display = 'flex';
}

function closeColorPrompt() {
  document.getElementById('color-popup').style.display = 'none';
  document.getElementById('hex-color').removeEventListener('input', handleHexInput);
  document.getElementById('slider-hue').removeEventListener('input', handleHueInput);
  document.getElementById('color-map').removeEventListener('pointerdown', startMapDrag);
  stopMapDrag();
}

const throttledColorSend = requestThrottle((strippedHex) => {
  if (App.isColorCommandPending) return; 
  App.isColorCommandPending = true;
  sendCmd('leds', 'hex', strippedHex)
    .catch(err => {
      console.error("Failed to send color command:", err);
    })
    .finally(() => {
      App.isColorCommandPending = false;
    });
}, 50);

function dispatchColorCommand(hex) {
  let strippedHex = hex.replace('#', '');
  throttledColorSend(strippedHex);
}

function updateUIFromHSV(triggerSend = true) {
  let rgb = hsvToRgb(App.currentHSV.h, App.currentHSV.s, App.currentHSV.v);
  let hex = rgbToHex(rgb.r, rgb.g, rgb.b);
  document.getElementById('hex-color').value = hex;
  App.colorPromptParser.style.color = hex;
  document.querySelectorAll('.color-swatch').forEach(swatch => {
    if (swatch.style.backgroundColor === App.colorPromptParser.style.color) {
      swatch.classList.add('active-swatch');
    } else {
      swatch.classList.remove('active-swatch');
    }
  });
  if (triggerSend) dispatchColorCommand(hex);
}

function updateStateFromHex(hex, updateMapUI = true, triggerSend = true) {
  let rgb = hexToRgb(hex);
  App.currentHSV = rgbToHsv(rgb.r, rgb.g, rgb.b);
  if (updateMapUI) {
    document.getElementById('slider-hue').value = App.currentHSV.h;
    document.getElementById('color-map-bg').style.background = `hsl(${App.currentHSV.h}, 100%, 50%)`;
    document.getElementById('color-pointer').style.left = `${App.currentHSV.s * 100}%`;
    document.getElementById('color-pointer').style.top = `${(1 - App.currentHSV.v) * 100}%`;
  }
  document.getElementById('hex-color').value = hex.toUpperCase();
  updateUIFromHSV(triggerSend);
}

function handleHexInput(e) {
  let val = e.target.value;
  if (!val.startsWith('#')) val = '#' + val;
  if (/^#[0-9A-F]{6}$/i.test(val)) {
    updateStateFromHex(val, true, true);
  }
}

function handleHueInput(e) {
  App.currentHSV.h = parseFloat(e.target.value);
  document.getElementById('color-map-bg').style.background = `hsl(${App.currentHSV.h}, 100%, 50%)`;
  updateUIFromHSV(true);
}

// Map Dragging
function startMapDrag(e) {
  App.isColorDragMap = true;
  updateMapPointer(e);
  window.addEventListener('pointermove', updateMapPointer);
  window.addEventListener('pointerup', stopMapDrag);
  window.addEventListener('pointercancel', stopMapDrag);
}

function updateMapPointer(e) {
  if (!App.isColorDragMap) return;
  const mapRect = document.getElementById('color-map').getBoundingClientRect();
  let x = Math.max(0, Math.min(e.clientX - mapRect.left, mapRect.width));
  let y = Math.max(0, Math.min(e.clientY - mapRect.top, mapRect.height));
  document.getElementById('color-pointer').style.left = `${(x / mapRect.width) * 100}%`;
  document.getElementById('color-pointer').style.top = `${(y / mapRect.height) * 100}%`;
  App.currentHSV.s = x / mapRect.width;
  App.currentHSV.v = 1 - (y / mapRect.height);
  updateUIFromHSV(true);
}

function stopMapDrag() {
  App.isColorDragMap = false;
  window.removeEventListener('pointermove', updateMapPointer);
  window.removeEventListener('pointerup', stopMapDrag);
  window.removeEventListener('pointercancel', stopMapDrag);
}

/// END DYNAMIC WINDOWS ///

function relaxSend(_cmd) {
  sendCmd('main', 'relax', _cmd);
}

// volume controls
function sendVol(_cmd) {
  if (App.ctlCommand == 'lr' ){
    setAmpVolume(_cmd, 'lrxmit', 100); // living room system
  }
  if (App.ctlCommand == 'subs' ){
    sendCmd('main', 'lrxmit', 'sub' + _cmd); // living room subwoofers
  }  
  if (App.ctlCommand == 'br' ){
    setAmpVolume(_cmd, 'brpi', 192); // bedroom system
  }
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

function showVolumePopup(vol) {
  let win_name = "vol-popup";
  let text_name = "vol-text";
  let grid_name = "vol-grid";
  let bar_name = "vol-bar";
  let voltext, volgrid, volbar;
  let volpopwin = document.getElementById(win_name);
  if (!volpopwin) { // window does not exist
    // draw window
    volpopwin = document.createElement("div");
    volpopwin.classList.add("popup-alert");
    volpopwin.id = win_name;
    // volume text box
    voltext = document.createElement("div");
    voltext.id = text_name;
    volpopwin.appendChild(voltext);
    // progress bar
    volgrid = document.createElement("div");
    volgrid.classList.add("progress_grid");
    volgrid.id = grid_name;
    volbar = document.createElement("div");
    volbar.classList.add("bar_progress");
    volbar.id = bar_name;
    volgrid.appendChild(volbar);
    volpopwin.appendChild(volgrid);
    // add to page
    document.body.appendChild(volpopwin);
  } else {
    // select elements in existing window
    volbar = document.getElementById(bar_name);
    volgrid = document.getElementById(grid_name);
    voltext = document.getElementById(text_name);
  }
  // set volume display
  switch(vol) {
    case -1:
      volgrid.style.display = "none";
      voltext.innerHTML = "---";
      break;    
    case 0:
      volgrid.style.display = "none";
      voltext.innerHTML = "Mute";
      break;     
    default:
      volgrid.style.display = "block";
      let volstr = vol + "%";
      voltext.innerHTML = volstr;
      volbar.style.width = volstr;
  }
  // reset hide window timeout
  clearTimeout(App.volPopupTimer);
  // hide window after delay
  App.volPopupTimer = setTimeout(() => {
    if (volpopwin) { // window exists
      // fade out window
      volpopwin.classList.add('fade-out');
      // wait for the transition to end before removing window
      volpopwin.addEventListener("animationend", function (event) {
        if (event && volpopwin) {
          document.body.removeChild(volpopwin);
        }
      });
    }
  }, 3000); // hide delay in (ms)
}

function roomOnOff(action) {
  sendCmd('main', App.ctlCommand + action, '');
}

// add / remove a class from a element
function addClassToElem(_action, _elmid, _class) {
  let _elm = document.getElementById(_elmid);
  if (_action === 'add') {
    if (!(_elm.classList.contains(_class))) {
      _elm.classList.add(_class);
    }
  }
  if (_action === 'remove') {
    if (_elm.classList.contains(_class)) {
      _elm.classList.remove(_class);
    } 
  }
}

function subMode(_action) {
  // hide subwoofer mode indicator
  addClassToElem(_action, 'voldwnbtn', 'submode_btn');
  addClassToElem(_action, 'volmutebtn', 'submode_btn');
  addClassToElem(_action, 'volupbtn', 'submode_btn');
}

// controls menu actions
function ctlsMenu(_mode) {
  App.ctlCommand = _mode; 
  // living room controls
  if ((_mode === 'lr') || (_mode === 'subs')) {
    // disable bedroom grid
    classDisplay('bedroom-grid', 'none');
    // enable hifi grid
    classDisplay('hifi-grid', 'block');
    if (_mode === 'lr') {
      // hide subwoofer controls 
      subMode('remove');
    } else {
      // subwoofer controls
      subMode('add');
    }
  } 
  // bedroom controls
  if (_mode === 'br') { 
    // disable hifi grid
    classDisplay('hifi-grid', 'none');
    // hide subwoofer controls 
    subMode('remove');
    // enable bedroom grid
    classDisplay('bedroom-grid', 'block');
  }
  // save state 
  localStorage.setItem("ctls-mode", _mode);
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

//// Bookmarks Menu ////

function showBookmarks() {
  // hide menu if clicked while open
  if (App.bookmarkState !== App.BKM_INACTIVE) {
    removeDropdowns();
    return;
  }
  // draw bookmarks
  showDynMenu('bookmarks');
  // show add / edit buttons
  classDisplay("bookmark-buttons", "block");
  // link open mode
  App.bookmarkState = App.BKM_OPEN_MODE;
}

function showBookmarkSearch() {
  const elem = document.getElementById("bookmarks");
  if (elem) {
    // draw text input box
    let searchBox = document.createElement("input"); 
    searchBox.type = "text";
    searchBox.autocorrect = "off";
    searchBox.autocapitalize = "none"; 
    searchBox.placeholder = "Search here...";
    searchBox.classList.add('nohide');
    searchBox.id = 'bookmark__search_txt';
    // draw menu container 
    let searchMenuItem = document.createElement('a');
    searchMenuItem.id = "bookmark__search";
    searchMenuItem.classList.add('nohide');
    // draw search icon
    let searchIcon = document.createElement("div");
    searchIcon.id = "bookmark__search_icon";
    showBookmarkSearchIcon(searchIcon);
    // add objects to window
    searchMenuItem.appendChild(searchIcon);
    searchMenuItem.appendChild(searchBox);
    elem.appendChild(searchMenuItem);
    // focus keyboard on search text box
    searchBox.focus();
    new Promise(function() {
      // add input listener
      searchMenuItem.addEventListener('input', function searchInputListener(int_evnt) {
        filterBookmarkItems(searchBox.value);
        // cleanup listener
        if (int_evnt.target.tagName !== 'INPUT') { return; }
        searchMenuItem.removeEventListener('click', searchInputListener); 
      });
      // search icon button listener
      searchMenuItem.addEventListener('click', function handleSearchButtonClick(btn_evnt) {
        // reset search
        filterBookmarkItems("");
        searchBox.value = "";
        searchBox.focus();
        // cleanup listener
        if (btn_evnt.target.tagName !== 'BUTTON') { return; }
        searchMenuItem.removeEventListener('click', handleSearchButtonClick);
      });
    });
  }
}

function filterBookmarkItems(termIn) {
  const searchTerm = termIn.toLowerCase();
  const searchIcon = document.getElementById("bookmark__search_icon")
  if (searchTerm === "") {
    showBookmarkSearchIcon(searchIcon);
  } else {
    showCancelSearchIcon(searchIcon);
  }
  const bookmarks = document.getElementById("bookmarks");
  if (bookmarks) {
    [...bookmarks.getElementsByClassName('bookmarked__item')].forEach(elem => {
      if (elem) {
        const bookmarkText = elem.textContent.toLowerCase();
        if (bookmarkText.includes(searchTerm)) {
          elem.style.display = 'list-item';
        } else {
          elem.style.display = 'none';
        }
      }
    });
  }
}

function showBookmarkSearchIcon(searchIcon) {
  searchIcon.className = "";
  searchIcon.classList.add("fad");
  searchIcon.classList.add("fa-search");
}

function showCancelSearchIcon(searchIcon) {
  searchIcon.className = "";
  searchIcon.classList.add("fas");
  searchIcon.classList.add("fa-times");
}

function removeBookmarkSearch() {
  const search = document.getElementById("bookmark__search");
  const search_txt = document.getElementById("bookmark__search_txt");
  const search_icon = document.getElementById("bookmark__search_icon")
  if (search_icon) search_icon.remove();
  if (search_txt) search_txt.remove();
  if (search) search.remove();
}

function editBookmark() {
  // hide menu if clicked while open
  if (App.bookmarkState === App.BKM_EDIT_MODE) {
    removeDropdowns();
    return;
  }
  enableEditAddMode();
}

function addBookmark() {
  enableEditAddMode();
  // create new unique menu ID
  let _newid;
  const _datalen = App.fileData.length;
  if (_datalen <= 1) {
    if (_datalen == 1) {
      _newid = 1;
    } else {
      _newid = 0;
    }
  } else {
    _newid = _datalen - 1;
  }
  // add temporary entry to menu array
  App.fileData.push("placeholder");
  // create placeholder menu item
  const navElement = document.getElementById('bookmarks');
  const _elm = document.createElement('a');
  _elm.id = "menu-" + _newid;
  _elm.innerText = "New Bookmark";
  _elm.classList.add('bookmarked__item');
  // store URL
  Object.defineProperty(_elm, "url", {
    enumerable: false,
    writable: true,
    value: "about:blank"
  });
  // define click action
  _elm.addEventListener("click", function() {
    clickBookmark(_newid);
  });
  // highlight selected item
  _elm.classList.add("dd-selected");
  // insert item below 
  navElement.insertBefore(_elm,navElement.firstChild);
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
  App.bookmarkState = App.BKM_EDIT_MODE;
  closeBookmarkPrompt();
}

function clickBookmark(id) {
  const elmid = "menu-" + id;
  const elem = document.getElementById(elmid);
  const url = Object.values(elem.url).join("");
  const name = elem.innerText;
  if (elem) { 
    if (App.bookmarkState === App.BKM_OPEN_MODE) {
      if (!(url === null || url == "" || url == "about:blank")) {
        // open URL in new tab
        window.open(url, "_blank");
      }
    }
    if (App.bookmarkState === App.BKM_EDIT_MODE) {
      // edit bookmark item
      closeBookmarkPrompt();
      elem.classList.add("dd-selected"); // highlight selected item
      drawBookmarkPrompt(false, url, name, elem);
    }
  }
}

async function drawBookmarkPrompt(add, url, name, elem){
  // create empty window
  let editFavPrompt = document.createElement("div"); 
  editFavPrompt.className = "editFav__win";
  editFavPrompt.id = "editFav__prompt";
  editFavPrompt.classList.add("prompt_win");
  // Link name edit box
  let editFavName = document.createElement("input"); 
  editFavName.type = "text";
  editFavName.value = name;
  editFavName.autocorrect = "off";
  editFavName.autocapitalize = "none"; 
  editFavName.id = "editFav__namebox";
  editFavName.classList.add("editFav__textbox");
  // URL edit box
  let editFavURL = document.createElement("input"); 
  editFavURL.type = "text";
  editFavURL.value = url;
  editFavURL.autocorrect = "off";
  editFavURL.autocapitalize = "none";
  editFavURL.id = "editFav__urlbox";
  editFavURL.classList.add("editFav__textbox");
  let bannerText;
  if (add === true) { // add new mode
    bannerText = "Add Bookmark";
    editFavName.placeholder = "Enter Name";
    editFavURL.placeholder = "Enter URL";
  } else { // edit mode 
    bannerText = "Edit Bookmark";
  }
  // window banner text
  let editFavText = document.createElement("div");
  editFavText.classList.add("editFav__toptext");
  editFavText.innerHTML = bannerText; 
  // cancel button
  let editFavCancelBtn = document.createElement("button");
  editFavCancelBtn.classList.add("editFav__button");
  editFavCancelBtn.classList.add("button");
  editFavCancelBtn.classList.add("fas");
  editFavCancelBtn.classList.add("fa-times");
  editFavCancelBtn.type = "button";
  // lookup favorites
  let editFavLookupBtn = document.createElement("button");
  editFavLookupBtn.classList.add("editFav__button");
  editFavLookupBtn.classList.add("button");
  editFavLookupBtn.classList.add("fad");
  editFavLookupBtn.classList.add("fa-search");
  editFavLookupBtn.type = "button";
  // save button
  let editFavSaveBtn = document.createElement("button");
  editFavSaveBtn.classList.add("editFav__button");
  editFavSaveBtn.classList.add("button");
  editFavSaveBtn.classList.add("fad");
  editFavSaveBtn.classList.add("fa-save");
  editFavSaveBtn.type = "button";
  // delete button
  let editFavDeleteBtn = document.createElement("button");
  editFavDeleteBtn.classList.add("editFav__button");
  editFavDeleteBtn.classList.add("button");
  editFavDeleteBtn.classList.add("fad");
  editFavDeleteBtn.classList.add("fa-trash-alt");
  editFavDeleteBtn.type = "button";
  // up button
  let editFavUpBtn = document.createElement("button");
  editFavUpBtn.classList.add("editFav__button");
  editFavUpBtn.classList.add("button");
  editFavUpBtn.classList.add("fad");
  editFavUpBtn.classList.add("fa-arrow-up");
  editFavUpBtn.type = "button"; 
  // down button
  let editFavDownBtn = document.createElement("button");
  editFavDownBtn.classList.add("editFav__button");
  editFavDownBtn.classList.add("button");
  editFavDownBtn.classList.add("fad");
  editFavDownBtn.classList.add("fa-arrow-down");
  editFavDownBtn.type = "button";
  // append elements to window
  editFavPrompt.appendChild(editFavText);      // window title text
  editFavPrompt.appendChild(editFavName);      // name text box
  editFavPrompt.appendChild(editFavURL);       // URL text box
  editFavPrompt.appendChild(editFavUpBtn);     // row #1
  editFavPrompt.appendChild(editFavLookupBtn); // row #1 
  editFavPrompt.appendChild(editFavDownBtn);   // row #2
  editFavPrompt.appendChild(editFavSaveBtn);   // row #2
  editFavPrompt.appendChild(editFavCancelBtn); // row #3
  editFavPrompt.appendChild(editFavDeleteBtn); // row #3
  // display window on page
  document.body.appendChild(editFavPrompt);
  // handle button actions
  new Promise(function() {
    editFavPrompt.addEventListener('click', function handleButtonClicks(btn_event) { 
    if (btn_event.target.tagName !== 'BUTTON') { return; }
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
        editFavPrompt.removeEventListener('click', handleButtonClicks);
        removeDropdowns();
      }
      // lookup URL action
      if (btn_event.target === editFavLookupBtn) {
        lookupURL();
      }
      // save button action
      if (btn_event.target === editFavSaveBtn) {
        // do not allow empty URL or name
        var stopsave = false;
        if (editFavName.value === null || editFavName.value == "") {
          editFavName.placeholder = "Name cannot be empty";
          stopsave = true;
        }
        if (editFavURL.value === null || editFavURL.value == "") {
          editFavURL.placeholder = "URL cannot be empty";
          stopsave = true;
        }
        if (stopsave === true){
          return;
        }
        // update changed values
        if (elem) { 
          // save name to menu object
          let _boxname = editFavName.value.replaceAll("|", "-");
          // name length limiter
          let maxNameLength = 42;
          if (_boxname.length >= maxNameLength) {
            elem.innerText = _boxname.substring(0,maxNameLength) + "...";
          } else {
            elem.innerText = _boxname;
          }
          // save URL to menu object
          let _boxurl = addHTTPtoURL(editFavURL.value.replaceAll("|", "-"));
          elem['url'] = _boxurl;
          editFavURL.value = _boxurl;
          editFavText.innerHTML = "Changes Saved"; 
        }
        // save to file
        saveBookmarks();
        // close dropdown
        editFavPrompt.removeEventListener('click', handleButtonClicks);
        closeBookmarkPrompt();
      }
      // delete button action
      if (btn_event.target === editFavDeleteBtn) {
        elem.remove();
        // save to file
        saveBookmarks();
        // close dropdown
        editFavPrompt.removeEventListener('click', handleButtonClicks);
        closeBookmarkPrompt();
      }    
    });
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

// close add / edit window
function closeBookmarkPrompt() {
  // re-enable click outside of window
  document.querySelectorAll("*:not(#editFav__prompt)").forEach(elem1 => {
    elem1.style.pointerEvents = "";
  }); 
  // un-highlight selected item
  for (var idx = 0; idx <= App.fileData.length; idx++) {
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
          _menudata += 'subsbutton|hfsubs|Subs Gain\n';       
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
        // power on/off toggle switch
        if (cmd1 == '1' || cmd1 == '0')
          _menudata += `${cmd1}~${target}~${menu}|toggle|\n`;
        // dual LED lamps channel 1=ON/OFF 2=PWM
        if (menu.endsWith('_dual')) 
          _menudata += `${cmd2}~main~${menu}|slider|\n`;
        // global LEDs brightness
        if (menu == 'fc_leds') 
          _menudata += `${cmd1}~leds~fc|slider|\n`;
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
    sendCmd('main', 'status', '').then((data) => { // GET request
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
    serverAction('files-mnt_arch_region');
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
    showPopup("[loadLog] " + error.message);
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
        showPopup("[savePOST] failed! " + file);
      }
    }
  }
  xhr.send(data);
}

//// Dynamic Menus ////

function showDynMenu(menu) {
  let _elem = document.getElementById(menu);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    removeDropdowns();
     _elem.style.display = 'block';
    // build URL / append data
    const url = location.protocol+ "//" +location.hostname + "/api/read?file=" + menu;
    loadMenu(url).then((data) => { // wait for response
      if (menu === 'bookmarks') {
        showBookmarkSearch();
      }
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
    showPopup("[loadMenu] " + error);
  }
}

// draws each menu item
function drawMenu(data, menu) {
  // remove any current dynamic menus
  removeDynMenus();
  if (data && typeof data === 'string') {
    // erase global data
    while (App.fileData.length) { 
      App.fileData.pop(); 
    }
    let splitdat = data.replace(/^"|"$/g, '').replace(/\\n/g, '\n').split('\n');
    let rows = splitdat.length;
    for (let row = 0; row < rows; row++) {
      let line = splitdat[row];
      if (line != "") {
        App.fileData.push(line); // write to array
        const navElement = document.getElementById(menu);
        const linearr = line.split("|");
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
    showPopup("[drawMenu] no data");
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
    case 'chkon': 
      // check-box checked
      drawCheckBoxItem(_elm, col0, col2, true);
      break;
    case 'chkoff': 
      // check-box unchecked
      drawCheckBoxItem(_elm, col0, col2, false);
      break;
    case 'ledmenu': 
      // LEDs menu
      const effect_cmd = col0;
      _elm.addEventListener("click", function() {
        if (effect_cmd == 'textscroll') {
          openTextScroll();
        } else {
          sendCmd('leds',effect_cmd,'');
        }
      });
      break;
    case 'relaxmenu': 
      // relax menu
      _elm.addEventListener("click", function() {
        relaxSend(col0);
      });
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
        throttledSliderSend(host, cmd, event.target.value);
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
    case 'hfsubs':
      // subwoofer volume toggle
      if (App.ctlCommand == 'subs' ) {
        _elm.classList.add('submode_btn');
      }
      _elm.addEventListener("click", function() {
        // toggle subwoofer mode
        if (App.ctlCommand == 'lr') {
          ctlsMenu('subs');
        } else {
          ctlsMenu('lr');
        } 
      });
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

function drawCheckBoxItem(elm, url, boxid, state) {
  const _cbox = document.createElement('input');
  _cbox.type = "checkbox";
  _cbox.className = "chkbox";
  _cbox.id = "chkbox-" + boxid;
  // read checkbox state from file
  _cbox.checked = state;
  elm.appendChild(_cbox);
  // URL on click
  elm.href = url;
  // a checkbox was clicked
  elm.addEventListener('click', function(event) {
    if (event.target.classList.contains('chkbox')) {
      boxChanged();
    }
  });
}

function boxChanged() {
  // loop through checkbox's state
  for (var i = 0; i < (App.fileData.length - 1); i++) {
    let line = App.fileData[i];
    // split up into array (host,state,name)
    const linearr = line.split("|");
    // only write box state on 0/1 state items
    if (linearr[1] == 'chkon' || linearr[1] == 'chkoff') {
      // read elements checkbox then write state
      const box = "chkbox-" + linearr[2];
      var boxelm = document.getElementById(box);
      if (boxelm.checked === true) {
        linearr[1] = 'chkon';
      } else {
        linearr[1] = 'chkoff';
      }
      // build new data
      let _outstr = linearr.join('|');
      // replace existing data
      App.fileData[i] = _outstr;
    }
  }
  // write checkbox changes to file (III)
  let menuid = App.fileData[App.fileData.length - 1]; // extract menu ID
  // remove last element of array
  App.fileData.pop();
  // update file
  let fileout = "";
  for (var row = 0; row < App.fileData.length; row++) {
    fileout += App.fileData[row] + '\n';
  }
  savePOST(menuid, fileout);
  // add menu ID back to array
  App.fileData.push(menuid);
}

function removeDynMenus() {
  // only if menu is on-screen 
  if (App.dynMenuActive == 1) {
    // remove dynamic menu elements (II)
    for (var idx = 0; idx <= App.fileData.length; idx++) {
      const _menuid = "menu-" + idx;
      const menuRemove = document.getElementById(_menuid);
      if (menuRemove != null) {
        menuRemove.remove();
      }
    }
    App.dynMenuActive = 0;
  }
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
