// Automate Website - living room
//
// The living room's button grid and the menus behind its buttons: the lamp,
// desktop and ceiling power menus, the stereo's volume buttons, its input
// menu and its More... menu (subwoofer gain, low cut, volume limiter).
// main.js lists LIVING_ROOM_CONFIG among its rooms and draws the grid.

import { registerListeners, gridButtonTemplate, dropdownButtonTemplate,
  isNumeric, menuRow, indicator } from './utils.js';
import { volumeButtonsTemplate, volumeButtonListeners, registerVolumeButtons,
  sendVolume } from './volctrl.js';
import { showPowerMenu, powerButtonTemplate, openDeviceMenu, showInputMenu,
  menuErrorRow, menuOfflineRow } from './dropdowns.js';

// the room transmitter every living room command goes through
const LIVING_ROOM_TARGET = 'lrxmit';

// What main.js needs to know about the room. key names its API commands
// (lrstatus, lron, lroff); the stereo amp counts its volume 0-100.
export const LIVING_ROOM_CONFIG = {
  key: 'lr',
  name: 'Living Room',
  icon: 'fad fa-couch',
  voltarget: LIVING_ROOM_TARGET,
  volmax: 100,
  template: livingRoomGridTemplate
};

// Stereo amp inputs, in the order the menu lists them. code is what the amp
// answers with when that input is live.
const HIFI_INPUTS = [
  { code: '6', cmd: 'autodac', label: 'Auto' },
  { code: '1', cmd: 'usb', label: 'USB' },
  { code: '3', cmd: 'phono', label: 'Phono' },
  { code: '5', cmd: 'opt', label: 'Optical' },
  { code: '4', cmd: 'coax', label: 'Coaxial' },
  { code: '2', cmd: 'aux', label: 'Aux' }
];

// what the amp answers in place of a state while it is switched off, and
// while it is muted
const HIFI_OFF_CODE = -1;
const HIFI_MUTED_CODE = 0;

// the two amp menus, named once for their buttons, spinners and rows
const HIFI_INPUT_MENU = 'hfinmenu';
const HIFI_MORE_MENU = 'hfmoremenu';

// the -5% and +5% buttons, which step the subwoofers in subwoofer mode
const VOLUME_STEP_DOWN_BUTTON = 'btn-vol-dwnc';
const VOLUME_STEP_UP_BUTTON = 'btn-vol-upc';

// the living room's button grid
function livingRoomGridTemplate() {
  return `
<div>
  ${powerButtonTemplate('wl1_dual', 'fad fa-lamp-floor', 'Lamp')}
  ${powerButtonTemplate('wkststate', 'fad fa-desktop-alt', 'Desktop')}
  ${powerButtonTemplate('clr1', 'fad fa-light-ceiling', 'Ceiling')}
  <p class="grid-sep"></p>
  ${volumeButtonsTemplate(LIVING_ROOM_CONFIG.key)}
  <p class="grid-sep"></p>
  ${gridButtonTemplate({
    buttonId: VOLUME_STEP_DOWN_BUTTON,
    icon: 'fad fa-volume-down',
    label: '-5%'
  })}
  ${powerButtonTemplate('hifistate', 'fad fa-speakers', 'Stereo')}
  ${gridButtonTemplate({
    buttonId: VOLUME_STEP_UP_BUTTON,
    icon: 'fad fa-volume-up',
    label: '+5%'
  })}
  <p class="grid-sep"></p>
  ${gridButtonTemplate({
    buttonId: 'btn-vol-display',
    icon: 'fad fa-volume-off fa-lg',
    label: 'Volume'
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-show-hifi-amp',
    buttonClass: 'btm-padding',
    icon: 'fad fa-sliders-v',
    label: 'Inputs',
    textId: `${HIFI_INPUT_MENU}-text`,
    spinnerId: `${HIFI_INPUT_MENU}-spinner`,
    menuId: HIFI_INPUT_MENU
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-show-hifi-more',
    buttonClass: 'btm-padding',
    icon: 'fad fa-list-music',
    label: 'More...',
    textId: `${HIFI_MORE_MENU}-text`,
    spinnerId: `${HIFI_MORE_MENU}-spinner`,
    menuId: HIFI_MORE_MENU
  })}
</div>`;
}
registerListeners({
  'btn-power-wkststate':   () => showPowerMenu(LIVING_ROOM_TARGET, 'wkststate'),
  'btn-power-wl1_dual':    () => showPowerMenu(LIVING_ROOM_TARGET, 'wl1_dual'),
  'btn-power-clr1':        () => showPowerMenu(LIVING_ROOM_TARGET, 'clr1'),
  [VOLUME_STEP_DOWN_BUTTON]: () => sendVolume(LIVING_ROOM_CONFIG, 'dwnc'),
  'btn-power-hifistate':   () => showPowerMenu(LIVING_ROOM_TARGET, 'hifistate'),
  [VOLUME_STEP_UP_BUTTON]: () => sendVolume(LIVING_ROOM_CONFIG, 'upc'),
  'btn-vol-display':       () => sendVolume(LIVING_ROOM_CONFIG, 'display'),
  'btn-show-hifi-amp':     () => showHiFiAmpInput(),
  'btn-show-hifi-more':    () => showHiFiMoreMenu()
});
registerListeners(volumeButtonListeners(LIVING_ROOM_CONFIG));
// the two step buttons go red with the volume buttons in subwoofer mode
registerVolumeButtons(LIVING_ROOM_CONFIG.key, [
  VOLUME_STEP_DOWN_BUTTON,
  VOLUME_STEP_UP_BUTTON
]);

// Inputs: the stereo amp's inputs, green on the one that is live
function showHiFiAmpInput() {
  showInputMenu({
    menu: HIFI_INPUT_MENU,
    target: LIVING_ROOM_TARGET,
    cmd: 'curinput',
    textId: `${HIFI_INPUT_MENU}-text`,
    spinnerId: `${HIFI_INPUT_MENU}-spinner`,
    offCode: HIFI_OFF_CODE,
    inputs: HIFI_INPUTS
  });
}

// More...: Subs Gain (turns the volume buttons into subwoofer controls), and
// the low cut filter and volume limiter, each green while it is on
function showHiFiMoreMenu() {
  openDeviceMenu({
    menu: HIFI_MORE_MENU,
    target: LIVING_ROOM_TARGET,
    cmd: 'curhpfvollim',
    textId: `${HIFI_MORE_MENU}-text`,
    spinnerId: `${HIFI_MORE_MENU}-spinner`,
    build: (reply) => {
      // No reply at all (the API could not be reached) is an error like any
      // other. It used to be asked whether it held a "~", which threw and
      // left the button's spinner turning.
      if (typeof reply !== 'string') {
        return menuErrorRow(HIFI_MORE_MENU);
      }
      // the amp answers one number while it is off or muted, and
      // volLimitState~hpfState while it is playing
      const replyIsNumber = isNumeric(reply) === true;
      const replyIsStatePair = reply.includes('~');
      if (!replyIsNumber && !replyIsStatePair) {
        return menuErrorRow(HIFI_MORE_MENU);
      }
      const replyCode = Number(reply);
      if (replyCode === HIFI_OFF_CODE) {
        return menuOfflineRow(HIFI_MORE_MENU);
      }
      if (replyCode === HIFI_MUTED_CODE) {
        return menuRow(`${HIFI_MORE_MENU}-muted`, 'noind', 'Muted');
      }
      const [limiterState, lowCutState] = reply.split("~");
      const lowCutIndicator = indicator(lowCutState == '1');
      const limiterIndicator = indicator(limiterState == '1');
      let rows = menuRow('subs', 'subs', 'Subs Gain');
      rows += menuRow(`cmd~${LIVING_ROOM_TARGET}~togglehpf`, lowCutIndicator, 'Low Cut');
      rows += menuRow(`cmd~${LIVING_ROOM_TARGET}~vlimit`, limiterIndicator, 'vLimiter');
      return rows;
    }
  });
}
