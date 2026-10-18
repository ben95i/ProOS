// Automate Website - colour picker prompt
//
// new ColorPrompt(anchorElementId, onColorChange, options) then .show().
// onColorChange is handed the hex without its "#", throttled while dragging.
// options: presets (swatches, hex strings or { color, name }), presetFile (a
// palette file to read instead), name (put into the heading), onClose().

//
// openColorPrompt(command, argument, title, threeArguments) is the one-call
// form the device buttons use: it opens the page's colour prompt and sends
// each picked colour to a device.

import { hexToRgb, rgbToHex, rgbToHexFromStyle, hsvToRgb, rgbToHsv,
    parsePalette, readFile, sendCmd,
    gridButtonTemplate } from './utils.js';
import { reportError } from './error.js';

// the element the page keeps for the device colour prompt (drawn by main.js)
const DEVICE_PROMPT_ANCHOR_ID = 'color-popup';
// what the prompt shows before a colour has been picked
const DEVICE_PROMPT_START_HEX = '#000000';

// Opens the colour prompt for a device and sends each colour picked in it.
// By default the colour goes out as main / command / <argument>_<hex>; with
// threeArguments it goes out as command / argument / <hex> instead.
// openColorPrompt('brxmit', 'bed_leds_hex_set', 'Bed Lights')
// openColorPrompt('leds', 'hex', 'Pixels', true)
export function openColorPrompt(command, argument, title, threeArguments = false) {
  // the previous prompt is destroyed rather than dropped: show() wipes the
  // anchor's markup, and an instance left behind mid-drag still holds
  // pointermove / pointerup listeners on window
  if (App.colorPrompt) {
    App.colorPrompt.destroy();
    App.colorPrompt = null;
  }
  const colorPrompt = new ColorPrompt(DEVICE_PROMPT_ANCHOR_ID, (strippedHex) => {
    let action = 'main';
    let firstArgument = command;
    let secondArgument = `${argument}_${strippedHex}`;
    if (threeArguments === true) {
      action = command;
      firstArgument = argument;
      secondArgument = strippedHex;
    }
    sendCmd(action, firstArgument, secondArgument)
      .catch(sendError => {
        reportError(`sending the colour #${strippedHex}`, sendError);
      });
  }, {
    name: title
  });
  colorPrompt.show();
  colorPrompt.updateStateFromHex(DEVICE_PROMPT_START_HEX, true, false);
  App.colorPrompt = colorPrompt;
}

export default class ColorPrompt {

  // The default palette is a file on the device, one colour per line as
  // hex_value|display_name, so the swatches change without a deploy.
  static PALETTE_FILE = 'colorpallet';
  // Palette file -> the read for it, done once per page and held. A read that
  // comes back empty is dropped from here again, so a file that was missing
  // is asked for once more rather than written off for the session.
  static paletteRequests = new Map();
  static THROTTLE_DELAY_MS = 50;
  static TITLE_PLAIN = 'Select Color';
  // the two faces of the box under the grid
  static HEX_LABEL = 'Hex:';
  static NAME_LABEL = 'Color:';
  static NAME_SHOWN_CLASS = 'color-name-shown';

  // 'LED' -> "Select LED Color"; nothing -> "Select Color"
  static titleFor(name) {
    let trimmedName = '';
    if (typeof name === 'string') {
      trimmedName = name.trim();
    }
    if (trimmedName === '') {
      return ColorPrompt.TITLE_PLAIN;
    }
    return `Select ${trimmedName} Color`;
  }

  // Reads a palette file, at most once per page per file. Answers [] when it
  // cannot be read -- the grid is then left out and the map, the hue slider
  // and the hex box carry the prompt on their own.
  static loadPaletteFile(file) {
    if (!ColorPrompt.paletteRequests.has(file)) {
      const request = readFile(file)
        .then((fileText) => {
          const palette = parsePalette(fileText);
          if (palette.length === 0) {
            ColorPrompt.paletteRequests.delete(file); // ask again next time
          }
          return palette;
        })
        .catch(() => {
          ColorPrompt.paletteRequests.delete(file);
          return [];
        });
      ColorPrompt.paletteRequests.set(file, request);
    }
    return ColorPrompt.paletteRequests.get(file);
  }

  // Reads a palette file ahead of the prompt that will want it, so its
  // swatches are already in hand when it opens. Nothing is drawn here.
  static warmPalette(file) {
    let wanted = ColorPrompt.PALETTE_FILE;
    if (typeof file === 'string' && file !== '') {
      wanted = file;
    }
    ColorPrompt.loadPaletteFile(wanted).catch(() => {});
  }

  // Takes a plain list of hex strings or a list of { color, name } entries
  // and always answers the second shape. Anything that is not a six digit hex
  // colour is dropped, so one bad line in a palette file cannot take the grid
  // with it. null when nothing usable is left, which is the caller's cue to
  // fall back to the default palette file.
  static normalizePresets(presets) {
    if (!Array.isArray(presets) || presets.length === 0) {
      return null;
    }
    const cleanedPresets = [];
    presets.forEach(entry => {
      const isPlainString = typeof entry === 'string';
      let rawColor = entry && entry.color;
      if (isPlainString) {
        rawColor = entry;
      }
      if (typeof rawColor !== 'string') {
        return;
      }
      const trimmedColor = rawColor.trim();
      let hexColor = `#${trimmedColor}`;
      if (trimmedColor.startsWith('#')) {
        hexColor = trimmedColor;
      }
      if (!/^#[0-9A-F]{6}$/i.test(hexColor)) {
        return;
      }
      let displayName = '';
      if (!isPlainString) {
        displayName = (entry && entry.name) || '';
      }
      cleanedPresets.push({ color: hexColor.toUpperCase(), name: displayName });
    });
    if (cleanedPresets.length === 0) {
      return null;
    }
    return cleanedPresets;
  }

  constructor(anchorElementId, onColorChangeCallback, options = {}) {
    this.anchorElement = document.getElementById(anchorElementId);
    if (!this.anchorElement) {
      throw new Error("Anchor element for ColorPrompt not found.");
    }
    this.onColorChangeCallback = onColorChangeCallback;
    const settings = options || {};
    // the caller's palette if it gave one; otherwise the grid starts empty
    // and show() fetches the default palette file into it
    this.presetColors = [];
    // uppercase hex -> the palette's name for it, for the colours that have
    // one. What the box under the grid shows is looked up in here.
    this.presetNames = new Map();
    this.setPresets(settings.presets);
    this.presetFile = ColorPrompt.PALETTE_FILE;
    if (typeof settings.presetFile === 'string' && settings.presetFile !== '') {
      this.presetFile = settings.presetFile;
    }
    this.promptTitle = ColorPrompt.titleFor(settings.name);
    this.onCloseCallback = null;
    if (typeof settings.onClose === 'function') {
      this.onCloseCallback = settings.onClose;
    }
    // destroy() can be reached more than once; the close hook fires on the
    // first of those and never again
    this.isClosed = false;
    // state
    this.currentHue = 0;
    this.currentSaturation = 1;
    this.currentValue = 1;
    this.isDraggingMap = false;
    this.lastCommandTime = 0;
    // DOM references, filled in by show()
    this.popupContent = null;
    this.colorMapElement = null;
    this.colorMapBackgroundElement = null;
    this.colorPointerElement = null;
    this.hueSliderElement = null;
    this.presetGridElement = null;
    this.titleElement = null;
    this.hexInputElement = null;
    this.hexLabelElement = null;
    this.closeButtonElement = null;
    // bound handlers, kept so they can be removed again
    this.boundHandleHexInput = this.handleHexInput.bind(this);
    this.boundHandleHueInput = this.handleHueInput.bind(this);
    this.boundHandleHueRelease = this.handleHueRelease.bind(this);
    this.boundStartMapDrag = this.startMapDrag.bind(this);
    this.boundUpdateMapPointer = this.updateMapPointer.bind(this);
    this.boundStopMapDrag = this.stopMapDrag.bind(this);
    this.boundClosePopup = this.closePopup.bind(this);
  }

  // builds the markup and puts it in the anchor element
  show() {
    this.anchorElement.innerHTML = `
      <div class="color-popup-content">
        <div class="color-popup-title" id="color-popup-title"></div>
        
        <div id="color-map">
          <div id="color-map-bg" class="color-map-layer"></div>
          <div class="color-map-layer map-layer-white"></div>
          <div class="color-map-layer map-layer-black"></div>
          <div id="color-pointer"></div>
        </div>

        <div class="hue-slider-container">
          <input type="range" class="slider-wide" id="slider-hue" min="0" max="360" value="0">
        </div>

        <div class="color-grid" id="preset-color-grid"></div>

        <div class="custom-color-input">
          <label for="hex-color" id="hex-color-label">Hex:</label>
          <input type="text" id="hex-color" maxlength="7" spellcheck="false" />
        </div>

        <div class="color-popup-actions">
          ${gridButtonTemplate({
            buttonId: 'btn-color-close',
            icon: 'fad fa-times-octagon',
            label: 'Close',
            buttonClass: 'button_left_txtwin color-popup-btn'
          })}
        </div>
      </div>
    `;
    this.popupContent              = this.anchorElement.querySelector('.color-popup-content');
    this.titleElement              = this.anchorElement.querySelector('#color-popup-title');
    this.colorMapElement           = this.anchorElement.querySelector('#color-map');
    this.colorMapBackgroundElement = this.anchorElement.querySelector('#color-map-bg');
    this.colorPointerElement       = this.anchorElement.querySelector('#color-pointer');
    this.hueSliderElement          = this.anchorElement.querySelector('#slider-hue');
    this.presetGridElement         = this.anchorElement.querySelector('#preset-color-grid');
    this.hexInputElement           = this.anchorElement.querySelector('#hex-color');
    this.hexLabelElement           = this.anchorElement.querySelector('#hex-color-label');
    this.closeButtonElement        = this.anchorElement.querySelector('#btn-color-close');
    // written in rather than built into the markup above, so a name never
    // reaches innerHTML -- the heading is text, and only ever text
    if (this.titleElement) {
      this.titleElement.innerText = this.promptTitle;
    }
    this.buildPresetGrid();
    this.bindEvents();
    this.anchorElement.style.display = 'flex';
    // no palette from the caller: read one off the device
    if (this.presetColors.length === 0) {
      this.loadPresets();
    }
  }

  // removes every listener and clears the markup
  destroy() {
    this.unbindEvents();
    this.releaseDragListeners();
    this.anchorElement.innerHTML = '';
    this.anchorElement.style.display = 'none';
    // Told once, after the markup is gone, so a handler that opens something
    // of its own is not doing it into a prompt that is half torn down. The
    // reference is dropped before the call, so a handler that reaches back in
    // here cannot come round again.
    this.isClosed = true;
    if (this.onCloseCallback) {
      const closeCallback = this.onCloseCallback;
      this.onCloseCallback = null;
      closeCallback();
    }
  }

  // Takes a palette in either accepted shape and keeps it, along with the
  // name lookup the box under the grid reads. Does not draw.
  setPresets(presets) {
    this.presetColors = ColorPrompt.normalizePresets(presets) || [];
    this.presetNames.clear();
    this.presetColors.forEach(preset => {
      if (preset.name) {
        this.presetNames.set(preset.color, preset.name);
      }
    });
  }

  // Fills an empty grid from the palette file. The prompt is already on
  // screen and usable while this is in the air.
  async loadPresets() {
    const palette = await ColorPrompt.loadPaletteFile(this.presetFile);
    // closed, or torn down and replaced, while the file was being read
    if (this.isClosed === true || !this.presetGridElement
        || !this.presetGridElement.isConnected) {
      return;
    }
    if (palette.length === 0) {
      return; // no file, no swatches -- the rest of the prompt stands alone
    }
    this.setPresets(palette);
    this.buildPresetGrid();
    // the swatch highlight and the name in the box both key off the palette,
    // so both are re-read now there is one; no command goes out for it
    this.updateUIFromState(false);
  }

  buildPresetGrid() {
    this.presetGridElement.innerHTML = '';
    this.presetColors.forEach(preset => {
      const colorHexCode = preset.color;
      const swatchElement = document.createElement('div');
      swatchElement.className = 'color-swatch';
      swatchElement.style.backgroundColor = colorHexCode;
      swatchElement.addEventListener('click', () => {
        this.updateStateFromHex(colorHexCode, true, true);
      });
      this.presetGridElement.appendChild(swatchElement);
    });
    // an empty grid is a bordered box with nothing in it, so it stands down
    // until there is a palette to put in it
    let gridDisplay = 'none';
    if (this.presetColors.length > 0) {
      gridDisplay = 'grid';
    }
    this.presetGridElement.style.display = gridDisplay;
  }

  bindEvents() {
    this.hexInputElement.addEventListener('input', this.boundHandleHexInput);
    this.hueSliderElement.addEventListener('input', this.boundHandleHueInput);
    // 'change' fires once on release and forces the resting value through the
    // throttle, the same thing stopMapDrag() does for the map. Without it a
    // release inside the throttle window was dropped and the lights kept the
    // second to last hue.
    this.hueSliderElement.addEventListener('change', this.boundHandleHueRelease);
    this.colorMapElement.addEventListener('pointerdown', this.boundStartMapDrag);
    this.closeButtonElement.addEventListener('click', this.boundClosePopup);
  }

  unbindEvents() {
    this.hexInputElement.removeEventListener('input', this.boundHandleHexInput);
    this.hueSliderElement.removeEventListener('input', this.boundHandleHueInput);
    this.hueSliderElement.removeEventListener('change', this.boundHandleHueRelease);
    this.colorMapElement.removeEventListener('pointerdown', this.boundStartMapDrag);
    this.closeButtonElement.removeEventListener('click', this.boundClosePopup);
  }

  releaseDragListeners() {
    window.removeEventListener('pointermove', this.boundUpdateMapPointer);
    window.removeEventListener('pointerup', this.boundStopMapDrag);
    window.removeEventListener('pointercancel', this.boundStopMapDrag);
  }

  closePopup() {
    this.destroy();
  }

  // typing in the hex box; anything that is not a full colour is ignored
  handleHexInput(event) {
    let inputValue = event.target.value;
    if (!inputValue.startsWith('#')) {
      inputValue = '#' + inputValue;
    }
    if (/^#[0-9A-F]{6}$/i.test(inputValue)) {
      this.updateStateFromHex(inputValue, true, true);
    }
  }

  // paints the map background for the hue currently held
  paintMapBackground() {
    this.colorMapBackgroundElement.style.background =
      `hsl(${this.currentHue}, 100%, 50%)`;
  }

  handleHueInput(event) {
    this.currentHue = parseFloat(event.target.value);
    this.paintMapBackground();
    this.updateUIFromState(true);
  }

  // the slider was released: send the resting value even if the last 'input'
  // fell inside the throttle window
  handleHueRelease(event) {
    this.currentHue = parseFloat(event.target.value);
    this.paintMapBackground();
    this.lastCommandTime = 0; // force the final colour through
    this.updateUIFromState(true);
  }

  startMapDrag(event) {
    this.isDraggingMap = true;
    this.updateMapPointer(event);
    window.addEventListener('pointermove', this.boundUpdateMapPointer);
    window.addEventListener('pointerup', this.boundStopMapDrag);
    window.addEventListener('pointercancel', this.boundStopMapDrag);
  }

  // saturation runs left to right across the map, value bottom to top
  updateMapPointer(event) {
    if (!this.isDraggingMap) {
      return;
    }
    const mapBoundingRect = this.colorMapElement.getBoundingClientRect();
    const rawPointerX = event.clientX - mapBoundingRect.left;
    const rawPointerY = event.clientY - mapBoundingRect.top;
    const mapWidth = mapBoundingRect.width;
    const mapHeight = mapBoundingRect.height;
    const clampedPointerX = Math.max(0, Math.min(rawPointerX, mapWidth));
    const clampedPointerY = Math.max(0, Math.min(rawPointerY, mapHeight));
    const saturationFraction = clampedPointerX / mapWidth;
    const depthFraction = clampedPointerY / mapHeight;
    const valueFraction = 1 - depthFraction;
    this.colorPointerElement.style.left = `${saturationFraction * 100}%`;
    this.colorPointerElement.style.top = `${depthFraction * 100}%`;
    this.currentSaturation = saturationFraction;
    this.currentValue = valueFraction;
    this.updateUIFromState(true);
  }

  stopMapDrag() {
    this.isDraggingMap = false;
    this.lastCommandTime = 0; // force the final colour through
    this.updateUIFromState(true);
    this.releaseDragListeners();
  }

  // syncs the state, and optionally the map, from a hex colour
  updateStateFromHex(hexString, updateMapUI = true, triggerCallback = true) {
    const rgbObject = hexToRgb(hexString);
    const hsvObject = rgbToHsv(rgbObject.red, rgbObject.green, rgbObject.blue);
    this.currentHue = hsvObject.hue;
    this.currentSaturation = hsvObject.saturation;
    this.currentValue = hsvObject.value;
    if (updateMapUI) {
      this.hueSliderElement.value = this.currentHue;
      this.paintMapBackground();
      const saturationPercentage = this.currentSaturation * 100;
      const depthPercentage = (1 - this.currentValue) * 100;
      this.colorPointerElement.style.left = `${saturationPercentage}%`;
      this.colorPointerElement.style.top = `${depthPercentage}%`;
    }
    // the box is written by updateUIFromState(), which knows whether this
    // colour has a name
    this.updateUIFromState(triggerCallback);
  }

  // repaints the box and the swatch highlight, and sends the colour on
  updateUIFromState(triggerCallback = true) {
    const rgbObject = hsvToRgb(this.currentHue, this.currentSaturation, this.currentValue);
    const hexString = rgbToHex(rgbObject.red, rgbObject.green, rgbObject.blue);
    this.showColorField(hexString);
    const allSwatches = this.anchorElement.querySelectorAll('.color-swatch');
    allSwatches.forEach(swatchElement => {
      const swatchColor = rgbToHexFromStyle(swatchElement.style.backgroundColor);
      swatchElement.classList.toggle('active-swatch', swatchColor === hexString);
    });
    if (triggerCallback) {
      this.dispatchColorCommand(hexString);
    }
  }

  // The box under the palette: the hex value, until the colour on screen is
  // one the palette has a name for -- then the name is shown in its place and
  // the label beside it changes with it. The map and the hue slider reach
  // colours no palette has a name for, so the hex is what it falls back to.
  showColorField(hexString) {
    const presetName = this.presetNames.get(hexString.toUpperCase());
    const hasName = typeof presetName === 'string' && presetName !== '';
    let fieldText = hexString;
    if (hasName) {
      fieldText = presetName;
    }
    this.hexInputElement.value = fieldText;
    // sized to what it holds: a hex is always seven characters, a name is
    // however long the palette file made it, and one spare character keeps
    // the text off the border
    this.hexInputElement.size = fieldText.length + 1;
    this.hexInputElement.classList.toggle(ColorPrompt.NAME_SHOWN_CLASS, hasName);
    if (this.hexLabelElement) {
      let labelText = ColorPrompt.HEX_LABEL;
      if (hasName) {
        labelText = ColorPrompt.NAME_LABEL;
      }
      this.hexLabelElement.innerText = labelText;
    }
  }

  // Throttled by timestamp rather than a timer, so a colour is only sent once
  // per THROTTLE_DELAY_MS while the map or slider is being dragged.
  dispatchColorCommand(hexString) {
    if (typeof this.onColorChangeCallback !== 'function') {
      return;
    }
    const currentTime = Date.now();
    const timeSinceLastCommand = currentTime - this.lastCommandTime;
    if (timeSinceLastCommand < ColorPrompt.THROTTLE_DELAY_MS) {
      return;
    }
    this.lastCommandTime = currentTime;
    this.onColorChangeCallback(hexString.replace('#', ''));
  }
}
