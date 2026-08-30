/**
 * ColorPrompt
 * Encapsulates the UI, state, and lifecycle of the color selection prompt.
 */
import { hexToRgb, rgbToHex, rgbToHexFromStyle, 
    hsvToRgb, rgbToHsv, parsePalette, readFile } from './utils.js';

export default class ColorPrompt {

  // The default palette. It used to be a list in here; it is a file on the
  // device now, in the same hex_value|display_name format the theme menu
  // uses, so the swatches can be changed without a deploy.
  static PALETTE_FILE = 'colorpallet';
  // The file, read once for the page and held. Every prompt after the first
  // gets the swatches without a second read. A read that comes back with
  // nothing clears this again, so a file that was missing or empty is asked
  // for again rather than being written off for the session.
  static paletteRequest = null;
  static THROTTLE_DELAY_MS = 50;
  // The heading. A caller that names what it is picking a colour for gets
  // that name in the middle of it; one that does not gets the plain form the
  // prompt has always carried.
  static TITLE_PLAIN = 'Select Color';
  static titleFor(name) {
    const trimmedName = typeof name === 'string' ? name.trim() : '';
    if (trimmedName === '') {
      return ColorPrompt.TITLE_PLAIN;
    }
    return `Select ${trimmedName} Color`;
  }
  // the two faces of the box under the grid
  static HEX_LABEL = 'Hex:';
  static NAME_LABEL = 'Color:';
  static NAME_SHOWN_CLASS = 'color-name-shown';

  /**
   * Reads the default palette file, at most once per page.
   * Returns [] when it cannot be read -- the grid is left out in that case
   * and the map, the hue slider and the hex box carry the prompt on their
   * own, which is what they did before there was a palette at all.
   */
  static loadDefaultPalette() {
    if (!ColorPrompt.paletteRequest) {
      ColorPrompt.paletteRequest = readFile(ColorPrompt.PALETTE_FILE)
        .then((fileText) => {
          const palette = parsePalette(fileText);
          if (palette.length === 0) {
            ColorPrompt.paletteRequest = null; // ask again next time
          }
          return palette;
        })
        .catch(() => {
          ColorPrompt.paletteRequest = null;
          return [];
        });
    }
    return ColorPrompt.paletteRequest;
  }

  /**
   * Accepts either a plain list of hex strings or a list of
   * { color, name } entries, and always returns the second shape. Anything
   * that is not a six digit hex colour is dropped: a palette can come out of
   * a file on the device, and one bad line must not take the grid with it.
   * Returns null when nothing usable is left, which is the caller's cue to
   * fall back to the default palette file.
   */
  static normalizePresets(presets) {
    if (!Array.isArray(presets) || presets.length === 0) {
      return null;
    }
    const cleanedPresets = [];
    presets.forEach(entry => {
      const isPlainString = typeof entry === 'string';
      const rawColor = isPlainString ? entry : (entry && entry.color);
      if (typeof rawColor !== 'string') {
        return;
      }
      const trimmedColor = rawColor.trim();
      const hexColor = trimmedColor.startsWith('#') ? trimmedColor : `#${trimmedColor}`;
      if (!/^#[0-9A-F]{6}$/i.test(hexColor)) {
        return;
      }
      const displayName = isPlainString ? '' : ((entry && entry.name) || '');
      cleanedPresets.push({ color: hexColor.toUpperCase(), name: displayName });
    });
    return cleanedPresets.length > 0 ? cleanedPresets : null;
  }

  /**
   * Initializes the Color Picker instance.
   * @param {string} anchorElementId - The ID of the HTML element where the popup will be injected.
   * @param {Function} onColorChangeCallback - Function to call when the color changes (e.g., to send network commands).
   * @param {Object} [options] - Optional extras. Both are left out by the
   *        callers that only send a colour to a device, so the two-argument
   *        form keeps working exactly as it did.
   * @param {Array}  [options.presets] - Swatches to load into the palette in
   *        place of the default palette file. Hex strings, or
   *        { color, name } entries.
   * @param {string} [options.name] - What the colour is being picked for,
   *        put into the heading: 'LED' gives "Select LED Color". Left out,
   *        the heading stays "Select Color".
   * @param {Function} [options.onClose] - Called once when the prompt goes
   *        away, however it went: the close button, being replaced by
   *        another prompt, or the page being torn down. Theme uses it to
   *        keep the colour it has been showing live.
   */
  constructor(anchorElementId, onColorChangeCallback, options = {}) {
    this.anchorElement = document.getElementById(anchorElementId);
    if (!this.anchorElement) {
      throw new Error("Anchor element for ColorPrompt not found.");
    }

    this.onColorChangeCallback = onColorChangeCallback;
    const settings = options || {};
    // The caller's palette if it gave one. If it did not, the grid starts
    // empty and show() fetches the default palette file into it.
    this.presetColors = [];
    // Uppercase hex -> the palette's name for it, for the colours that were
    // given one. What the box under the grid shows is looked up in here.
    this.presetNames = new Map();
    this.setPresets(settings.presets);
    this.promptTitle = ColorPrompt.titleFor(settings.name);
    this.onCloseCallback =
      typeof settings.onClose === 'function' ? settings.onClose : null;
    // destroy() can be reached more than once; the close hook fires on the
    // first of those and never again
    this.isClosed = false;

    // State variables
    this.currentHue = 0;
    this.currentSaturation = 1;
    this.currentValue = 1;
    this.isDraggingMap = false;
    this.lastCommandTime = 0;

    // DOM Element References
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

    // Bound Event Handlers (Stored for memory management/removal)
    this.boundHandleHexInput = this.handleHexInput.bind(this);
    this.boundHandleHueInput = this.handleHueInput.bind(this);
    this.boundHandleHueRelease = this.handleHueRelease.bind(this);
    this.boundStartMapDrag = this.startMapDrag.bind(this);
    this.boundUpdateMapPointer = this.updateMapPointer.bind(this);
    this.boundStopMapDrag = this.stopMapDrag.bind(this);
    this.boundClosePopup = this.closePopup.bind(this);
  }

  /**
   * Builds the DOM structure and injects it into the anchor element.
   */
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
          <button type="button" class="button button_left_txtwin color-popup-btn" id="btn-color-close">
            <i class="fad fa-times-octagon"></i> Close
          </button>
        </div>
      </div>
    `;

    // Cache DOM references
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

    // Written in rather than built into the markup above, so a name never
    // reaches innerHTML -- the heading is text, and only ever text.
    if (this.titleElement) {
      this.titleElement.innerText = this.promptTitle;
    }

    this.buildPresetGrid();
    this.bindEvents();
    
    // Ensure anchor acts as popup container
    this.anchorElement.style.display = 'flex';

    // no palette from the caller: read the default one off the device
    if (this.presetColors.length === 0) {
      this.loadPresets();
    }
  }

  /**
   * Safely removes all event listeners and clears the DOM to prevent memory leaks.
   */
  destroy() {
    this.unbindEvents();
    // Ensure window listeners are definitely removed even if drag was active
    window.removeEventListener('pointermove', this.boundUpdateMapPointer);
    window.removeEventListener('pointerup', this.boundStopMapDrag);
    window.removeEventListener('pointercancel', this.boundStopMapDrag);
    
    this.anchorElement.innerHTML = '';
    this.anchorElement.style.display = 'none';

    // Told once, after the markup is gone, so a handler that opens something
    // of its own is not doing it into a prompt that is half torn down. The
    // reference is dropped before the call, so a handler that reaches back
    // in here cannot come round again.
    this.isClosed = true;
    if (this.onCloseCallback) {
      const closeCallback = this.onCloseCallback;
      this.onCloseCallback = null;
      closeCallback();
    }
  }

  /**
   * Takes a palette in either accepted shape and keeps it, along with the
   * name lookup the box under the grid reads. Does not draw -- callers
   * follow it with buildPresetGrid().
   */
  setPresets(presets) {
    this.presetColors = ColorPrompt.normalizePresets(presets) || [];
    this.presetNames.clear();
    this.presetColors.forEach(preset => {
      if (preset.name) {
        this.presetNames.set(preset.color, preset.name);
      }
    });
  }

  /**
   * Fills an empty grid from the default palette file. The prompt is already
   * on screen and usable while this is in the air; the swatches appear when
   * the file lands, and on every prompt after the first it is already held
   * and they appear with the rest of it.
   */
  async loadPresets() {
    const palette = await ColorPrompt.loadDefaultPalette();
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

  /**
   * Populates the grid of preset colors.
   */
  buildPresetGrid() {
    this.presetGridElement.innerHTML = ''; 
    this.presetColors.forEach(preset => {
      const colorHexCode = preset.color;
      const swatchElement = document.createElement('div');
      swatchElement.className = 'color-swatch';
      swatchElement.style.backgroundColor = colorHexCode;
      
      swatchElement.addEventListener('click', () => {
        const updateMapUI = true;
        const triggerCallback = true;
        this.updateStateFromHex(colorHexCode, updateMapUI, triggerCallback);
      });
      
      this.presetGridElement.appendChild(swatchElement);
    });
    // an empty grid is a bordered box with nothing in it, so it stands down
    // until there is a palette to put in it
    this.presetGridElement.style.display =
      this.presetColors.length > 0 ? 'grid' : 'none';
  }

  /**
   * Attaches local DOM event listeners.
   */
  bindEvents() {
    this.hexInputElement.addEventListener('input', this.boundHandleHexInput);
    this.hueSliderElement.addEventListener('input', this.boundHandleHueInput);
    // 'change' fires once on release, and forces the value the slider came to
    // rest on through the throttle -- the same thing stopMapDrag() does for
    // the map. Without it a release inside the throttle window was dropped
    // and the lights kept the second-to-last hue.
    this.hueSliderElement.addEventListener('change', this.boundHandleHueRelease);
    this.colorMapElement.addEventListener('pointerdown', this.boundStartMapDrag);
    this.closeButtonElement.addEventListener('click', this.boundClosePopup);
  }

  /**
   * Detaches local DOM event listeners.
   */
  unbindEvents() {
    this.hexInputElement.removeEventListener('input', this.boundHandleHexInput);
    this.hueSliderElement.removeEventListener('input', this.boundHandleHueInput);
    this.hueSliderElement.removeEventListener('change', this.boundHandleHueRelease);
    this.colorMapElement.removeEventListener('pointerdown', this.boundStartMapDrag);
    this.closeButtonElement.removeEventListener('click', this.boundClosePopup);
  }

  /**
   * Closes and destroys the popup.
   */
  closePopup() {
    this.destroy();
  }

  /**
   * Handles user typing into the hexadecimal input field.
   */
  handleHexInput(event) {
    let inputValue = event.target.value;
    
    const isMissingHash = !inputValue.startsWith('#');
    if (isMissingHash) {
      inputValue = '#' + inputValue;
    }
    
    const isValidHexPattern = /^#[0-9A-F]{6}$/i.test(inputValue);
    if (isValidHexPattern) {
      const updateMapUI = true;
      const triggerCallback = true;
      this.updateStateFromHex(inputValue, updateMapUI, triggerCallback);
    }
  }

  /**
   * Handles user dragging the hue slider.
   */
  handleHueInput(event) {
    const newHueValue = parseFloat(event.target.value);
    this.currentHue = newHueValue;
    this.colorMapBackgroundElement.style.background = `hsl(${this.currentHue}, 100%, 50%)`;
    this.updateUIFromState(true);
  }

  /**
   * The hue slider was released: send the resting value even if the last
   * 'input' fell inside the throttle window.
   */
  handleHueRelease(event) {
    this.currentHue = parseFloat(event.target.value);
    this.colorMapBackgroundElement.style.background = `hsl(${this.currentHue}, 100%, 50%)`;
    this.lastCommandTime = 0;              // force the final color through
    this.updateUIFromState(true);
  }

  /**
   * Initiates the color map drag interaction.
   */
  startMapDrag(event) {
    this.isDraggingMap = true;
    this.updateMapPointer(event);
    
    window.addEventListener('pointermove', this.boundUpdateMapPointer);
    window.addEventListener('pointerup', this.boundStopMapDrag);
    window.addEventListener('pointercancel', this.boundStopMapDrag);
  }

  /**
   * Calculates new saturation and value based on pointer coordinates.
   * Broken down into explicit intermediate variables for clarity.
   */
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
    const valueFraction = 1 - (clampedPointerY / mapHeight);
    
    const pointerLeftPercentage = saturationFraction * 100;
    const pointerTopPercentage = (clampedPointerY / mapHeight) * 100;
    
    this.colorPointerElement.style.left = `${pointerLeftPercentage}%`;
    this.colorPointerElement.style.top = `${pointerTopPercentage}%`;
    
    this.currentSaturation = saturationFraction;
    this.currentValue = valueFraction;
    
    const triggerCallback = true;
    this.updateUIFromState(triggerCallback);
  }

  /**
   * Ends the color map drag interaction.
   */
  stopMapDrag() {
    this.isDraggingMap = false;
    this.lastCommandTime = 0;              // force the final color through
    this.updateUIFromState(true);
    window.removeEventListener('pointermove', this.boundUpdateMapPointer);
    window.removeEventListener('pointerup', this.boundStopMapDrag);
    window.removeEventListener('pointercancel', this.boundStopMapDrag);
  }

  /**
   * Syncs internal state from a given Hex color.
   */
  updateStateFromHex(hexString, updateMapUI = true, triggerCallback = true) {
    const rgbObject = hexToRgb(hexString);
    const hsvObject = rgbToHsv(rgbObject.red, rgbObject.green, rgbObject.blue);
    
    this.currentHue = hsvObject.hue;
    this.currentSaturation = hsvObject.saturation;
    this.currentValue = hsvObject.value;
    
    if (updateMapUI) {
      this.hueSliderElement.value = this.currentHue;
      this.colorMapBackgroundElement.style.background = `hsl(${this.currentHue}, 100%, 50%)`;
      
      const pointerLeftPercentage = this.currentSaturation * 100;
      const pointerTopPercentage = (1 - this.currentValue) * 100;
      
      this.colorPointerElement.style.left = `${pointerLeftPercentage}%`;
      this.colorPointerElement.style.top = `${pointerTopPercentage}%`;
    }
    
    // the box is written by updateUIFromState() on the next line, which
    // knows whether this colour has a name; setting it here as well only put
    // a value in that was immediately replaced
    this.updateUIFromState(triggerCallback);
  }

  /**
   * Updates swatches and triggers the network callback if requested.
   */
  updateUIFromState(triggerCallback = true) {
    const rgbObject = hsvToRgb(this.currentHue, this.currentSaturation, this.currentValue);
    const hexString = rgbToHex(rgbObject.red, rgbObject.green, rgbObject.blue);
    
    this.showColorField(hexString);
    
    // Highlight matching preset swatch
    const allSwatches = this.anchorElement.querySelectorAll('.color-swatch');
    allSwatches.forEach(swatchElement => {
      const swatchColor = rgbToHexFromStyle(swatchElement.style.backgroundColor);
      if (swatchColor === hexString) {
        swatchElement.classList.add('active-swatch');
      } else {
        swatchElement.classList.remove('active-swatch');
      }
    });

    if (triggerCallback) {
      this.dispatchColorCommand(hexString);
    }
  }

  /**
   * The box under the palette. It is the hex value, as it always was, until
   * the colour on screen is one the palette has a name for -- then the name
   * is shown in its place and the label beside it changes with it.
   *
   * The map and the hue slider reach colours no palette has a name for, so
   * the hex is what the box falls back to rather than emptying: it is still
   * the only way to read an arbitrary colour off the prompt, and still the
   * box that can be typed into.
   */
  showColorField(hexString) {
    const presetName = this.presetNames.get(hexString.toUpperCase());
    const hasName = typeof presetName === 'string' && presetName !== '';
    const fieldText = hasName ? presetName : hexString;
    this.hexInputElement.value = fieldText;
    // The box is sized to what it is holding rather than to a fixed width:
    // a hex is always seven characters, a name is however long the palette
    // file made it. One spare character keeps the text off the border.
    this.hexInputElement.size = fieldText.length + 1;
    this.hexInputElement.classList.toggle(ColorPrompt.NAME_SHOWN_CLASS, hasName);
    if (this.hexLabelElement) {
      this.hexLabelElement.innerText =
        hasName ? ColorPrompt.NAME_LABEL : ColorPrompt.HEX_LABEL;
    }
  }

  /**
   * Safely throttles the color command using timestamps to prevent race conditions.
   */
  dispatchColorCommand(hexString) {
    if (typeof this.onColorChangeCallback !== 'function') {
      return;
    }

    const currentTime = Date.now();
    const timeSinceLastCommand = currentTime - this.lastCommandTime;
    
    const isReadyToSend = timeSinceLastCommand >= ColorPrompt.THROTTLE_DELAY_MS;
    
    if (isReadyToSend) {
      this.lastCommandTime = currentTime;
      const strippedHex = hexString.replace('#', '');
      this.onColorChangeCallback(strippedHex);
    }
  }

}
