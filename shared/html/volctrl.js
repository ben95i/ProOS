// Automate Website - volume controls
//
// The volume buttons the rooms with an amp share (the living room and the
// bedroom): their template and handlers, the subwoofer mode, sending a
// volume command and showing the level it comes back with in the volume
// popup. Sits just above utils.js, so the room modules and dropdowns.js can
// import it.

import { html, sendCmd, cleanString, isNumeric, mapNumber,
  gridButtonTemplate } from './utils.js';

//// volume popup ////

const VOLUME_POPUP_MS = 3000;  // how long the level stays up before it fades
const VOLUME_AMP_OFF = -1;     // showVolumePopup() draws this level as "---"
const VOLUME_MUTED = 0;        // and this one as "Mute"
let volumePopupTimer = null;

// lets the page teardown drop a pending fade
export function clearVolumePopupTimer() {
  if (volumePopupTimer) {
    clearTimeout(volumePopupTimer);
    volumePopupTimer = null;
  }
}

// Shows the amp's level at the top of the page for three seconds.
// volumePercent is 0-100; 0 shows "Mute" and -1 shows "---" (the amp is off).
export function showVolumePopup(volumePercent) {
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
    // Attached once, like the message popup: a volume press arriving mid-fade
    // cancels the animation, so a per-fade {once:true} listener would never
    // fire and never unregister.
    popupWindow.addEventListener("animationend", () => {
      if (popupWindow.classList.contains("fade-out") && popupWindow.isConnected) {
        popupWindow.remove();
      }
    });
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
    // the listener above removes the window once the fade ends
    popupWindow.classList.add("fade-out");
  }, VOLUME_POPUP_MS);
}

//// volume controls ////
//
// Shared by every room with an amp (the living room and the bedroom). A room
// hands in its own config -- { key, voltarget, volmax } -- so nothing here
// has to know which rooms exist.

// the class a volume button carries while it controls the subwoofers
export const SUBWOOFER_CLASS = 'red_btn_menu';

// The three volume buttons every amp room draws, left to right. `suffix` is
// the end of the button's id (after the room key), `command` what follows
// "vol" or "subvol" in the command it sends.
const VOLUME_BUTTONS = [
  { suffix: 'voldwnbtn', command: 'dwn', icon: 'fad fa-volume-down fa-lg' },
  { suffix: 'volmutebtn', command: 'mute', icon: 'fad fa-volume-mute fa-lg' },
  { suffix: 'volupbtn', command: 'up', icon: 'fad fa-volume-up fa-lg' }
];

// The amp's 5% steps ('upc', 'dwnc') as the subwoofers know them: the API
// has no subupc / subdwnc, its big subwoofer steps are subup_fast and
// subdwn_fast.
const SUBWOOFER_STEP_COMMANDS = {
  'upc': 'up_fast',
  'dwnc': 'dwn_fast'
};

// Amp commands the subwoofers have no counterpart for. The API has no
// subdisplay, so in subwoofer mode the Volume button sends nothing at all
// rather than a request that can only come back as an invalid command.
const SUBWOOFER_UNSUPPORTED_COMMANDS = ['display'];

// what the volume popup counts in, whatever scale the amp itself uses
const VOLUME_PERCENT_MIN = 0;
const VOLUME_PERCENT_MAX = 100;

// Volume presses are numbered as they go out, and the number of the newest
// answer drawn is kept. Two answers can come back in the other order to the
// presses that asked for them; the older one is then dropped, where it used
// to leave the popup showing a level the amp had already moved on from.
let volumeRequestSequence = 0;
let volumeShownSequence = 0;

// the id of one volume button of one room: volumeButtonId('lr', 'volupbtn')
function volumeButtonId(roomKey, suffix) {
  return `${roomKey}-${suffix}`;
}

// Buttons a room draws besides the three above that also send volume
// commands (the living room's -5% and +5%), per room key. They turn red with
// the others in subwoofer mode, as they control the subwoofers then too.
const EXTRA_VOLUME_BUTTON_IDS = {};

// Names a room's extra volume buttons, by id. Called once by the room's
// module while it loads: registerVolumeButtons('lr', ['btn-vol-dwnc'])
export function registerVolumeButtons(roomKey, buttonIds) {
  EXTRA_VOLUME_BUTTON_IDS[roomKey] = buttonIds.slice();
}

// The ids of every volume button of a room: its three standard buttons
// first, then any extra ones it registered.
function volumeButtonIds(roomKey) {
  const buttonIds = [];
  VOLUME_BUTTONS.forEach(volumeButton => {
    buttonIds.push(volumeButtonId(roomKey, volumeButton.suffix));
  });
  if (Object.prototype.hasOwnProperty.call(EXTRA_VOLUME_BUTTON_IDS, roomKey)) {
    EXTRA_VOLUME_BUTTON_IDS[roomKey].forEach(extraButtonId => {
      buttonIds.push(extraButtonId);
    });
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

// the handlers for that row, in the shape registerListeners() takes
export function volumeButtonListeners(roomConfig) {
  const listeners = {};
  VOLUME_BUTTONS.forEach(volumeButton => {
    const buttonId = volumeButtonId(roomConfig.key, volumeButton.suffix);
    listeners[buttonId] = () => sendVolume(roomConfig, volumeButton.command);
  });
  return listeners;
}

// Are this room's volume buttons controlling the subwoofers? The mode is kept
// as a class on the buttons themselves, so it goes when the grid is redrawn.
export function subwooferVolumeEnabled(roomKey) {
  const buttonIds = volumeButtonIds(roomKey);
  if (buttonIds.length === 0) {
    return false;
  }
  const firstButton = document.getElementById(buttonIds[0]);
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

// Sends one volume command for a room: 'up', 'dwn', 'mute', 'upc', 'dwnc' or
// 'display'. In subwoofer mode it goes to the subwoofers and nothing is
// shown, and 'display', which they do not have, is not sent.
export function sendVolume(roomConfig, command) {
  if (subwooferVolumeEnabled(roomConfig.key) === true) {
    if (SUBWOOFER_UNSUPPORTED_COMMANDS.includes(command)) {
      return;
    }
    // the subwoofers name their big steps differently to the amp
    let subwooferCommand = command;
    if (Object.prototype.hasOwnProperty.call(SUBWOOFER_STEP_COMMANDS, command)) {
      subwooferCommand = SUBWOOFER_STEP_COMMANDS[command];
    }
    sendCmd('main', roomConfig.voltarget, 'sub' + subwooferCommand);
    return;
  }
  setAmpVolume(command, roomConfig.voltarget, roomConfig.volmax);
}

// Reads the amp back after the command and shows the level as a percentage.
// An empty answer means the amp is off, which the popup shows as "---".
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
      let displayPercent = Number(ampVolume);
      if (maximumVolume != VOLUME_PERCENT_MAX) {
        const scaledPercent = mapNumber(Number(ampVolume), 0, maximumVolume,
          VOLUME_PERCENT_MIN, VOLUME_PERCENT_MAX);
        displayPercent = Math.round(scaledPercent);
      }
      volumeShownSequence = requestSequence;
      showVolumePopup(displayPercent);
    } else if (ampVolume == "") {
      volumeShownSequence = requestSequence;
      showVolumePopup(VOLUME_AMP_OFF); // empty response = power off
    }
  });
}
