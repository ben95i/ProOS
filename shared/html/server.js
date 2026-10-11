// Automate Website - server window
//
// The Server button's log window and the option panels that open over it:
// the server's three panels (main, backup, regions) and the Pi models' one.
// A panel button arms one server command and the log window's send button
// sends it; the commands that need a password ask for it first. The windows
// the panels open live here too -- the password prompt, the guest Wi-Fi
// window, the Pi's Wi-Fi setup and the upload picker.
//
// main.js draws serverOptionsTemplate() and logFormTemplate() into the page,
// and opens and closes the log window through showLogWindow() and
// closeLogWindow(), as it has the camera window to close alongside it.

import { classDisplay, addClassToElem, base64URLSafeEncode, sendCmd, html,
  showPopup, savePOST, fileURL, promptWindow, onButtons, setDisplayById,
  registerListeners, gridButtonTemplate, dropdownButtonTemplate } from './utils.js';
import { removeDropdowns, openDropdown } from './menus.js';
import Uploader from './upload.js';
import { SSIDpwd } from './wifi.js';

// the log window, its text box and its send button
const LOG_WINDOW_ID = 'logForm';
const LOG_TEXT_BOX_ID = 'logTextBox';
const SEND_BUTTON_ID = 'sendButton';
// the class the send button carries while a command is armed
const SEND_ARMED_CLASS = 'selected_btn';
// the file the server writes its log to
const SERVER_LOG_FILE = 'sysout';

// the options window, and the element the server's panels are drawn into
const OPTIONS_WINDOW_CLASS = 'svropt__prompt';
const OPTIONS_PANEL_CONTAINER_ID = 'svropt-panel-container';

// The server command a panel button has armed, or null when none is. The
// send button transmits it and disarms.
let pendingServerCommand = null;

// Log reads are numbered, so an older read that lands after a newer one
// cannot put a stale log back on screen.
let logReadSequence = 0;

//// page templates ////

// every button of an option panel carries the panel's own button size
const OPTION_BUTTON_CLASS = 'svropt_button';

// The options window: an empty container the server's panels are drawn into,
// or the Pi models' one panel. f says which model the page is drawn for.
export function serverOptionsTemplate(f) {
  return `
<div class="svropt__prompt prompt_win" data-app-injected>
  ${f.isServer ? `
  <div id="svropt-panel-container"></div>` : ''}
  ${f.isPiLike ? piOptionsTemplate() : ''}
</div>`;
}

// the server's main panel: services, network, and the way to the other two
function svrOptMainTemplate() {
  return `
<div class="svropt__main">
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-atalk',
    icon: 'fab fa-apple',
    label: 'AFP',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'atalk-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-svr-startatalk"><i class="fad fa-play"></i> Start</a>
      <a id="btn-svr-stopatalk"><i class="fad fa-stop"></i> Stop</a>`
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-legacy',
    icon: 'fad fa-save',
    label: 'CIFS',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'legacy-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-svr-startlegacy"><i class="fad fa-play"></i> Start</a>
      <a id="btn-svr-stoplegacy"><i class="fad fa-stop"></i> Stop</a>`
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-xana',
    icon: 'fab fa-linux',
    label: 'Xana',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'xana-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-svr-startxana"><i class="fad fa-play"></i> Start</a>
      <a id="btn-svr-stopxana"><i class="fad fa-stop"></i> Stop</a>`
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-open-regions',
    icon: 'fad fa-th',
    label: 'Regions',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-system',
    icon: 'fad fa-microchip',
    label: 'System',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'system-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-ext-system"><i class="fad fa-link"></i> Open</a>
      <a id="btn-svr-syslog"><i class="fad fa-tachometer"></i> Status</a>`
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-open-backup',
    icon: 'fad fa-sync',
    label: 'Backup',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-router',
    icon: 'fad fa-ethernet',
    label: 'Router',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'router-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-ext-router"><i class="fad fa-link"></i> Open</a>
      <a id="btn-svr-rebootrouter"><i class="fad fa-redo"></i> Reboot</a>`
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-netwrk',
    icon: 'fad fa-network-wired',
    label: 'Network',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'netwrk-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-wifi-prompt"><i class="fad fa-wifi"></i> Access</a>
      <a id="btn-ext-edgeswitch"><i class="fad fa-sitemap"></i> Switch</a>
      <a id="btn-svr-netscan"><i class="fad fa-chart-network"></i> LAN Scanner</a>`
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-unifi',
    icon: 'fad fa-router',
    label: 'UniFi',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'unifi-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-ext-unifi"><i class="fad fa-link"></i> Open</a>
      <a id="btn-svr-startunifi"><i class="fad fa-play"></i> Start</a>
      <a id="btn-svr-stopunifi"><i class="fad fa-stop"></i> Stop</a>`
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-close-svr-opt1',
    icon: 'fad fa-times-octagon',
    label: 'Close',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-clearlog',
    icon: 'fad fa-trash-alt',
    label: 'Clear',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-upload',
    icon: 'fad fa-cloud-upload',
    label: 'Upload',
    buttonClass: OPTION_BUTTON_CLASS
  })}
</div>`;
}
registerListeners({
  'btn-upload':           () => showUpload(),
  'btn-menu-atalk':       () => openDropdown('atalk-menu'),
  'btn-svr-startatalk':   () => serverAction('startatalk'),
  'btn-svr-stopatalk':    () => serverAction('stopatalk'),
  'btn-menu-legacy':      () => openDropdown('legacy-menu'),
  'btn-svr-startlegacy':  () => serverAction('startlegacy'),
  'btn-svr-stoplegacy':   () => serverAction('stoplegacy'),
  'btn-menu-unifi':       () => openDropdown('unifi-menu'),
  'btn-ext-unifi':        () => GoToExtPage('unifi:11443/network/ff7gqlig/dashboard'),
  'btn-svr-startunifi':   () => serverAction('startunifi'),
  'btn-svr-stopunifi':    () => serverAction('stopunifi'),
  'btn-menu-xana':        () => openDropdown('xana-menu'),
  'btn-svr-startxana':    () => serverAction('startxana'),
  'btn-svr-stopxana':     () => serverAction('stopxana'),
  'btn-menu-system':      () => openDropdown('system-menu'),
  'btn-ext-system':       () => GoToExtPage('pve:8006'),
  'btn-open-backup':      () => openServerPanel(svrOptBackupTemplate),
  'btn-open-regions':     () => openServerPanel(svrOptRegionsTemplate),
  'btn-svr-syslog':       () => serverAction('syslog'),
  'btn-menu-netwrk':      () => openDropdown('netwrk-menu'),
  'btn-wifi-prompt':      () => wifiPrompt(),
  'btn-ext-edgeswitch':   () => GoToExtPage('edgeswitch.home'),
  'btn-svr-netscan':      () => serverAction('netscan'),
  'btn-close-svr-opt1':   () => closeServerOptions(),
  'btn-svr-clearlog':     () => serverAction('clearlog'),
  'btn-menu-router':      () => openDropdown('router-menu'),
  'btn-ext-router':       () => GoToExtPage('router.home'),
  'btn-svr-rebootrouter': () => serverAction('rebootrouter')
});

// the server's backup panel
function svrOptBackupTemplate() {
  return `
<div class="svropt__backup">
  ${gridButtonTemplate({
    buttonId: 'btn-svr-last_backup_dates',
    icon: 'fad fa-history',
    label: 'History',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-backupchk',
    label: 'Checksum',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-pve_vmsbkp',
    label: 'VMs/LXCs',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-pwd-unlock',
    icon: 'fad fa-lock-open',
    label: 'Unlock',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-backupstd',
    label: 'Standard',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-detach_bkps',
    icon: 'fad fa-eject',
    label: 'Eject',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-close-svr-opt2',
    icon: 'fad fa-times-octagon',
    label: 'Close',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-pve_listsnaps',
    label: 'Snapshots',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-open-svr-opt1',
    icon: 'fad fa-arrow-left',
    label: 'Back',
    buttonClass: OPTION_BUTTON_CLASS
  })}
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

// the server's regions panel: archive, snapshot and RAM disk mounts
function svrOptRegionsTemplate() {
  return `
<div class="svropt__regions">
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-archive',
    label: 'Archive',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'archive-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-pwd-fusearch"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-pwd-fusearch_unmnt"><i class="fad fa-eject"></i> Detach</a>`
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-snapshots',
    label: 'Snapshots',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'snapshots-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-svr-mnt_snap_region"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-svr-unmnt_snap_region"><i class="fad fa-eject"></i> Detach</a>`
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-ramdisk',
    label: 'RAM Disk',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'ramdisk-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-svr-mnt_ram_region"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-svr-unmnt_ram_region"><i class="fad fa-eject"></i> Detach</a>`
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-close-svr-opt3',
    icon: 'fad fa-times-octagon',
    label: 'Close',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-unmnt_all',
    label: 'Detach All',
    buttonClass: `${OPTION_BUTTON_CLASS} dark_btn`
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-open-svr-opt2',
    icon: 'fad fa-arrow-left',
    label: 'Back',
    buttonClass: OPTION_BUTTON_CLASS
  })}
</div>`;
}
registerListeners({
  'btn-menu-archive':          () => openDropdown('archive-menu'),
  'btn-pwd-fusearch':          () => getPassword('fusearch'),
  'btn-pwd-fusearch_unmnt':    () => serverAction('unmnt_arch_region'),
  'btn-menu-snapshots':        () => openDropdown('snapshots-menu'),
  'btn-svr-mnt_snap_region':   () => serverAction('mnt_snap_region'),
  'btn-svr-unmnt_snap_region': () => serverAction('unmnt_snap_region'),
  'btn-menu-ramdisk':          () => openDropdown('ramdisk-menu'),
  'btn-svr-mnt_ram_region':    () => serverAction('mnt_ram_region'),
  'btn-svr-unmnt_ram_region':  () => serverAction('unmnt_ram_region'),
  'btn-close-svr-opt3':        () => closeServerOptions(),
  'btn-svr-unmnt_all':         () => serverAction('unmnt_all'),
  'btn-open-svr-opt2':         () => openServerPanel(svrOptMainTemplate),
  'btn-svr-mnt_ext_region':    () => serverAction('mnt_ext_region'),
  'btn-svr-unmnt_ext_region':  () => serverAction('unmnt_ext_region')
});

// the Pi models' options panel
function piOptionsTemplate() {
  return `
<div>
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-piwifi',
    icon: 'fad fa-wifi',
    label: 'WiFi',
    buttonClass: OPTION_BUTTON_CLASS,
    menuId: 'piwifi-menu',
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-show-piwifiprompt"> Setup WiFi</a>
      <a id="btn-svr-client"> Client Mode</a>
      <a id="btn-svr-apd"> Hotspot Mode</a>
      <a id="btn-svr-wifiscan"> WiFi Scanner</a>`
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-showwpa',
    label: 'Network',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-temp',
    label: 'Temperature',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-close-svr-opt4',
    icon: 'fad fa-times-octagon',
    label: 'Close',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-pi-clearlog',
    label: 'Clear Log',
    buttonClass: OPTION_BUTTON_CLASS
  })}
  ${gridButtonTemplate({
    buttonId: 'btn-svr-resetbt',
    icon: 'fab fa-bluetooth',
    label: 'Reset',
    buttonClass: OPTION_BUTTON_CLASS
  })}
</div>`;
}
registerListeners({
  'btn-menu-piwifi':       () => openDropdown('piwifi-menu'),
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

// server log popup window (all models -- Pi opens it via the System menu)
export function logFormTemplate() {
  return `
<div class="log-popup popup-win" id="logForm" data-app-injected>
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
  'btn-log-load':     () => loadLog(SERVER_LOG_FILE),
  'sendButton':       () => serverSend()
});

//// log window ////

// Shows the log window and reads the log into it. main.js closes whatever
// other window is open first.
export function showLogWindow() {
  setDisplayById(LOG_WINDOW_ID, "block");
  loadLog(SERVER_LOG_FILE);
}

// Hides the log window, drops an armed command and closes the options window
// over it.
export function closeLogWindow() {
  setDisplayById(LOG_WINDOW_ID, "none");
  clearPendingCmd();
  closeServerOptions();
}

// Loads an entire text file into the log window, scrolled to its newest
// line. A command armed before the reload is dropped.
function loadLog(file) {
  logReadSequence += 1;
  const readSequence = logReadSequence;
  fetch(fileURL('read', file), { method: 'GET' })
    .then((response) => {
      if (!response.ok) {
        throw new Error(`status ${response.status}`);
      }
      return response.json();
    })
    .then(logText => {
      // a newer read has gone out since; its text is the one to show
      if (readSequence !== logReadSequence) {
        return;
      }
      const logTextBox = document.getElementById(LOG_TEXT_BOX_ID);
      if (!logTextBox) {
        return;
      }
      logTextBox.value = logText;
      logTextBox.scrollTop = logTextBox.scrollHeight;
    })
    .catch(() => {
      showPopup("[loadLog] error fetching data.");
    });
  // The send button's highlight goes with the command. Only the command
  // used to be dropped here, which left the button lit with nothing to send.
  clearPendingCmd();
}

//// arming and sending a command ////

// arms a server command; the send button then transmits it
function serverAction(command) {
  pendingServerCommand = command;
  sendBtnAlert("on");
}

// Send button: transmits the armed command and notes it in the log box, or
// asks for one to be picked when none is armed.
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

// lights the send button while a command is armed: 'on' or 'off'
function sendBtnAlert(state) {
  if (state === 'on') {
    addClassToElem('add', SEND_BUTTON_ID, SEND_ARMED_CLASS);
  }
  if (state === 'off') {
    addClassToElem('remove', SEND_BUTTON_ID, SEND_ARMED_CLASS);
  }
}

// drops the armed command and the send button's highlight with it
function clearPendingCmd() {
  sendBtnAlert("off");
  pendingServerCommand = null;
}

// Once a password has been saved, arms the command that uses it and sends it
// straight away. passwordFile is the file the password was written to.
function afterPOSTActions(passwordFile) {
  let commandArmed = false;
  if (passwordFile === 'pwd') {
    serverAction('attach_bkps');
    commandArmed = true;
  }
  if (passwordFile === 'fusearch') {
    serverAction('mnt_arch_region');
    commandArmed = true;
  }
  if (commandArmed === true) {
    serverSend();
  }
}

//// options window ////

// Opens the options window on one of the server's panels. The Pi models have
// no panel container: their one panel is already in the window.
function openServerPanel(panelTemplate) {
  closeServerOptions();
  const panelContainer = document.getElementById(OPTIONS_PANEL_CONTAINER_ID);
  if (panelContainer) {
    panelContainer.innerHTML = panelTemplate();
  }
  classDisplay(OPTIONS_WINDOW_CLASS, "block");
}

// Closes the options window, its menus and any armed command, and takes the
// rendered panel markup out entirely.
function closeServerOptions() {
  removeDropdowns();
  clearPendingCmd();
  classDisplay(OPTIONS_WINDOW_CLASS, "none");
  const panelContainer = document.getElementById(OPTIONS_PANEL_CONTAINER_ID);
  if (panelContainer) {
    panelContainer.innerHTML = "";
  }
}

// Upload button: opens the file dialog. The uploader is kept on App, where
// the page teardown destroys it.
function showUpload() {
  removeDropdowns();
  if (!App.uploader) {
    App.uploader = new Uploader();
  }
  App.uploader.pick();
}

/////// Pi Wi-Fi Window ///////

const PI_WIFI_PROMPT_ID = 'pinet__prompt';

// Pi options, Setup WiFi: asks for a network and password and sends them to
// the Pi. A second press while the window is open does nothing.
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

// the two buttons of the Pi's Wi-Fi setup window
const PI_WIFI_CANCEL_BUTTON_ID = 'pinet__btn-cancel';
const PI_WIFI_SAVE_BUTTON_ID = 'pinet__btn-save';

// Asks for an SSID and password and resolves with them base64 encoded as
// "ssid|$|password", or null when it was cancelled.
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
    // empties the boxes, closes the window and settles the promise; after a
    // save the promise is already settled and the null is ignored
    function closeWiFiPrompt() {
      ssidBox.value = "";
      passwordBox.value = "";
      promptElement.remove();
      resolve(null);
    }
    // settles with the two values, or says which one is still missing
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
    ssidBox.addEventListener("keyup", (keyEvent) => {
      if (keyEvent.key === "Enter") {
        passwordBox.focus();
      } else if (keyEvent.key === "Escape") {
        closeWiFiPrompt();
      }
    });
    passwordBox.addEventListener("keyup", (keyEvent) => {
      if (keyEvent.key === "Enter") {
        saveWiFiSettings();
      } else if (keyEvent.key === "Escape") {
        closeWiFiPrompt();
      }
    });
  });
}

/////// guest Wi-Fi window ///////

const GUEST_WIFI_PROMPT_ID = 'wifi__prompt';

// Network menu, Access: the QR code and password for guest WiFi access
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
  // password text set separately (value is data, not markup)
  document.getElementById("wifi__pwdtxt").innerHTML = atob(SSIDpwd());
  onButtons(promptElement, () => promptElement.remove());
}

/////// password prompt ///////

const PASSWORD_PROMPT_ID = 'pass__prompt_win';
const PASSWORD_CANCEL_BUTTON_ID = 'pass__btn-cancel';
const PASSWORD_SEND_BUTTON_ID = 'pass__btn-send';

// Asks for a password and saves it to the file named, which the server side
// reads it from; afterPOSTActions() then sends the command that needs it.
async function getPassword(passwordFile) {
  try {
    removeDropdowns();
    const password = await passwordPrompt();
    if (password !== null && password !== "" && password !== undefined) {
      savePOST(passwordFile, [password], afterPOSTActions);
    }
  } catch (cancelled) {
    // user cancelled -- nothing to do
  }
}

// Resolves with the typed password and rejects when it was cancelled.
// Answers null, with no new window, when the prompt is already open.
function passwordPrompt() {
  if (document.getElementById(PASSWORD_PROMPT_ID)) {
    return null;
  }
  const promptElement = promptWindow(PASSWORD_PROMPT_ID, html`
    <div>Enter password:</div>
    <input id="pass__textbox" type="password">
    <div id="pass__prmbtnctr">
      ${gridButtonTemplate({
        buttonId: PASSWORD_CANCEL_BUTTON_ID,
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
  return new Promise(function (resolve, reject) {
    onButtons(promptElement, (button) => {
      if (button.id === PASSWORD_SEND_BUTTON_ID) {
        resolve(passwordBox.value);
      } else {
        reject(new Error("User cancelled"));
      }
      promptElement.remove();
    });
    passwordBox.addEventListener("keyup", function (keyEvent) {
      if (keyEvent.key === "Enter") {
        resolve(passwordBox.value);
        promptElement.remove();
      } else if (keyEvent.key === "Escape") {
        promptElement.remove();
        reject(new Error("User cancelled"));
      }
    });
  });
}
