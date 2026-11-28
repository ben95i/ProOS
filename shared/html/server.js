// Automate - Server window: the options window and its panels (the server's main and
// regions, or a Pi's one), opened by the Server button or a Pi's System row. A panel's
// command runs at once and its output streams into the command output window
// (/api/svrcmd), which goes back to the panel it came from once closed; the Archive's
// attach asks for its password first, over the options window. Also the windows the
// panels open: guest Wi-Fi, a Pi's Wi-Fi setup, and the LAN Scanner and upload panels.
// The main panel's Backup button opens the backup wizard.

import { classDisplay, base64URLSafeEncode, html, savePOST, promptWindow, onButtons,
  setDisplayById, registerListeners, gridButtonTemplate, dropdownButtonTemplate,
  textBoxTemplate, GoToExtPage, onDemandModule } from './utils.js';
import { removeDropdowns, openDropdown } from './dropdowns.js';
import { HttpError, reportError, reportException, reportNotConnected,
  showPopup } from './error.js';
import { EXT_PAGES } from './opts.js';
import Uploader from './upload.js';
import { SSIDpwd } from './wifi.js';

// the Archive region's gocryptfs passfile, ram/fusearch.save.txt on the host
const ARCHIVE_PASSWORD_FILE = 'fusearch.save';
const OPTIONS_WINDOW_CLASS = 'svropt__prompt';
const OPTIONS_PANEL_CONTAINER_ID = 'svropt-panel-container';
// every option panel button's size
const OPTION_BUTTON_CLASS = 'svropt_button';

// The commands the panels run: each one's output window title and, for a command whose
// output shares a file, that file (ram/<file>.out.save.txt; any other command's is
// <command>.out.save.txt). These shared files are the ones the host's file-monitor.sh
// (outputRoutes) writes the same commands to.
const REGIONS_OUTPUT_FILE = 'regions';
const SERVER_COMMANDS = {
  startatalk:        { title: 'AFP Start' },
  stopatalk:         { title: 'AFP Stop' },
  startcifs:         { title: 'CIFS Start' },
  stopcifs:          { title: 'CIFS Stop' },
  startxana:         { title: 'Xana Start' },
  stopxana:          { title: 'Xana Stop' },
  startunifi:        { title: 'UniFi Start' },
  stopunifi:         { title: 'UniFi Stop' },
  syslog:            { title: 'System Status' },
  rebootrouter:      { title: 'Router Reboot' },
  mnt_arch_region:   { title: 'Archive Attach', outputFile: REGIONS_OUTPUT_FILE },
  unmnt_arch_region: { title: 'Archive Detach', outputFile: REGIONS_OUTPUT_FILE },
  mnt_snap_region:   { title: 'Snapshots Attach', outputFile: REGIONS_OUTPUT_FILE },
  unmnt_snap_region: { title: 'Snapshots Detach', outputFile: REGIONS_OUTPUT_FILE },
  mnt_ram_region:    { title: 'RAM Disk Attach', outputFile: REGIONS_OUTPUT_FILE },
  unmnt_ram_region:  { title: 'RAM Disk Detach', outputFile: REGIONS_OUTPUT_FILE },
  unmnt_all:         { title: 'Detach All', outputFile: REGIONS_OUTPUT_FILE },
  pve_listsnaps:     { title: 'Snapshots List' },
  client:            { title: 'Client Mode' },
  apd:               { title: 'Hotspot Mode' },
  wifiscan:          { title: 'WiFi Scanner' },
  showwpa:           { title: 'Network' },
  temp:              { title: 'Temperature' },
  resetbt:           { title: 'Bluetooth Reset' },
  confwpa:           { title: 'WiFi Setup' }
};

// the command output window
const OUTPUT_WINDOW_ID = 'svrout__prompt';
const OUTPUT_TITLE_ID = 'svrout__title';
const OUTPUT_BOX_ID = 'svrout__box';
const OUTPUT_BACK_BUTTON_ID = 'svrout__btn-back';
const OUTPUT_CLOSE_BUTTON_ID = 'svrout__btn-close';
const OUTPUT_HIDDEN_CLASS = 'is-hidden';
const OUTPUT_PATH = 'api/svrcmd';
// the box's line while nothing has been written into it
const OUTPUT_STARTING_TEXT = 'Starting...';
const OUTPUT_RUNNING_TEXT = 'Running...';
const OUTPUT_EMPTY_TEXT = 'Finished with no output.';
const OUTPUT_CUT_SHORT = 'The connection closed before the command finished.';
// within this many pixels of the box's end, new output keeps it on its newest line
const OUTPUT_END_SLACK = 24;
// a run's outcome
const RUN_RUNNING = 'running';
const RUN_DONE = 'done';
const RUN_FAILED = 'failed';
// the bar over the options while the output window is hidden: per outcome, its icon,
// the word after the command's name and its class
const RUNNING_BAR_ROW_ID = 'svropt-running';
const RUNNING_BAR_BUTTON_ID = 'svropt__btn-running';
const RUNNING_BAR_STATES = {
  [RUN_RUNNING]: { icon: 'fad fa-terminal', text: 'Running', className: 'is-running' },
  [RUN_DONE]:    { icon: 'fas fa-check', text: 'Finished', className: 'is-done' },
  [RUN_FAILED]:  { icon: 'fas fa-exclamation-triangle', text: 'Failed', className: 'is-failed' }
};

// the LAN Scanner (Network > LAN Scanner) and the backup wizard (Backup), each loaded by
// its first use; main.js preloads them
export const loadLanScanner = onDemandModule(() => import('./lanscan.js'), 'the LAN scanner');
export const loadBackupWizard = onDemandModule(() => import('./backup.js'), 'the backup wizard');

// the panel the options window last showed, and what it does once drawn (the LAN
// Scanner's scan, the upload's rows), which the output window's Back goes back to
let lastPanelTemplate = svrOptMainTemplate;
let lastPanelAfterDraw = null;

// the command output window, shown or hidden: its elements, its stream's AbortController,
// the command's title and outcome, and whether it has ended or is hidden; null once closed
let outputRun = null;

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

// handlers for btn-svr-<command> buttons, which run their command
function commandButtons(commands) {
  const listeners = {};
  commands.forEach(command => {
    listeners[`btn-svr-${command}`] = () => runServerCommand(command);
  });
  return listeners;
}

// The options window: the bar for a hidden command output window over the container for
// the server's panels and the Archive password over them, or a Pi's one panel.
export function serverOptionsTemplate(f) {
  return `
<div class="svropt__prompt prompt_win">
  <div class="svropt__running-row ${OUTPUT_HIDDEN_CLASS}" id="${RUNNING_BAR_ROW_ID}"></div>
  ${f.isServer ? `<div id="svropt-panel-container"></div>${archivePasswordTemplate()}` : piOptionsTemplate()}
</div>`;
}

// the server's main panel
function svrOptMainTemplate() {
  return `
<div class="svropt__panel svropt__main">
  ${optionMenu('atalk', 'fab fa-apple', 'AFP', startStopRows('atalk'))}
  ${optionMenu('cifs', 'fad fa-save', 'CIFS', startStopRows('cifs'))}
  ${optionMenu('xana', 'fab fa-linux', 'Xana', startStopRows('xana'))}
  ${optionButton('btn-open-regions', 'fad fa-th', 'Regions')}
  ${optionMenu('system', 'fad fa-microchip', 'System', `
      <a id="btn-ext-system"><i class="fad fa-link"></i> Open</a>
      <a id="btn-svr-syslog"><i class="fad fa-tachometer"></i> Status</a>`)}
  ${optionButton('btn-open-backup', 'fad fa-sync', 'Backup')}
  ${optionMenu('router', 'fad fa-ethernet', 'Router', `
      <a id="btn-ext-router"><i class="fad fa-link"></i> Open</a>
      <a id="btn-ext-edgeswitch"><i class="fad fa-sitemap"></i> Switch</a>
      <a id="btn-svr-rebootrouter"><i class="fad fa-redo"></i> Reboot</a>`)}
  ${optionButton('btn-svr-netscan', 'fad fa-chart-network', 'Scanner')}
  ${optionMenu('unifi', 'fad fa-router', 'UniFi', `
      <a id="btn-ext-unifi"><i class="fad fa-link"></i> Open</a>${startStopRows('unifi')}`)}
  ${optionButton('btn-close-svr-opt1', 'fas fa-times', '')}
  ${optionButton('btn-upload', 'fad fa-cloud-upload', 'Upload')}
  ${optionButton('btn-wifi-prompt', 'fad fa-wifi', 'Wi-Fi')}
</div>`;
}
registerListeners({
  'btn-upload':           () => showUpload(),
  ...menuOpeners(['atalk', 'cifs', 'unifi', 'xana', 'system', 'netwrk', 'router']),
  ...commandButtons(['startatalk', 'stopatalk', 'startcifs', 'stopcifs', 'startunifi',
    'stopunifi', 'startxana', 'stopxana', 'syslog', 'rebootrouter']),
  'btn-svr-netscan':      () => openLanScanner(),
  'btn-ext-unifi':        () => GoToExtPage(EXT_PAGES.unifi),
  'btn-ext-system':       () => GoToExtPage(EXT_PAGES.proxmox),
  'btn-open-backup':      () => openBackupWizard(),
  'btn-open-regions':     () => openServerPanel(svrOptRegionsTemplate),
  'btn-wifi-prompt':      () => wifiPrompt(),
  'btn-ext-edgeswitch':   () => GoToExtPage(EXT_PAGES.edgeswitch),
  'btn-close-svr-opt1':   () => closeServerOptions(),
  'btn-ext-router':       () => GoToExtPage(EXT_PAGES.router)
});

// the server's regions panel: archive, snapshot and RAM disk mounts
function svrOptRegionsTemplate() {
  return `
<div class="svropt__panel svropt__regions">
  ${optionMenu('archive', '', 'Archive', `
      <a id="btn-pwd-fusearch"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-svr-unmnt_arch_region"><i class="fad fa-eject"></i> Detach</a>`)}
  ${optionMenu('snapshots', '', 'Snapshots', `
      <a id="btn-svr-mnt_snap_region"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-svr-unmnt_snap_region"><i class="fad fa-eject"></i> Detach</a>
      <a id="btn-svr-pve_listsnaps"><i class="fad fa-list"></i> List</a>`)}
  ${optionMenu('ramdisk', '', 'RAM Disk', `
      <a id="btn-svr-mnt_ram_region"><i class="fad fa-link"></i> Attach</a>
      <a id="btn-svr-unmnt_ram_region"><i class="fad fa-eject"></i> Detach</a>`)}
  ${optionButton('btn-close-svr-opt3', 'fas fa-times', '')}
  ${optionButton('btn-open-svr-opt2', 'fad fa-arrow-left', '')}
  ${optionButton('btn-svr-unmnt_all', '', 'Detach All', 'dark_btn')}
</div>`;
}
registerListeners({
  ...menuOpeners(['archive', 'snapshots', 'ramdisk']),
  ...commandButtons(['unmnt_arch_region', 'mnt_snap_region', 'unmnt_snap_region',
    'pve_listsnaps', 'mnt_ram_region', 'unmnt_ram_region', 'unmnt_all']),
  'btn-pwd-fusearch':       () => getPassword(ARCHIVE_PASSWORD_FILE),
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
  ${optionButton('btn-close-svr-opt4', 'fas fa-times', '')}
  ${optionButton('btn-svr-resetbt', 'fab fa-bluetooth', 'Reset')}
</div>`;
}
registerListeners({
  ...menuOpeners(['piwifi']),
  ...commandButtons(['client', 'apd', 'wifiscan', 'showwpa', 'temp', 'resetbt']),
  'btn-show-piwifiprompt': () => showPiWiFiPrompt(),
  'btn-close-svr-opt4':    () => closeServerOptions()
});

// the command output window: the command's name over a Pico read-out's box (as the
// thermostat's status), then Close and Back, as the bathroom settings' log view
function outputWindowTemplate() {
  return `
  <div class="popup-ctn svrout__log">
    <div class="btn-group-label svrout__title"><i class="fad fa-terminal"></i><span id="${OUTPUT_TITLE_ID}"></span></div>
    <textarea readonly class="scroll-bar-style pico-logbox" id="${OUTPUT_BOX_ID}" spellcheck="false"
      placeholder="${OUTPUT_STARTING_TEXT}"></textarea>
    <div class="popup-actions svrout__actions">
      ${gridButtonTemplate({
        buttonId: OUTPUT_CLOSE_BUTTON_ID,
        icon: 'fas fa-times',
        buttonClass: 'svrout__btn'
      })}
      ${gridButtonTemplate({
        buttonId: OUTPUT_BACK_BUTTON_ID,
        icon: 'fad fa-arrow-left',
        buttonClass: 'svrout__btn'
      })}
    </div>
  </div>`;
}

//// options window ////

// Server button, or a Pi's System row: the options window on the server's main panel, or
// the Pi's panel, with the bar for a hidden command output window over it.
export function openServerOptions() {
  openServerPanel(svrOptMainTemplate);
}

// Puts every Server window away: closes the options window and hides the command output
// window, whose command runs on and is opened again from the options' bar; main.js does
// this as it opens the camera.
export function closeServerWindows() {
  if (outputRun !== null && !outputRun.hidden) {
    hideCommandOutput();
  }
  closeServerOptions();
}

// Opens the options window on one of the server's panels; a Pi's panel is already in it.
// afterDraw(panelContainer) runs once the panel is drawn, as the LAN Scanner and upload
// panels bind themselves. openServerPanel(svrOptRegionsTemplate)
function openServerPanel(panelTemplate, afterDraw = null) {
  closeServerOptions();
  lastPanelTemplate = panelTemplate;
  lastPanelAfterDraw = afterDraw;
  const panelContainer = document.getElementById(OPTIONS_PANEL_CONTAINER_ID);
  if (panelContainer) {
    panelContainer.innerHTML = panelTemplate();
  }
  drawRunningBar();
  classDisplay(OPTIONS_WINDOW_CLASS, "block");
  if (panelContainer && afterDraw) {
    afterDraw(panelContainer);
  }
}

// Opens the panel the options window last showed again.
function openLastPanel() {
  openServerPanel(lastPanelTemplate, lastPanelAfterDraw);
}

// Closes the options window and its menus, and removes the panel, stopping the LAN
// Scanner's scan when it is the one shown; an upload goes on.
function closeServerOptions() {
  removeDropdowns();
  closePasswordOverlay();
  const lanScanModule = loadLanScanner.loaded();
  if (lanScanModule) {
    lanScanModule.closeLanScanner();
  }
  classDisplay(OPTIONS_WINDOW_CLASS, "none");
  const panelContainer = document.getElementById(OPTIONS_PANEL_CONTAINER_ID);
  if (panelContainer) {
    panelContainer.innerHTML = "";
  }
}

// Main panel, Backup: closes the options window and opens the backup wizard (backup.js),
// whose Back comes back to the main panel.
async function openBackupWizard() {
  closeServerOptions();
  const backupModule = await loadBackupWizard();
  if (backupModule) {
    backupModule.openBackupWizard({ onBack: () => openServerPanel(svrOptMainTemplate) });
  }
}

// What the LAN Scanner and upload panels' Close and Back do: close the options window, or
// go back to the main panel.
const PANEL_ACTIONS = {
  onClose: () => closeServerOptions(),
  onBack: () => openServerPanel(svrOptMainTemplate)
};

// Network menu, LAN Scanner: the scanner's panel (lanscan.js), which starts a scan.
async function openLanScanner() {
  removeDropdowns();
  const lanScanModule = await loadLanScanner();
  if (lanScanModule) {
    openServerPanel(lanScanModule.lanScannerTemplate,
      () => lanScanModule.openLanScanner(PANEL_ACTIONS));
  }
}

// Upload button: opens the file dialog, whose files' progress shows as a panel (upload.js);
// while an upload runs, its panel instead.
function showUpload() {
  removeDropdowns();
  if (!App.uploader) {
    App.uploader = new Uploader({
      showPanel: (panelTemplate, afterDraw) => openServerPanel(panelTemplate, afterDraw),
      ...PANEL_ACTIONS
    });
  }
  App.uploader.pick();
}

//// command output window ////

// the command each password file is saved for
const PASSWORD_COMMANDS = { [ARCHIVE_PASSWORD_FILE]: 'mnt_arch_region' };

// A password was saved: runs the command that uses it.
function afterPOSTActions(passwordFile) {
  runServerCommand(PASSWORD_COMMANDS[passwordFile]);
}

// True, with a notice, while a command has not finished: one runs at a time, so a panel
// starts no other (nor asks for its password) until then.
function commandStillRunning() {
  const isRunning = outputRun !== null && !outputRun.finished;
  if (isRunning) {
    showPopup(`[server] ${outputRun.title} is still running.`);
  }
  return isRunning;
}

// Runs a panel's command: closes the options window and opens the output window, which
// shows the command's output as it runs. commandArg is the Pi Wi-Fi setup's settings.
// Refused while another command runs; a finished one's window is replaced.
// runServerCommand('mnt_snap_region'), runServerCommand('confwpa', encodedSettings)
function runServerCommand(command, commandArg = '') {
  if (commandStillRunning()) {
    return;
  }
  closeServerOptions();
  closeCommandOutput();
  const commandSpec = SERVER_COMMANDS[command];
  const win = promptWindow(OUTPUT_WINDOW_ID, outputWindowTemplate());
  const run = {
    win: win,
    box: document.getElementById(OUTPUT_BOX_ID),
    abortController: new AbortController(),
    title: commandSpec.title,
    what: `running ${commandSpec.title}`,
    // running, then done or failed
    outcome: RUN_RUNNING,
    finished: false,
    hidden: false
  };
  document.getElementById(OUTPUT_TITLE_ID).textContent = commandSpec.title;
  outputRun = run;
  onButtons(win, (button) => {
    if (button.id === OUTPUT_CLOSE_BUTTON_ID) {
      putAwayCommandOutput();
    }
    if (button.id === OUTPUT_BACK_BUTTON_ID) {
      putAwayCommandOutput();
      openLastPanel();
    }
  });
  followCommandOutput(run, command, commandArg, outputFileName(command));
}

// A command's window title: 'mnt_ram_region' -> 'RAM Disk Attach'; one the page does not
// list goes by its name.
function commandTitle(command) {
  const commandSpec = SERVER_COMMANDS[command];
  if (commandSpec) {
    return commandSpec.title;
  }
  return command;
}

// A command's output file, as /api/svrcmd names it: 'mnt_ram_region' -> 'regions.out.save'
function outputFileName(command) {
  let fileName = command;
  const commandSpec = SERVER_COMMANDS[command];
  if (commandSpec.outputFile) {
    fileName = commandSpec.outputFile;
  }
  return `${fileName}.out.save`;
}

// the stream's address: http://automate.home/api/svrcmd?cmd=syslog&out=syslog.out.save&arg=
function outputStreamURL(command, commandArg, outputFile) {
  const queryText = new URLSearchParams({ cmd: command, out: outputFile, arg: commandArg });
  return `${location.protocol}//${location.hostname}/${OUTPUT_PATH}?${queryText}`;
}

// Starts the command and writes its output into the window as each event arrives (one
// JSON event per line, see shared/nodeapi/svrcmd.js), shown or hidden, until it ends or
// fails.
async function followCommandOutput(run, command, commandArg, outputFile) {
  let finished = false;
  try {
    const response = await fetch(outputStreamURL(command, commandArg, outputFile), {
      cache: 'no-store',
      signal: run.abortController.signal
    });
    if (!response.ok || !response.body) {
      const answerText = await response.text();
      throw new HttpError(response.status, answerText);
    }
    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let pending = '';
    while (!finished) {
      const { value, done } = await reader.read();
      if (done) {
        break;
      }
      pending += decoder.decode(value, { stream: true });
      const lines = pending.split('\n');
      // the last piece is a line still arriving
      pending = lines.pop();
      for (const line of lines) {
        if (line.trim() !== '' && readOutputLine(run, line)) {
          finished = true;
        }
      }
    }
    if (!finished) {
      run.outcome = RUN_FAILED;
      reportNotConnected(run.what, OUTPUT_CUT_SHORT);
      appendOutput(run, OUTPUT_CUT_SHORT + '\n');
    }
  } catch (error) {
    // replaced by a newer command
    if (error.name === 'AbortError' || run !== outputRun) {
      return;
    }
    run.outcome = RUN_FAILED;
    reportError(run.what, error);
    appendOutput(run, `${error.message}\n`);
  }
  showCommandFinished(run);
}

// Acts on one event of the stream; true once it is the last ('done' or 'error').
// A line that is not JSON throws, ending the read as an exception.
function readOutputLine(run, line) {
  const outputEvent = JSON.parse(line);
  switch (outputEvent.type) {
    case 'waiting':
      // another command holds the output file, on this page or another user's
      run.box.placeholder = `Waiting for ${commandTitle(outputEvent.command)} to finish...`;
      return false;
    case 'start':
      run.box.placeholder = OUTPUT_RUNNING_TEXT;
      return false;
    case 'text':
      appendOutput(run, outputEvent.text);
      return false;
    case 'done':
      run.outcome = RUN_DONE;
      if (outputEvent.code !== 0) {
        run.outcome = RUN_FAILED;
        appendOutput(run, `(exit code ${outputEvent.code})\n`);
      }
      return true;
    case 'error':
      // "syslog did not start"
      run.outcome = RUN_FAILED;
      reportException(run.what, outputEvent.message);
      appendOutput(run, `${outputEvent.message}\n`);
      return true;
    default:
      return false;
  }
}

// Adds text to the box, kept on its newest line unless scrolled up from it.
function appendOutput(run, text) {
  if (run !== outputRun) {
    return;
  }
  const box = run.box;
  const hiddenBelow = box.scrollHeight - box.scrollTop - box.clientHeight;
  const atEnd = hiddenBelow <= OUTPUT_END_SLACK;
  box.value += text;
  if (atEnd) {
    box.scrollTop = box.scrollHeight;
  }
}

// The command has ended: the bar over the options shows how it went, and a hidden one
// that went well says so (a failure has already been reported).
function showCommandFinished(run) {
  if (run !== outputRun) {
    return;
  }
  run.finished = true;
  run.box.placeholder = OUTPUT_EMPTY_TEXT;
  drawRunningBar();
  if (run.hidden && run.outcome === RUN_DONE) {
    showPopup(`[server] ${run.title} finished.`);
  }
}

// Close or Back: a finished command's window is closed; one still running is hidden,
// still followed, and the options' bar opens it again. Back then opens the last panel.
function putAwayCommandOutput() {
  if (outputRun === null) {
    return;
  }
  if (outputRun.finished) {
    closeCommandOutput();
    return;
  }
  hideCommandOutput();
}

// Hides the output window while its command runs on; the options' bar opens it again.
function hideCommandOutput() {
  outputRun.hidden = true;
  outputRun.win.style.display = 'none';
}

// The bar over the options: shows the hidden output window again, on its newest line.
function restoreCommandOutput() {
  if (outputRun === null) {
    return;
  }
  closeServerOptions();
  outputRun.hidden = false;
  outputRun.win.style.display = '';
  outputRun.box.scrollTop = outputRun.box.scrollHeight;
}

// Removes the output window, if any; only a finished command's, or one being replaced.
function closeCommandOutput() {
  if (outputRun === null) {
    return;
  }
  const run = outputRun;
  outputRun = null;
  run.abortController.abort();
  run.win.remove();
  drawRunningBar();
}

// Draws the bar over the options for a hidden output window, or clears it: the command's
// name with its state, running, finished or failed, and a press opens the window again.
function drawRunningBar() {
  const barRow = document.getElementById(RUNNING_BAR_ROW_ID);
  if (!barRow) {
    return;
  }
  const showBar = outputRun !== null && outputRun.hidden;
  if (!showBar) {
    barRow.innerHTML = '';
    barRow.classList.add(OUTPUT_HIDDEN_CLASS);
    return;
  }
  const barState = RUNNING_BAR_STATES[outputRun.outcome];
  barRow.innerHTML = gridButtonTemplate({
    buttonId: RUNNING_BAR_BUTTON_ID,
    icon: barState.icon,
    label: `${outputRun.title} · ${barState.text}`,
    buttonClass: `svropt__running ${barState.className}`
  });
  barRow.classList.remove(OUTPUT_HIDDEN_CLASS);
}
registerListeners({
  'svropt__btn-running': () => restoreCommandOutput()
});

/////// Pi Wi-Fi Window ///////

const PI_WIFI_PROMPT_ID = 'pinet__prompt';

// Pi options, Setup WiFi: asks for a network and password and sends them to the Pi.
async function showPiWiFiPrompt() {
  removeDropdowns();
  if (document.getElementById(PI_WIFI_PROMPT_ID) || commandStillRunning()) {
    return;
  }
  const encodedSettings = await piWiFiPrompt(PI_WIFI_PROMPT_ID);
  if (encodedSettings !== null && encodedSettings !== "") {
    runServerCommand('confwpa', encodedSettings);
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
      ${textBoxTemplate({ boxId: 'pinet__ssidbox' })}
      <div>Password:</div>
      ${textBoxTemplate({ boxId: 'pinet__passbox', type: 'password' })}
      <div id="pinet__btnrow">
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
      </div>
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
      icon: 'fas fa-times'
    })}
  `);
  // set separately: the value is data, not markup
  document.getElementById("wifi__pwdtxt").innerHTML = atob(SSIDpwd());
  onButtons(promptElement, () => promptElement.remove());
}

/////// Archive password ///////

// The Archive password, drawn over the options window as the lists window asks for its
// password (lists.js), with the lists window's own dialog rules (lists.css): a dimmed
// cover, the heading, one password box, a line for a missing password, then Cancel and
// Unlock.
const PASSWORD_OVERLAY_ID = 'svropt__pwd';
const PASSWORD_BOX_ID = 'svropt__pwd-box';
const PASSWORD_ERROR_ID = 'svropt__pwd-err';
const PASSWORD_CANCEL_BUTTON_ID = 'svropt__btn-pwd-cancel';
const PASSWORD_UNLOCK_BUTTON_ID = 'svropt__btn-pwd-unlock';
const PASSWORD_MISSING_TEXT = 'enter the password';

// the password file the overlay is asking for (ARCHIVE_PASSWORD_FILE), or null while shut
let pendingPasswordFile = null;

// the overlay's markup, in the server's options window from the start and hidden
function archivePasswordTemplate() {
  return `
  <div class="list__overlay" id="${PASSWORD_OVERLAY_ID}">
    <div class="list__dialog">
      <div class="list__dialog-msg">Enter Archive password</div>
      ${textBoxTemplate({ boxId: PASSWORD_BOX_ID, type: 'password', placeholder: 'Password' })}
      <div class="list__pwerr" id="${PASSWORD_ERROR_ID}"></div>
      <div class="list__dialog-btns">
        ${gridButtonTemplate({
          buttonId: PASSWORD_CANCEL_BUTTON_ID,
          label: 'Cancel',
          buttonClass: 'list__dlg-btn'
        })}
        ${gridButtonTemplate({
          buttonId: PASSWORD_UNLOCK_BUTTON_ID,
          label: 'Unlock',
          buttonClass: 'list__dlg-btn'
        })}
      </div>
    </div>
  </div>`;
}
registerListeners({
  'svropt__btn-pwd-cancel': () => closePasswordOverlay(),
  'svropt__btn-pwd-unlock': () => submitPassword()
});

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

// Regions > Archive > Attach: asks for the password over the options window; not while
// another command runs. getPassword(ARCHIVE_PASSWORD_FILE)
function getPassword(passwordFile) {
  removeDropdowns();
  if (commandStillRunning()) {
    return;
  }
  const passwordBox = document.getElementById(PASSWORD_BOX_ID);
  pendingPasswordFile = passwordFile;
  passwordBox.value = '';
  // set, not added, so showing the overlay again leaves one handler
  passwordBox.onkeyup = (keyEvent) => {
    if (keyEvent.key === 'Enter') {
      submitPassword();
    } else if (keyEvent.key === 'Escape') {
      closePasswordOverlay();
    }
  };
  showPasswordError('');
  setDisplayById(PASSWORD_OVERLAY_ID, 'flex');
  passwordBox.focus();
}

// Unlock: saves the password to the file the host reads, which then runs the command that
// uses it; an empty box says so instead.
function submitPassword() {
  const passwordBox = document.getElementById(PASSWORD_BOX_ID);
  const password = passwordBox.value;
  if (pendingPasswordFile === null) {
    return;
  }
  if (password === '') {
    showPasswordError(PASSWORD_MISSING_TEXT);
    passwordBox.focus();
    return;
  }
  const passwordFile = pendingPasswordFile;
  closePasswordOverlay();
  savePOST(passwordFile, password, afterPOSTActions);
}

// writes the overlay's error line, or clears it with ''
function showPasswordError(text) {
  const errorLine = document.getElementById(PASSWORD_ERROR_ID);
  if (errorLine) {
    errorLine.innerText = text;
  }
}

// Hides the overlay, leaving no password in its box.
function closePasswordOverlay() {
  pendingPasswordFile = null;
  const passwordBox = document.getElementById(PASSWORD_BOX_ID);
  if (passwordBox) {
    passwordBox.value = '';
  }
  showPasswordError('');
  setDisplayById(PASSWORD_OVERLAY_ID, 'none');
}
