// Automate - bedroom: button grid and what it opens (bed lights menu and adjustment
// window, relax sounds, power menus, the amp's volume and inputs).

import { registerListeners, dropdownButtonTemplate, sendCmd,
  requestThrottle, promptWindow, onButtons, gridButtonTemplate } from './utils.js';
import { volumeButtonsTemplate, volumeButtonListeners } from './volctrl.js';
import { showPowerMenu, powerButtonTemplate, powerListeners, spinnerIds, showDynAPIMenu,
  showInputMenu } from './dropdowns.js';
import { openColorPrompt } from './colorprompt.js';

// the room transmitter, and the Bedroom Pi that drives the amp
const BEDROOM_TARGET = 'brxmit';
const BEDROOM_AMP_TARGET = 'brpi';

// The room's config. key names its API commands (brstatus, bron, broff); the amp counts
// its volume 0-192; climate is the temperature window's source, the hub's temperature~humidity.
export const BEDROOM_CONFIG = {
  key: 'br',
  name: 'Bedroom',
  icon: 'fad fa-bed',
  voltarget: BEDROOM_AMP_TARGET,
  volmax: 192,
  climate: { target: BEDROOM_TARGET, cmd: 'roomth', fields: [0, 1] },
  template: bedroomGridTemplate
};

// Amp inputs in menu order; code is the amp's answer while that input is live.
const BEDROOM_INPUTS = [
  { code: '2', cmd: 'opt-a', label: 'USB' },
  { code: '3', cmd: 'coaxial', label: 'Coaxial' },
  { code: '1', cmd: 'opt-b', label: 'Optical' },
  { code: '4', cmd: 'aux', label: 'Aux' }
];

// the amp's answer in place of an input while off
const BEDROOM_AMP_OFF_CODE = 0;

// the bed lights menu, which showPowerMenu() also fills with a slider
const BED_LEDS_MENU = 'bed_leds';
const BEDROOM_INPUT_MENU = 'brinpmenu';
const BEDROOM_INPUT_SPINNER = 'ampinp';

// the room's button grid
function bedroomGridTemplate() {
  return `
<div>
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-br-bed',
    icon: 'fad fa-bed-alt',
    label: 'Bed',
    ...spinnerIds(BED_LEDS_MENU),
    menuId: `${BED_LEDS_MENU}-menu`,
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-bed-adjprompt"> Adjust<i class="fad fa-sort-circle-up"></i></a>
      <a id="bed_leds-colorprompt"> Colors<i class="fad fa-palette"></i></a>
      <a id="bed_leds-rainbow"> Rainbow</a>
      <a id="bed_leds-colorcycle"> Cycle</a>
      <a id="bed_leds-sweep"> Sweep</a>
    `
  })}
  ${dropdownButtonTemplate({
    buttonId: 'btn-dyn-relaxmenu',
    icon: 'fad fa-snooze',
    label: 'Relax',
    menuId: 'relaxmenu',
    menuClass: 'dd-button'
  })}
  ${powerButtonTemplate('brlamp1', 'fad fa-lamp-desk', 'Dresser')}
  <p class="grid-sep"></p>
  ${volumeButtonsTemplate(BEDROOM_CONFIG.key)}
  <p class="grid-sep"></p>
  ${powerButtonTemplate('brtv', 'fad fa-tv-retro', 'TV')}
  ${powerButtonTemplate('ampstate', 'fad fa-waveform', 'Audio')}
  ${powerButtonTemplate('brpc', 'fad fa-mouse', 'PC')}
  <p class="grid-sep"></p>
  ${powerButtonTemplate('brmacs', 'fad fa-computer-classic', 'Macs')}
  ${dropdownButtonTemplate({
    buttonId: 'btn-show-br-amp',
    buttonClass: 'btm-padding',
    icon: 'fad fa-sliders-v',
    label: 'Inputs',
    ...spinnerIds(BEDROOM_INPUT_SPINNER),
    menuId: BEDROOM_INPUT_MENU
  })}
  ${powerButtonTemplate('retropi', 'fad fa-gamepad', 'RetroPi')}
</div>`;
}
registerListeners({
  'btn-menu-br-bed':      () => showPowerMenu(BEDROOM_TARGET, BED_LEDS_MENU),
  'btn-bed-adjprompt':    () => bedPrompt(),
  'bed_leds-colorprompt': () => openColorPrompt(BEDROOM_TARGET, 'bed_leds_hex_set', 'Bed Lights'),
  'bed_leds-rainbow':     () => sendCmd('main', BEDROOM_TARGET, 'bed_leds_rb'),
  'bed_leds-colorcycle':  () => sendCmd('main', BEDROOM_TARGET, 'bed_leds_cy'),
  'bed_leds-sweep':       () => sendCmd('main', BEDROOM_TARGET, 'bed_leds_sw'),
  'btn-dyn-relaxmenu':    () => showDynAPIMenu('relaxmenu'),
  'btn-show-br-amp':      () => showBedroomAmpInput(),
  ...powerListeners(BEDROOM_TARGET, ['brlamp1', 'brtv', 'brpc', 'brmacs', 'retropi']),
  ...powerListeners(BEDROOM_AMP_TARGET, ['ampstate'])
});
registerListeners(volumeButtonListeners(BEDROOM_CONFIG));

// Inputs: the amp's inputs, green on the live one
function showBedroomAmpInput() {
  showInputMenu({
    menu: BEDROOM_INPUT_MENU,
    target: BEDROOM_AMP_TARGET,
    cmd: 'inputstate',
    spinner: BEDROOM_INPUT_SPINNER,
    offCode: BEDROOM_AMP_OFF_CODE,
    inputs: BEDROOM_INPUTS
  });
}

/////// Bed Adjustment Window ///////

// the bed motor takes at most one step this often
const BED_ADJUST_THROTTLE_MS = 250;
const BED_PROMPT_ID = 'bed__prompt';

// separate throttles, so up and down each get their own window
const throttledBedUp = requestThrottle(() => {
  sendCmd('main', BEDROOM_TARGET, 'bed_adj_up');
}, BED_ADJUST_THROTTLE_MS);

const throttledBedDown = requestThrottle(() => {
  sendCmd('main', BEDROOM_TARGET, 'bed_adj_dwn');
}, BED_ADJUST_THROTTLE_MS);

// Opens the bed adjustment window: up, down and Close.
function bedPrompt() {
  if (document.getElementById(BED_PROMPT_ID)) {
    return;
  }
  const bedWindow = promptWindow(BED_PROMPT_ID, `
    <div class="bed__title">Bed Adjustment</div>
    ${gridButtonTemplate({
      buttonId: 'bed__btn-up',
      icon: 'fad fa-arrow-up bed__icon',
      buttonClass: 'bed__btn'
    })}
    ${gridButtonTemplate({
      buttonId: 'bed__btn-down',
      icon: 'fad fa-arrow-down bed__icon',
      buttonClass: 'bed__btn'
    })}
    ${gridButtonTemplate({
      buttonId: 'bed__btn-close',
      icon: 'fad fa-times-octagon right-padding',
      label: 'Close'
    })}
  `);
  onButtons(bedWindow, (button) => {
    switch (button.id) {
      case 'bed__btn-up':
        throttledBedUp();
        break;
      case 'bed__btn-down':
        throttledBedDown();
        break;
      case 'bed__btn-close':
        // a trailing press would otherwise go out after its window had gone
        throttledBedUp.cancel();
        throttledBedDown.cancel();
        bedWindow.remove();
        break;
    }
  });
}
