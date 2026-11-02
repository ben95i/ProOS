// Automate - the amp rooms' volume controls: button template and handlers, subwoofer
// mode, sending a volume command and showing the level in the volume popup.

import { html, sendCmd, cleanString, isNumeric, mapNumber,
  gridButtonTemplate } from './utils.js';
import { removeOnFade } from './error.js';

//// volume popup ////

const VOLUME_POPUP_MS = 3000;
const VOLUME_AMP_OFF = -1;
const VOLUME_MUTED = 0;
let volumePopupTimer = null;

// drops a pending fade, for the page teardown
export function clearVolumePopupTimer() {
  if (volumePopupTimer) {
    clearTimeout(volumePopupTimer);
    volumePopupTimer = null;
  }
}

// Shows the amp's level at the top of the page. volumePercent is 0-100; 0 shows "Mute"
// and -1 shows "---" (the amp is off).
function showVolumePopup(volumePercent) {
  const windowId = "vol-popup";
  const textId = "vol-text";
  const gridId = "vol-grid";
  const barId = "vol-bar";
  let popupWindow = document.getElementById(windowId);
  if (!popupWindow) {
    popupWindow = document.createElement("div");
    popupWindow.classList.add("popup-alert");
    popupWindow.id = windowId;
    popupWindow.innerHTML = html`
      <div id="${textId}"></div>
      <div class="progress_grid" id="${gridId}">
        <div class="bar_progress" id="${barId}"></div>
      </div>
    `;
    removeOnFade(popupWindow);
    document.body.appendChild(popupWindow);
  }
  popupWindow.classList.remove("fade-out");
  const volumeText = document.getElementById(textId);
  const volumeGrid = document.getElementById(gridId);
  const volumeBar = document.getElementById(barId);
  switch (volumePercent) {
    case VOLUME_AMP_OFF:
      volumeGrid.style.display = "none";
      volumeText.innerHTML = "---";
      break;
    case VOLUME_MUTED:
      volumeGrid.style.display = "none";
      volumeText.innerHTML = "Mute";
      break;
    default: {
      volumeGrid.style.display = "block";
      const percentText = `${volumePercent}%`;
      volumeText.innerHTML = percentText;
      volumeBar.style.width = percentText;
    }
  }
  clearVolumePopupTimer();
  volumePopupTimer = setTimeout(() => {
    volumePopupTimer = null;
    // removeOnFade() takes the window off once the fade ends
    popupWindow.classList.add("fade-out");
  }, VOLUME_POPUP_MS);
}

//// volume controls ////

// the class a volume button carries while it controls the subwoofers
// the class a volume button carries while it controls the subwoofers
export const SUBWOOFER_CLASS = 'red_btn_menu';

// The three buttons every amp room draws. suffix ends the button's id, and command
// follows "vol" or "subvol" in what it sends.
const VOLUME_BUTTONS = [
  { suffix: 'voldwnbtn', command: 'dwn', icon: 'fad fa-volume-down fa-lg' },
  { suffix: 'volmutebtn', command: 'mute', icon: 'fad fa-volume-mute fa-lg' },
  { suffix: 'volupbtn', command: 'up', icon: 'fad fa-volume-up fa-lg' }
];

// the amp's 5% steps as the subwoofers name them
const SUBWOOFER_STEP_COMMANDS = {
  'upc': 'up_fast',
  'dwnc': 'dwn_fast'
};

// Presses are numbered and the newest answer drawn is kept, so an older answer landing
// late is dropped rather than showing a level the amp has left.
let volumeRequestSequence = 0;
let volumeShownSequence = 0;

// the id of one room's volume button: volumeButtonId('lr', 'volupbtn')
function volumeButtonId(roomKey, suffix) {
  return `${roomKey}-${suffix}`;
}

// Per room key: other buttons that send volume commands and go red in subwoofer mode
// (the living room's -5% and +5%).
const EXTRA_VOLUME_BUTTON_IDS = {};

// Names a room's extra volume buttons: registerVolumeButtons('lr', ['btn-vol-dwnc'])
export function registerVolumeButtons(roomKey, buttonIds) {
  EXTRA_VOLUME_BUTTON_IDS[roomKey] = buttonIds.slice();
}

// the ids of every volume button of a room
function volumeButtonIds(roomKey) {
  const buttonIds = VOLUME_BUTTONS.map(volumeButton => volumeButtonId(roomKey, volumeButton.suffix));
  const extraButtonIds = EXTRA_VOLUME_BUTTON_IDS[roomKey];
  if (extraButtonIds) {
    buttonIds.push(...extraButtonIds);
  }
  return buttonIds;
}

// the down / mute / up row of a room's grid
export function volumeButtonsTemplate(roomKey) {
  let buttonsMarkup = '';
  VOLUME_BUTTONS.forEach(volumeButton => {
    buttonsMarkup += gridButtonTemplate({
      buttonId: volumeButtonId(roomKey, volumeButton.suffix),
      icon: volumeButton.icon
    });
  });
  return buttonsMarkup;
}

// that row's handlers, in the shape registerListeners() takes
export function volumeButtonListeners(roomConfig) {
  const listeners = {};
  VOLUME_BUTTONS.forEach(volumeButton => {
    const buttonId = volumeButtonId(roomConfig.key, volumeButton.suffix);
    listeners[buttonId] = () => sendVolume(roomConfig, volumeButton.command);
  });
  return listeners;
}

// Are this room's volume buttons controlling the subwoofers? Kept as a class on the
// buttons, so the mode goes when the grid is redrawn.
export function subwooferVolumeEnabled(roomKey) {
  const firstButton = document.getElementById(volumeButtonId(roomKey, VOLUME_BUTTONS[0].suffix));
  if (firstButton) {
    return firstButton.classList.contains(SUBWOOFER_CLASS);
  }
  return false;
}

// turns this room's volume buttons into subwoofer controls, or back
export function subwooferVolumeMode(roomKey, enable) {
  volumeButtonIds(roomKey).forEach(buttonId => {
    const button = document.getElementById(buttonId);
    if (button) {
      button.classList.toggle(SUBWOOFER_CLASS, enable === true);
    }
  });
}

// Sends one volume command: 'up', 'dwn', 'mute', 'upc', 'dwnc' or 'display'. In subwoofer
// mode it goes to the subwoofers with nothing shown; they have no 'display'.
export function sendVolume(roomConfig, command) {
  if (subwooferVolumeEnabled(roomConfig.key) === true) {
    if (command === 'display') {
      return;
    }
    let subwooferCommand = command;
    if (SUBWOOFER_STEP_COMMANDS[command]) {
      subwooferCommand = SUBWOOFER_STEP_COMMANDS[command];
    }
    sendCmd('main', roomConfig.voltarget, 'sub' + subwooferCommand);
    return;
  }
  setAmpVolume(command, roomConfig.voltarget, roomConfig.volmax);
}

// Sends the command and shows the level it answers as a percentage; an empty answer
// means the amp is off.
function setAmpVolume(command, device, maximumVolume) {
  volumeRequestSequence += 1;
  const requestSequence = volumeRequestSequence;
  sendCmd('main', device, 'vol' + command).then((data) => {
    // the answer to a later press is already on screen
    if (requestSequence < volumeShownSequence) {
      return;
    }
    const ampVolume = cleanString(data);
    if (isNumeric(ampVolume) === true) {
      const scaledPercent = mapNumber(Number(ampVolume), 0, maximumVolume, 0, 100);
      const displayPercent = Math.round(scaledPercent);
      volumeShownSequence = requestSequence;
      showVolumePopup(displayPercent);
    } else if (ampVolume == "") {
      volumeShownSequence = requestSequence;
      showVolumePopup(VOLUME_AMP_OFF);
    }
  });
}
