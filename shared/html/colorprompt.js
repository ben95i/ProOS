/**
 * ColorPrompt
 * Encapsulates the UI, state, and lifecycle of the color selection prompt.
 */
import { hexToRgb, rgbToHex, rgbToHexFromStyle, 
    hsvToRgb, rgbToHsv } from './utils.js';

export default class ColorPrompt {

  static PRESET_COLORS = [
    '#FF0000', '#FF7F00', '#FFFF00', '#00FF00', '#00FFFF',
    '#0000FF', '#7F00FF', '#FF00FF', '#FFFFFF', '#000000'
  ];
  static THROTTLE_DELAY_MS = 50;

  /**
   * Initializes the Color Picker instance.
   * @param {string} anchorElementId - The ID of the HTML element where the popup will be injected.
   * @param {Function} onColorChangeCallback - Function to call when the color changes (e.g., to send network commands).
   */
  constructor(anchorElementId, onColorChangeCallback) {
    this.anchorElement = document.getElementById(anchorElementId);
    if (!this.anchorElement) {
      throw new Error("Anchor element for ColorPrompt not found.");
    }

    this.onColorChangeCallback = onColorChangeCallback;

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
    this.hexInputElement = null;
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
        <div class="color-popup-title">Select Color</div>
        
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
          <label for="hex-color">Hex:</label>
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
    this.colorMapElement           = this.anchorElement.querySelector('#color-map');
    this.colorMapBackgroundElement = this.anchorElement.querySelector('#color-map-bg');
    this.colorPointerElement       = this.anchorElement.querySelector('#color-pointer');
    this.hueSliderElement          = this.anchorElement.querySelector('#slider-hue');
    this.presetGridElement         = this.anchorElement.querySelector('#preset-color-grid');
    this.hexInputElement           = this.anchorElement.querySelector('#hex-color');
    this.closeButtonElement        = this.anchorElement.querySelector('#btn-color-close');

    this.buildPresetGrid();
    this.bindEvents();
    
    // Ensure anchor acts as popup container
    this.anchorElement.style.display = 'flex';
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
  }

  /**
   * Populates the grid of preset colors.
   */
  buildPresetGrid() {
    this.presetGridElement.innerHTML = ''; 
    ColorPrompt.PRESET_COLORS.forEach(colorHexCode => {
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
    
    this.hexInputElement.value = hexString.toUpperCase();
    this.updateUIFromState(triggerCallback);
  }

  /**
   * Updates swatches and triggers the network callback if requested.
   */
  updateUIFromState(triggerCallback = true) {
    const rgbObject = hsvToRgb(this.currentHue, this.currentSaturation, this.currentValue);
    const hexString = rgbToHex(rgbObject.red, rgbObject.green, rgbObject.blue);
    
    this.hexInputElement.value = hexString;
    
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
