// Automate - Server window: the log window and its option panels (the server's main,
// backup and regions, or a Pi's one). A panel button arms one server command and the
// send button sends it; password commands ask first. Also the windows the panels open:
// password prompt, guest Wi-Fi, a Pi's Wi-Fi setup, upload.

import { classDisplay, base64URLSafeEncode, sendCmd, html, fetchAnswer,
  savePOST, fileURL, promptWindow, onButtons, setDisplayById,
  registerListeners, gridButtonTemplate, dropdownButtonTemplate,
  GoToExtPage } from './utils.js';
import { removeDropdowns, openDropdown } from './dropdowns.js';
import { reportError } from './error.js';
import Uploader from './upload.js';
import { SSIDpwd } from './wifi.js';

const LOG_WINDOW_ID = 'logForm';
const LOG_TEXT_BOX_ID = 'logTextBox';
const SEND_BUTTON_ID = 'sendButton';
const SEND_ARMED_CLASS = 'selected_btn';
const SERVER_LOG_FILE = 'sysout';
const OPTIONS_WINDOW_CLASS = 'svropt__prompt';
const OPTIONS_PANEL_CONTAINER_ID = 'svropt-panel-container';
// every option panel button's size
const OPTION_BUTTON_CLASS = 'svropt_button';

// the command a panel button has armed, or null
let pendingServerCommand = null;

// numbered, so an older log read landing late cannot put a stale log on screen
let logReadSequence = 0;

//// page templates ////

// one plain button of an option panel
function optionButton(buttonId, icon, label, extraClass = '') {
  return gridButtonTemplate({
    buttonId: buttonId,
    icon: icon,
    label: label,
    buttonClass: `${OPTION_BUTTON_CLASS} ${extraClass}`.trim()
  });
}

// an option panel button with its menu: btn-menu-<name> opening <name>-menu
function optionMenu(name, icon, label, menuContent) {
  return dropdownButtonTemplate({
    buttonId: `btn-menu-${name}`,
    icon: icon,
    label: label,
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: `${name}-menu`,
    menuClass: 'dd-button',
    menuContent: menuContent
  });
}

// Start and Stop rows for a service: btn-svr-start<name>, btn-svr-stop<name>
function startStopRows(name) {
  return `
      <a id="btn-svr-start${name}"><i class="fad fa-play"></i> Start</a>
      <a id="btn-svr-stop${name}"><i class="fad fa-stop"></i> Stop</a>`;
}

// handlers that open the menus optionMenu() draws
function menuOpeners(names) {
  const listeners = {};
  names.forEach(name => {
    listeners[`btn-menu-${name}`] = () => openDropdown(`${name}-menu`);
  });
  return listeners;
}

// handlers for btn-svr-<command> buttons, which arm their command
function commandButtons(commands) {
  const listeners = {};
  commands.forEach(command => {
    listeners[`btn-svr-${command}`] = () => serverAction(command);
  });
  return listeners;
}

// The options window: the container for the server's panels, or a Pi's one panel.
export function serverOptionsTemplate(f) {
  return `
<div class="svropt__prompt prompt_win">
  ${f.isServer ? '<div id="svropt-panel-container"></div>' : piOptionsTemplate()}
</div>`;
}

// the server's main panel
function svrOptMainTemplate() {
  return `
<div class="svropt__panel svropt__main">
  ${optionMenu('atalk', 'fab fa-apple', 'AFP', startStopRows('atalk'))}
  ${optionMenu('legacy', 'fad fa-save', 'CIFS', startStopRows('legacy'))}
  ${optionMenu('xana', 'fab fa-linux', 'Xana', startStopRows('xana'))}
  ${optionButton('btn-open-regions', 'fad fa-th', 'Regions')}
  ${optionMenu('system', 'fad fa-microchip', 'System', `
      <a id="btn-ext-system"><i class="fad fa-link"></i> Open</a>
      <a id="btn-svr-syslog"><i class="fad fa-tachometer"></i> Status</a>`)}
  ${optionButton('btn-open-backup', 'fad fa-sync', 'Backup')}
  ${optionMenu('router', 'fad fa-ethernet', 'Router', `
      <a id="btn-ext-router"><i class="fad fa-link"></i> Open</a>
      <a id="btn-svr-rebootrouter"><i class="fad fa-redo"></i> Reboot</a>`)}
  ${optionMenu('netwrk', 'fad fa-network-wired', 'Network', `
      <a id="btn-wifi-prompt"><i class="fad fa-wifi"></i> Access</a>
      <a id="btn-ext-edgeswitch"><i class="fad fa-sitemap"></i> Switch</a>
      <a id="btn-svr-netscan"><i class="fad fa-chart-network"></i> LAN Scanner</a>`)}
  ${optionMenu('unifi', 'fad fa-router', 'UniFi', `
      <a id="btn-ext-unifi"><i class="fad fa-link"></i> Open</a>${startStopRows('unifi')}`)}
  ${optionButton('btn-close-svr-opt1', 'fad fa-times-octagon', 'Close')}
  ${optionButton('btn-svr-clearlog', 'fad fa-trash-alt', 'Clear')}
  ${optionButton('btn-upload', 'fad fa-cloud-upload', 'Upload')}
</div>`;
}
registerListeners({
  'btn-upload':           () => showUpload(),
  ...menuOpeners(['atalk', 'legacy', 'unifi', 'xana', 'system', 'netwrk', 'router']),
  ...commandButtons(['startatalk', 'stopatalk', 'startlegacy', 'stoplegacy', 'startunifi',
    'stopunifi', 'startxana', 'stopxana', 'syslog', 'netscan', 'clearlog', 'rebootrouter']),
  'btn-ext-unifi':        () => GoToExtPage('unifi:11443/network/ff7gqlig/dashboard'),
  'btn-ext-system':       () => GoToExtPage('pve:8006'),
  'btn-open-backup':      () => openServerPanel(svrOptBackupTemplate),
  'btn-open-regions':     () => openServerPanel(svrOptRegionsTemplate),
  'btn-wifi-prompt':      () => wifiPrompt(),
  'btn-ext-edgeswitch':   () => GoToExtPage('edgeswitch.home'),
  'btn-close-svr-opt1':   () => closeServerOptions(),
  'btn-ext-router':       () => GoToExtPage('router.home')
});

// the server's backup panel
function svrOptBackupTemplate() {
  return `
<div class="svropt__panel svropt__backup">
  ${optionButton('btn-svr-last_backup_dates', 'fad fa-history', 'History')}
  ${optionButton('btn-svr-backupchk', '', 'Checksum')}
  ${optionButton('btn-svr-pve_vmsbkp', '', 'VMs/LXCs')}
  ${optionButton('btn-pwd-unlock', 'fad fa-lock-open', 'Unlock')}
  ${optionButton('btn-svr-backupstd', '', 'Standard')}
  ${optionButton('btn-svr-detach_bkps', 'fad fa-eject', 'Eject')}
  ${optionButton('btn-close-svr-opt2', 'fad fa-times-octagon', 'Close')}
  ${optionButton('btn-svr-pve_listsnaps', '', 'Snapshots')}
  ${optionButton('btn-open-svr-opt1', 'fad fa-arrow-left', 'Back')}
</div>`;
}
registerListeners({
  ...commandButtons(['last_backup_dates', 'backupchk', 'pve_vmsbkp', 'backupstd', 'detach_bkps', 'pve_listsnaps']),
  'btn-pwd-unlock':     () => getPassword('pwd'),
  'btn-close-svr-opt2': () => closeServerOptions(),
  'btn-open-svr-opt1':  () => openServerPanel(svrOptMainTemplate)
});

// the server's regions panel: archive, snapshot and RAM disk mounts
function svrOptRegionsTemplate() {
  return `
<div class="svropt__panel svropt__regions">
  ${optionMenu('archive', '', 'Archive', `
      <a id="btn-pwd-fusearch"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-pwd-fusearch_unmnt"><i class="fad fa-eject"></i> Detach</a>`)}
  ${optionMenu('snapshots', '', 'Snapshots', `
      <a id="btn-svr-mnt_snap_region"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-svr-unmnt_snap_region"><i class="fad fa-eject"></i> Detach</a>`)}
  ${optionMenu('ramdisk', '', 'RAM Disk', `
      <a id="btn-svr-mnt_ram_region"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-svr-unmnt_ram_region"><i class="fad fa-eject"></i> Detach</a>`)}
  ${optionButton('btn-close-svr-opt3', 'fad fa-times-octagon', 'Close')}
  ${optionButton('btn-svr-unmnt_all', '', 'Detach All', 'dark_btn')}
  ${optionButton('btn-open-svr-opt2', 'fad fa-arrow-left', 'Back')}
</div>`;
}
registerListeners({
  ...menuOpeners(['archive', 'snapshots', 'ramdisk']),
  ...commandButtons(['mnt_snap_region', 'unmnt_snap_region', 'mnt_ram_region', 'unmnt_ram_region', 'unmnt_all']),
  'btn-pwd-fusearch':       () => getPassword('fusearch'),
  'btn-pwd-fusearch_unmnt': () => serverAction('unmnt_arch_region'),
  'btn-close-svr-opt3':     () => closeServerOptions(),
  'btn-open-svr-opt2':      () => openServerPanel(svrOptMainTemplate)
});

// the Pi models' options panel
function piOptionsTemplate() {
  return `
<div class="svropt__panel">
  ${optionMenu('piwifi', 'fad fa-wifi', 'WiFi', `
      <a id="btn-show-piwifiprompt"> Setup WiFi</a>
      <a id="btn-svr-client"> Client Mode</a>
      <a id="btn-svr-apd"> Hotspot Mode</a>
      <a id="btn-svr-wifiscan"> WiFi Scanner</a>`)}
  ${optionButton('btn-svr-showwpa', '', 'Network')}
  ${optionButton('btn-svr-temp', '', 'Temperature')}
  ${optionButton('btn-close-svr-opt4', 'fad fa-times-octagon', 'Close')}
  ${optionButton('btn-svr-pi-clearlog', '', 'Clear Log')}
  ${optionButton('btn-svr-resetbt', 'fab fa-bluetooth', 'Reset')}
</div>`;
}
registerListeners({
  ...menuOpeners(['piwifi']),
  ...commandButtons(['client', 'apd', 'wifiscan', 'showwpa', 'temp', 'resetbt']),
  'btn-show-piwifiprompt': () => showPiWiFiPrompt(),
  'btn-close-svr-opt4':    () => closeServerOptions(),
  'btn-svr-pi-clearlog':   () => serverAction('clearlog')
});

// the log window (all models; a Pi opens it from its System row)
export function logFormTemplate() {
  return `
<div class="log-popup popup-win" id="logForm">
  <div class="popup-ctn">
    <textarea readonly class="scroll-bar-style" id="logTextBox" spellcheck="false"></textarea>
    <div class="popup-actions">
      ${gridButtonTemplate({
        buttonId: 'btn-log-close',
        icon: 'fad fa-times-octagon',
        buttonClass: 'button_left_txtwin'
      })}
      ${gridButtonTemplate({
        buttonId: 'btn-log-open-svr',
        icon: 'fad fa-sliders-h',
        buttonClass: 'button_left_txtwin'
      })}
      ${gridButtonTemplate({
        buttonId: 'btn-log-load',
        icon: 'fad fa-sync-alt',
        buttonClass: 'button_left_txtwin'
      })}
      ${gridButtonTemplate({
        buttonId: SEND_BUTTON_ID,
        icon: 'fad fa-upload',
        buttonClass: 'button_right_txtwin'
      })}
    </div>
  </div>
</div>`;
}
registerListeners({
  'btn-log-open-svr': () => openServerPanel(svrOptMainTemplate),
  'btn-log-load':     () => loadLog(),
  'sendButton':       () => serverSend()
});

//// log window ////

// Shows the log window and reads the log into it.
export function showLogWindow() {
  setDisplayById(LOG_WINDOW_ID, "block");
  loadLog();
}

// Hides the log window, its armed command and the options window over it.
export function closeLogWindow() {
  setDisplayById(LOG_WINDOW_ID, "none");
  clearPendingCmd();
  closeServerOptions();
}

// Reads the whole log into the window, on its newest line, and drops any armed command.
async function loadLog() {
  logReadSequence += 1;
  const readSequence = logReadSequence;
  clearPendingCmd();
  try {
    const answerText = await fetchAnswer(fileURL('read', SERVER_LOG_FILE));
    const logText = JSON.parse(answerText);
    // a newer read's text is the one to show
    if (readSequence !== logReadSequence) {
      return;
    }
    const logTextBox = document.getElementById(LOG_TEXT_BOX_ID);
    logTextBox.value = logText;
    logTextBox.scrollTop = logTextBox.scrollHeight;
  } catch (error) {
    reportError(`reading the ${SERVER_LOG_FILE} log`, error);
  }
}

//// arming and sending a command ////

// arms a server command; the send button then sends it
function serverAction(command) {
  pendingServerCommand = command;
  document.getElementById(SEND_BUTTON_ID).classList.add(SEND_ARMED_CLASS);
}

// Send button: sends the armed command and notes it in the log box, or asks for one.
function serverSend() {
  const logTextBox = document.getElementById(LOG_TEXT_BOX_ID);
  if (pendingServerCommand === null) {
    logTextBox.value = "select an option.";
  } else {
    sendCmd('main', 'server', pendingServerCommand);
    logTextBox.value += "\n" + pendingServerCommand + " command sent.";
    logTextBox.scrollTop = logTextBox.scrollHeight;
  }
  clearPendingCmd();
}

// drops the armed command and the send button's highlight
function clearPendingCmd() {
  document.getElementById(SEND_BUTTON_ID).classList.remove(SEND_ARMED_CLASS);
  pendingServerCommand = null;
}

// the command each password file is saved for
const PASSWORD_COMMANDS = { pwd: 'attach_bkps', fusearch: 'mnt_arch_region' };

// A password was saved: sends the command that uses it.
function afterPOSTActions(passwordFile) {
  serverAction(PASSWORD_COMMANDS[passwordFile]);
  serverSend();
}

//// options window ////

// Opens the options window on one of the server's panels; a Pi's panel is already in it.
function openServerPanel(panelTemplate) {
  closeServerOptions();
  const panelContainer = document.getElementById(OPTIONS_PANEL_CONTAINER_ID);
  if (panelContainer) {
    panelContainer.innerHTML = panelTemplate();
  }
  classDisplay(OPTIONS_WINDOW_CLASS, "block");
}

// Closes the options window, its menus and any armed command, and removes the panel.
function closeServerOptions() {
  removeDropdowns();
  clearPendingCmd();
  classDisplay(OPTIONS_WINDOW_CLASS, "none");
  const panelContainer = document.getElementById(OPTIONS_PANEL_CONTAINER_ID);
  if (panelContainer) {
    panelContainer.innerHTML = "";
  }
}

// Upload button: opens the file dialog.
function showUpload() {
  removeDropdowns();
  if (!App.uploader) {
    App.uploader = new Uploader();
  }
  App.uploader.pick();
}

/////// Pi Wi-Fi Window ///////

const PI_WIFI_PROMPT_ID = 'pinet__prompt';

// Pi options, Setup WiFi: asks for a network and password and sends them to the Pi.
async function showPiWiFiPrompt() {
  if (document.getElementById(PI_WIFI_PROMPT_ID)) {
    return;
  }
  removeDropdowns();
  const encodedSettings = await piWiFiPrompt(PI_WIFI_PROMPT_ID);
  if (encodedSettings !== null && encodedSettings !== "") {
    sendCmd("main", "confwpa", encodedSettings);
    document.getElementById(LOG_TEXT_BOX_ID).value =
      "Wi-Fi configuration updated, select client mode to apply changes.";
  }
}

// the Pi Wi-Fi window's buttons
const PI_WIFI_CANCEL_BUTTON_ID = 'pinet__btn-cancel';
const PI_WIFI_SAVE_BUTTON_ID = 'pinet__btn-save';

// Asks for an SSID and password; resolves with them base64 encoded as "ssid|$|password",
// or null when cancelled.
function piWiFiPrompt(windowId) {
  return new Promise((resolve) => {
    const promptElement = promptWindow(windowId, html`
      <div>Network (SSID):</div>
      <input id="pinet__ssidbox">
      <div>Password:</div>
      <input id="pinet__passbox" type="password">
      ${gridButtonTemplate({
        buttonId: PI_WIFI_CANCEL_BUTTON_ID,
        label: 'Cancel',
        buttonClass: 'pinet__btn'
      })}
      ${gridButtonTemplate({
        buttonId: PI_WIFI_SAVE_BUTTON_ID,
        label: 'Save',
        buttonClass: 'pinet__btn'
      })}
    `);
    const ssidLabel = promptElement.querySelector("div");
    const ssidBox = document.getElementById("pinet__ssidbox");
    const passwordBox = document.getElementById("pinet__passbox");
    ssidBox.focus();
    // empties the boxes, closes the window and settles the promise as null, which a
    // save has already settled
    function closeWiFiPrompt() {
      ssidBox.value = "";
      passwordBox.value = "";
      promptElement.remove();
      resolve(null);
    }
    // settles with the two values, or says which is missing
    function saveWiFiSettings() {
      if (ssidBox.value === "") {
        ssidLabel.innerHTML = "enter SSID!";
        return;
      }
      if (passwordBox.value === "") {
        ssidLabel.innerHTML = "enter password!";
        return;
      }
      resolve(base64URLSafeEncode(`${ssidBox.value}|$|${passwordBox.value}`));
      closeWiFiPrompt();
    }
    onButtons(promptElement, (button) => {
      if (button.id === PI_WIFI_SAVE_BUTTON_ID) {
        saveWiFiSettings();
      }
      if (button.id === PI_WIFI_CANCEL_BUTTON_ID) {
        closeWiFiPrompt();
      }
    });
    promptKeys(ssidBox, () => passwordBox.focus(), closeWiFiPrompt);
    promptKeys(passwordBox, saveWiFiSettings, closeWiFiPrompt);
  });
}

/////// guest Wi-Fi window ///////

const GUEST_WIFI_PROMPT_ID = 'wifi__prompt';

// Network menu, Access: the QR code and password for guest WiFi
function wifiPrompt() {
  if (document.getElementById(GUEST_WIFI_PROMPT_ID)) {
    return;
  }
  const promptElement = promptWindow(GUEST_WIFI_PROMPT_ID, html`
    <div>Scan for WiFi Access</div>
    <img src="img/wifi.png" id="wifi__img">
    <div id="wifi__pwdtxt"></div>
    ${gridButtonTemplate({
      buttonId: 'wifipmt__btn',
      label: 'Close'
    })}
  `);
  // set separately: the value is data, not markup
  document.getElementById("wifi__pwdtxt").innerHTML = atob(SSIDpwd());
  onButtons(promptElement, () => promptElement.remove());
}

/////// password prompt ///////

const PASSWORD_PROMPT_ID = 'pass__prompt_win';
const PASSWORD_SEND_BUTTON_ID = 'pass__btn-send';

// runs onEnter or onEscape as a box's keys are released
function promptKeys(box, onEnter, onEscape) {
  box.addEventListener("keyup", (keyEvent) => {
    if (keyEvent.key === "Enter") {
      onEnter();
    } else if (keyEvent.key === "Escape") {
      onEscape();
    }
  });
}

// Asks for a password and saves it to the file the server side reads, which then sends
// the command that needs it.
async function getPassword(passwordFile) {
  removeDropdowns();
  const password = await passwordPrompt();
  if (password !== null && password !== "") {
    savePOST(passwordFile, password, afterPOSTActions);
  }
}

// Resolves with the typed password, or null when cancelled or already open.
function passwordPrompt() {
  if (document.getElementById(PASSWORD_PROMPT_ID)) {
    return null;
  }
  const promptElement = promptWindow(PASSWORD_PROMPT_ID, html`
    <div>Enter password:</div>
    <input id="pass__textbox" type="password">
    <div id="pass__prmbtnctr">
      ${gridButtonTemplate({
        buttonId: 'pass__btn-cancel',
        label: 'Cancel',
        buttonClass: 'pass__prompt_btn'
      })}
      ${gridButtonTemplate({
        buttonId: PASSWORD_SEND_BUTTON_ID,
        label: 'Send',
        buttonClass: 'pass__prompt_btn'
      })}
    </div>
  `);
  const passwordBox = document.getElementById("pass__textbox");
  passwordBox.focus();
  return new Promise((resolve) => {
    const finish = (password) => {
      resolve(password);
      promptElement.remove();
    };
    onButtons(promptElement, (button) => {
      if (button.id === PASSWORD_SEND_BUTTON_ID) {
        finish(passwordBox.value);
      } else {
        finish(null);
      }
    });
    promptKeys(passwordBox, () => finish(passwordBox.value), () => finish(null));
  });
}
