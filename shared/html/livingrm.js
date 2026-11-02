// Automate - living room: button grid and the menus behind it (power menus, stereo volume
// and inputs, More...: subwoofer gain, low cut, volume limiter).

import { registerListeners, gridButtonTemplate, dropdownButtonTemplate,
  isNumeric, menuRow, indicator } from './utils.js';
import { volumeButtonsTemplate, volumeButtonListeners, registerVolumeButtons,
  sendVolume } from './volctrl.js';
import { powerButtonTemplate, powerListeners, spinnerIds, openDeviceMenu, showInputMenu,
  menuErrorRow, menuOfflineRow } from './dropdowns.js';

// the room transmitter every command goes through
const LIVING_ROOM_TARGET = 'lrxmit';

// The room's config. key names its API commands (lrstatus, lron, lroff); the amp counts
// its volume 0-100; climate is the temperature window's source, the thermostat's status.
export const LIVING_ROOM_CONFIG = {
  key: 'lr',
  name: 'Living Room',
  icon: 'fad fa-couch',
  voltarget: LIVING_ROOM_TARGET,
  volmax: 100,
  climate: { target: LIVING_ROOM_TARGET, cmd: 'lr-thermo1', fields: [3, 4] },
  template: livingRoomGridTemplate
};

// Amp inputs in menu order; code is the amp's answer while that input is live.
const HIFI_INPUTS = [
  { code: '6', cmd: 'autodac', label: 'Auto' },
  { code: '1', cmd: 'usb', label: 'USB' },
  { code: '3', cmd: 'phono', label: 'Phono' },
  { code: '5', cmd: 'opt', label: 'Optical' },
  { code: '4', cmd: 'coax', label: 'Coaxial' },
  { code: '2', cmd: 'aux', label: 'Aux' }
];

// the amp's answer in place of a state while off, and while muted
const HIFI_OFF_CODE = -1;
const HIFI_MUTED_CODE = 0;

// the two amp menus
const HIFI_INPUT_MENU = 'hfinmenu';
const HIFI_MORE_MENU = 'hfmoremenu';

// the -5% and +5% buttons, which step the subwoofers in subwoofer mode
const VOLUME_STEP_DOWN_BUTTON = 'btn-vol-dwnc';
const VOLUME_STEP_UP_BUTTON = 'btn-vol-upc';

// the room's button grid
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
    ...spinnerIds(HIFI_INPUT_MENU),
    menuId: HIFI_INPUT_MENU
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-show-hifi-more',
    buttonClass: 'btm-padding',
    icon: 'fad fa-list-music',
    label: 'More...',
    ...spinnerIds(HIFI_MORE_MENU),
    menuId: HIFI_MORE_MENU
  })}
</div>`;
}
registerListeners({
  ...powerListeners(LIVING_ROOM_TARGET, ['wkststate', 'wl1_dual', 'clr1', 'hifistate']),
  [VOLUME_STEP_DOWN_BUTTON]: () => sendVolume(LIVING_ROOM_CONFIG, 'dwnc'),
  [VOLUME_STEP_UP_BUTTON]: () => sendVolume(LIVING_ROOM_CONFIG, 'upc'),
  'btn-vol-display':       () => sendVolume(LIVING_ROOM_CONFIG, 'display'),
  'btn-show-hifi-amp':     () => showHiFiAmpInput(),
  'btn-show-hifi-more':    () => showHiFiMoreMenu()
});
registerListeners(volumeButtonListeners(LIVING_ROOM_CONFIG));
// the step buttons go red with the volume buttons in subwoofer mode
registerVolumeButtons(LIVING_ROOM_CONFIG.key, [
  VOLUME_STEP_DOWN_BUTTON,
  VOLUME_STEP_UP_BUTTON
]);

// Inputs: the amp's inputs, green on the live one
function showHiFiAmpInput() {
  showInputMenu({
    menu: HIFI_INPUT_MENU,
    target: LIVING_ROOM_TARGET,
    cmd: 'curinput',
    offCode: HIFI_OFF_CODE,
    inputs: HIFI_INPUTS
  });
}

// More...: Subs Gain (the volume buttons control the subwoofers), and the low cut and
// volume limiter, each green while on
function showHiFiMoreMenu() {
  openDeviceMenu({
    menu: HIFI_MORE_MENU,
    target: LIVING_ROOM_TARGET,
    cmd: 'curhpfvollim',
    build: (reply) => {
      // no reply at all is an error too
      if (typeof reply !== 'string') {
        return menuErrorRow(HIFI_MORE_MENU);
      }
      // one number while off or muted, volLimitState~hpfState while playing
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
