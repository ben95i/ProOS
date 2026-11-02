// Automate - colour picker prompt: saturation / value map, hue slider, palette swatches
// and hex box, drawn into the page's color-popup element. Colours reach onColorChange as
// hex without the "#", throttled while dragging.

import { hexToRgb, rgbToHex, rgbToHexFromStyle, hsvToRgb, rgbToHsv,
    parsePalette, readFile, sendCmd,
    gridButtonTemplate } from './utils.js';

const PROMPT_ANCHOR_ID = 'color-popup';
const HEX_COLOR = /^#[0-9A-F]{6}$/i;
// the default palette file, hex|name per line
const DEFAULT_PALETTE_FILE = 'colorpallet';
// palette file -> its read, done once per page; an empty read is asked for again
const paletteRequests = new Map();

// Reads a palette file, at most once per page per file; [] when it cannot be read, and
// the prompt then works without a grid.
export function loadPaletteFile(file) {
  if (!paletteRequests.has(file)) {
    const request = readFile(file).then((fileText) => {
      const palette = parsePalette(fileText);
      if (palette.length === 0) {
        paletteRequests.delete(file);
      }
      return palette;
    });
    paletteRequests.set(file, request);
  }
  return paletteRequests.get(file);
}

// Destroys an open prompt, as one left mid-drag holds listeners on window.
export function closeColorPrompt() {
  if (App.colorPrompt) {
    App.colorPrompt.destroy();
    App.colorPrompt = null;
  }
}

// Opens the page's one prompt on startHex, without sending that colour.
// options: name (in the heading), presetFile, onClose
export function showColorPrompt(startHex, onColorChange, options) {
  closeColorPrompt();
  const colorPrompt = new ColorPrompt(onColorChange, options);
  colorPrompt.show();
  colorPrompt.updateStateFromHex(startHex, false);
  App.colorPrompt = colorPrompt;
}

// Opens the prompt for a device and sends each colour picked: main / command /
// <argument>_<hex>, or with threeArguments command / argument / <hex>.
// openColorPrompt('brxmit', 'bed_leds_hex_set', 'Bed Lights')
// openColorPrompt('leds', 'hex', 'Pixels', true)
export function openColorPrompt(command, argument, title, threeArguments = false) {
  showColorPrompt('#000000', (strippedHex) => {
    let action = 'main';
    let firstArgument = command;
    let secondArgument = `${argument}_${strippedHex}`;
    if (threeArguments === true) {
      action = command;
      firstArgument = argument;
      secondArgument = strippedHex;
    }
    sendCmd(action, firstArgument, secondArgument);
  }, {
    name: title
  });
}

class ColorPrompt {

  static THROTTLE_DELAY_MS = 50;
  static HEX_LABEL = 'Hex:';
  static NAME_LABEL = 'Color:';
  static NAME_SHOWN_CLASS = 'color-name-shown';

  // options: see showColorPrompt()
  constructor(onColorChangeCallback, options) {
    this.anchorElement = document.getElementById(PROMPT_ANCHOR_ID);
    this.onColorChangeCallback = onColorChangeCallback;
    this.presetColors = [];
    // uppercase hex -> its palette name, for the box under the grid
    this.presetNames = new Map();
    this.presetFile = DEFAULT_PALETTE_FILE;
    if (options.presetFile) {
      this.presetFile = options.presetFile;
    }
    this.promptTitle = `Select ${options.name} Color`;
    this.onCloseCallback = options.onClose;
    this.currentHue = 0;
    this.currentSaturation = 1;
    this.currentValue = 1;
    this.isDraggingMap = false;
    this.lastCommandTime = 0;
    // kept so destroy() can take them off window
    this.boundUpdateMapPointer = this.updateMapPointer.bind(this);
    this.boundStopMapDrag = this.stopMapDrag.bind(this);
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
    this.titleElement              = this.anchorElement.querySelector('#color-popup-title');
    this.colorMapElement           = this.anchorElement.querySelector('#color-map');
    this.colorMapBackgroundElement = this.anchorElement.querySelector('#color-map-bg');
    this.colorPointerElement       = this.anchorElement.querySelector('#color-pointer');
    this.hueSliderElement          = this.anchorElement.querySelector('#slider-hue');
    this.presetGridElement         = this.anchorElement.querySelector('#preset-color-grid');
    this.hexInputElement           = this.anchorElement.querySelector('#hex-color');
    this.hexLabelElement           = this.anchorElement.querySelector('#hex-color-label');
    this.titleElement.innerText = this.promptTitle;
    this.buildPresetGrid();
    this.bindEvents();
    this.anchorElement.style.display = 'flex';
    this.loadPresets();
  }

  // Clears the markup and its listeners; onClose is told once, after the markup has gone.
  destroy() {
    this.releaseDragListeners();
    this.anchorElement.innerHTML = '';
    this.anchorElement.style.display = 'none';
    if (this.onCloseCallback) {
      const closeCallback = this.onCloseCallback;
      this.onCloseCallback = null;
      closeCallback();
    }
  }

  // keeps a palette with its name lookup, dropping any colour that is not six digit hex
  setPresets(presets) {
    this.presetColors = [];
    this.presetNames.clear();
    presets.forEach(preset => {
      const hexColor = preset.color.trim();
      if (!HEX_COLOR.test(hexColor)) {
        return;
      }
      const upperColor = hexColor.toUpperCase();
      this.presetColors.push({ color: upperColor, name: preset.name });
      if (preset.name) {
        this.presetNames.set(upperColor, preset.name);
      }
    });
  }

  // Fills the grid from the palette file, with the prompt already usable.
  async loadPresets() {
    const palette = await loadPaletteFile(this.presetFile);
    // closed, or replaced, while the file was being read
    if (!this.presetGridElement.isConnected) {
      return;
    }
    if (palette.length === 0) {
      return;
    }
    this.setPresets(palette);
    this.buildPresetGrid();
    // the highlight and the name key off the palette; no command goes out
    this.updateUIFromState(false);
  }

  // draws the swatches; an empty grid stands down
  buildPresetGrid() {
    this.presetGridElement.innerHTML = '';
    this.presetColors.forEach(preset => {
      const colorHexCode = preset.color;
      const swatchElement = document.createElement('div');
      swatchElement.className = 'color-swatch';
      swatchElement.style.backgroundColor = colorHexCode;
      swatchElement.addEventListener('click', () => {
        this.updateStateFromHex(colorHexCode, true);
      });
      this.presetGridElement.appendChild(swatchElement);
    });
    let gridDisplay = 'none';
    if (this.presetColors.length > 0) {
      gridDisplay = 'grid';
    }
    this.presetGridElement.style.display = gridDisplay;
  }

  // binds the prompt's own controls; they go with the markup
  bindEvents() {
    this.hexInputElement.addEventListener('input', (event) => this.handleHexInput(event));
    this.hueSliderElement.addEventListener('input', (event) => this.handleHueInput(event));
    this.hueSliderElement.addEventListener('change', (event) => this.handleHueRelease(event));
    this.colorMapElement.addEventListener('pointerdown', (event) => this.startMapDrag(event));
    this.anchorElement.querySelector('#btn-color-close').addEventListener('click', () => this.destroy());
  }

  // stops following the pointer
  releaseDragListeners() {
    window.removeEventListener('pointermove', this.boundUpdateMapPointer);
    window.removeEventListener('pointerup', this.boundStopMapDrag);
    window.removeEventListener('pointercancel', this.boundStopMapDrag);
  }

  // typing in the hex box; anything that is not a full colour is ignored
  handleHexInput(event) {
    let inputValue = event.target.value;
    if (!inputValue.startsWith('#')) {
      inputValue = '#' + inputValue;
    }
    if (HEX_COLOR.test(inputValue)) {
      this.updateStateFromHex(inputValue, true);
    }
  }

  // paints the map background for the hue held
  paintMapBackground() {
    this.colorMapBackgroundElement.style.background =
      `hsl(${this.currentHue}, 100%, 50%)`;
  }

  // the hue slider moved
  handleHueInput(event) {
    this.currentHue = parseFloat(event.target.value);
    this.paintMapBackground();
    this.updateUIFromState(true);
  }

  // the slider was released: the resting value goes out even inside the throttle
  handleHueRelease(event) {
    this.lastCommandTime = 0;
    this.handleHueInput(event);
  }

  // a press on the map: follows the pointer until release
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

  // the map was released: the final colour goes out even inside the throttle
  stopMapDrag() {
    this.isDraggingMap = false;
    this.lastCommandTime = 0;
    this.updateUIFromState(true);
    this.releaseDragListeners();
  }

  // sets the state, the slider and the map pointer from a hex colour
  updateStateFromHex(hexString, triggerCallback) {
    const rgbObject = hexToRgb(hexString);
    const hsvObject = rgbToHsv(rgbObject.red, rgbObject.green, rgbObject.blue);
    this.currentHue = hsvObject.hue;
    this.currentSaturation = hsvObject.saturation;
    this.currentValue = hsvObject.value;
    this.hueSliderElement.value = this.currentHue;
    this.paintMapBackground();
    const saturationPercentage = this.currentSaturation * 100;
    const depthPercentage = (1 - this.currentValue) * 100;
    this.colorPointerElement.style.left = `${saturationPercentage}%`;
    this.colorPointerElement.style.top = `${depthPercentage}%`;
    this.updateUIFromState(triggerCallback);
  }

  // repaints the box and the swatch highlight, and sends the colour on
  updateUIFromState(triggerCallback) {
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

  // The box under the palette: the colour's palette name when it has one, or its hex.
  showColorField(hexString) {
    const presetName = this.presetNames.get(hexString.toUpperCase());
    const hasName = typeof presetName === 'string' && presetName !== '';
    let fieldText = hexString;
    if (hasName) {
      fieldText = presetName;
    }
    this.hexInputElement.value = fieldText;
    // sized to its text, with one spare character off the border
    this.hexInputElement.size = fieldText.length + 1;
    this.hexInputElement.classList.toggle(ColorPrompt.NAME_SHOWN_CLASS, hasName);
    let labelText = ColorPrompt.HEX_LABEL;
    if (hasName) {
      labelText = ColorPrompt.NAME_LABEL;
    }
    this.hexLabelElement.innerText = labelText;
  }

  // sends a colour at most once per THROTTLE_DELAY_MS
  dispatchColorCommand(hexString) {
    const currentTime = Date.now();
    const timeSinceLastCommand = currentTime - this.lastCommandTime;
    if (timeSinceLastCommand < ColorPrompt.THROTTLE_DELAY_MS) {
      return;
    }
    this.lastCommandTime = currentTime;
    this.onColorChangeCallback(hexString.replace('#', ''));
  }
}
