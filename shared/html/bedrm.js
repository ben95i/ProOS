// Automate Website - bedroom
//
// The bedroom's button grid and what its buttons open: the bed lights menu
// and the bed adjustment window, the relax sounds menu, the power menus, the
// amp's volume buttons and its input menu. main.js lists BEDROOM_CONFIG among
// its rooms and draws the grid.

import { registerListeners, dropdownButtonTemplate, volumeButtonsTemplate,
  volumeButtonListeners, sendCmd, requestThrottle, promptWindow,
  onButtons } from './utils.js';
import { showPowerMenu, powerButtonTemplate, showDynAPIMenu,
  showInputMenu } from './menus.js';
import { openColorPrompt } from './colorprompt.js';

// the room transmitter, and the Bedroom Pi that drives the amp
const BEDROOM_TARGET = 'brxmit';
const BEDROOM_AMP_TARGET = 'brpi';

// What main.js needs to know about the room. key names its API commands
// (brstatus, bron, broff); the amp counts its volume 0-192, which the volume
// popup shows as a percentage.
export const BEDROOM_CONFIG = {
  key: 'br',
  name: 'Bedroom',
  icon: 'fad fa-bed',
  voltarget: BEDROOM_AMP_TARGET,
  volmax: 192,
  template: bedroomGridTemplate
};

// Amp inputs, in the order the menu lists them. code is what the amp answers
// with when that input is live.
const BEDROOM_INPUTS = [
  { code: '2', cmd: 'opt-a', label: 'USB' },
  { code: '3', cmd: 'coaxial', label: 'Coaxial' },
  { code: '1', cmd: 'opt-b', label: 'Optical' },
  { code: '4', cmd: 'aux', label: 'Aux' }
];

// what the amp answers in place of an input while it is switched off
const BEDROOM_AMP_OFF_CODE = 0;

// the bed lights menu, which showPowerMenu() also fills with a slider
const BED_LEDS_MENU = 'bed_leds';
const BEDROOM_INPUT_MENU = 'brinpmenu';

// the bedroom's button grid
function bedroomGridTemplate() {
  return `
<div>
  ${dropdownButtonTemplate({
    buttonId: 'btn-menu-br-bed',
    icon: 'fad fa-bed-alt',
    label: 'Bed',
    textId: `${BED_LEDS_MENU}-text`,
    spinnerId: `${BED_LEDS_MENU}-spinner`,
    menuId: `${BED_LEDS_MENU}-menu`,
    menuClass: 'dd-button',
    menuContent: `
      <a id="btn-bed-adjprompt"> Adjust<i class="fad fa-sort-circle-up"></i></a>
      <a id="bed_leds-colorprompt"> Colors<i class="fad fa-palette"></i></a>
      <a id="bed_leds-colorcycle"> Cycle</a>
      <a id="bed_leds-sweep"> Sweep</a>
      <a id="bed_leds-rainbow"> Rainbow</a>
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
    textId: 'ampinp-text',
    spinnerId: 'ampinp-spinner',
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
  'btn-power-brlamp1':    () => showPowerMenu(BEDROOM_TARGET, 'brlamp1'),
  'btn-power-brtv':       () => showPowerMenu(BEDROOM_TARGET, 'brtv'),
  'btn-power-ampstate':   () => showPowerMenu(BEDROOM_AMP_TARGET, 'ampstate'),
  'btn-power-brpc':       () => showPowerMenu(BEDROOM_TARGET, 'brpc'),
  'btn-power-brmacs':     () => showPowerMenu(BEDROOM_TARGET, 'brmacs'),
  'btn-show-br-amp':      () => showBedroomAmpInput(),
  'btn-power-retropi':    () => showPowerMenu(BEDROOM_TARGET, 'retropi')
});
registerListeners(volumeButtonListeners(BEDROOM_CONFIG));

// Inputs: the amp's inputs, green on the one that is live
function showBedroomAmpInput() {
  showInputMenu({
    menu: BEDROOM_INPUT_MENU,
    target: BEDROOM_AMP_TARGET,
    cmd: 'inputstate',
    textId: 'ampinp-text',
    spinnerId: 'ampinp-spinner',
    offCode: BEDROOM_AMP_OFF_CODE,
    inputs: BEDROOM_INPUTS
  });
}

/////// Bed Adjustment Window ///////

// the bed motor takes at most one step every this many milliseconds
const BED_ADJUST_THROTTLE_MS = 250;
const BED_PROMPT_ID = 'bed__prompt';

// separate throttles so up/down each get their own 250ms window
const throttledBedUp = requestThrottle(() => {
  sendCmd('main', BEDROOM_TARGET, 'bed_adj_up');
}, BED_ADJUST_THROTTLE_MS);

const throttledBedDown = requestThrottle(() => {
  sendCmd('main', BEDROOM_TARGET, 'bed_adj_dwn');
}, BED_ADJUST_THROTTLE_MS);

// Opens the bed adjustment window: an up button, a down button and Close.
// A second press of Adjust while it is open does nothing.
function bedPrompt() {
  if (document.getElementById(BED_PROMPT_ID)) {
    return;
  }
  const bedWindow = promptWindow(BED_PROMPT_ID, `
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
        // a trailing press still inside its window would otherwise go out
        // after the window it came from had gone
        throttledBedUp.cancel();
        throttledBedDown.cancel();
        bedWindow.remove();
        break;
    }
  });
}
