// Automate - a checkbox drawn as a sliding switch that can also be dragged across.
// new ToggleSwitch(parentElement, { id, checked, cmdHost, command, onToggle }) then
// .activate() to bind, .destroy() to unbind and remove. onToggle is handed
// { host, cmd }, cmd being the command plus 'on' / 'off'.

import { sendCmd } from './utils.js';

const DRAG_THRESHOLD_PX = 3;
const HANDLE_HEIGHT_RATIO = 0.85;
const HANDLE_PAD_RATIO = 0.075;
const CLICK_BLOCK_MS = 300; // a drag swallows the click that follows it

export default class ToggleSwitch {

  // draws the switch; see the top of this file
  constructor(parent, config) {
    this.parent = parent;
    this.config = config;
    this.toggleId = `input-${config.id}`;
    this.cmdHost = config.cmdHost;
    this.command = config.command;
    this.onToggleCallback = config.onToggle;
    this.state = {
      isDragging: false,
      hasMoved: false,
      blockNextClick: false,
      startX: 0,
      startProgress: 0,
      dragProgress: 0,
      handleTravelDistance: 0,
    };
    this.ticking = false;
    this.moveRafId = null;
    this.upRafId = null;
    this.clickBlockTimeout = null;
    this._render();
  }

  // builds the markup
  _render() {
    this.wrapper = document.createElement('div');
    this.wrapper.className = 'toggle-switch';
    this.wrapper.classList.add('nohide');
    this.wrapper.style.contain = 'layout paint';
    if (this.config.id) this.wrapper.id = this.config.id;
    if (this.config.width) this.wrapper.style.setProperty('--local-width', this.config.width);
    if (this.config.height) this.wrapper.style.setProperty('--local-height', this.config.height);
    this.wrapper.innerHTML = `
      <input type="checkbox" id="${this.toggleId}" class="toggle-input" ${this.config.checked ? 'checked' : ''}>
      <label for="${this.toggleId}" class="toggle-label">
          <div class="toggle-track">
              <div class="toggle-handle">
                  <span class="toggle-text-on">ON</span>
                  <span class="toggle-text-off">OFF</span>
              </div>
          </div>
      </label>
    `;
    this.parent.appendChild(this.wrapper);
    this.checkbox = this.wrapper.querySelector('.toggle-input');
    this.label = this.wrapper.querySelector('.toggle-label');
  }

  // binds the switch's events
  activate() {
    this.label.addEventListener('pointerdown', this);
    this.label.addEventListener('click', this);
    this.checkbox.addEventListener('change', this);
  }

  // unbinds the events and removes the switch
  destroy() {
    this.label.removeEventListener('pointerdown', this);
    this.label.removeEventListener('click', this);
    this.checkbox.removeEventListener('change', this);
    this._releasePointer();
    if (this.moveRafId) cancelAnimationFrame(this.moveRafId);
    if (this.upRafId) cancelAnimationFrame(this.upRafId);
    if (this.clickBlockTimeout) clearTimeout(this.clickBlockTimeout);
    this.wrapper.remove();
  }

  // routes the bound events
  handleEvent(event) {
    switch (event.type) {
      case 'pointercancel':
      case 'pointerup': this._onPointerUp(event); break;
      case 'pointerdown': this._onPointerDown(event); break;
      case 'pointermove': this._onPointerMove(event); break;
      case 'click': this._onClick(event); break;
      case 'change': this._onChange(event); break;
    }
  }

  // stops following the pointer
  _releasePointer() {
    window.removeEventListener('pointermove', this);
    window.removeEventListener('pointerup', this);
    window.removeEventListener('pointercancel', this);
  }

  // drops the drag styling, leaving the switch on its committed state
  _clearDragStyle() {
    this.wrapper.classList.remove('is-dragging');
    this.wrapper.style.removeProperty('--drag-progress');
    this.wrapper.style.removeProperty('--drag-x');
  }

  // a press: starts following the pointer
  _onPointerDown(e) {
    if (e.button !== 0 && e.pointerType === 'mouse') return;
    this.state.blockNextClick = false;
    clearTimeout(this.clickBlockTimeout);
    this.state.handleTravelDistance = 0;
    this.state.startX = e.clientX;
    this.state.isDragging = true;
    this.state.hasMoved = false;
    this.state.startProgress = this.checkbox.checked ? 1 : 0;
    window.addEventListener('pointermove', this, { passive: true });
    window.addEventListener('pointerup', this);
    window.addEventListener('pointercancel', this);
  }

  // How far the handle can travel: the label's width less the handle and the padding at
  // both ends. Measured on a drag's first move, when the switch is laid out.
  _measureTravel() {
    const trackRect = this.label.getBoundingClientRect();
    const height = trackRect.height;
    const handleSize = height * HANDLE_HEIGHT_RATIO;
    this.state.padding = height * HANDLE_PAD_RATIO;
    const bothEdges = 2 * this.state.padding;
    this.state.handleTravelDistance = trackRect.width - handleSize - bothEdges;
  }

  // a drag: moves the handle once per frame
  _onPointerMove(e) {
    if (!this.state.isDragging) return;
    const deltaX = e.clientX - this.state.startX;
    if (Math.abs(deltaX) <= DRAG_THRESHOLD_PX) return;
    this.state.hasMoved = true;
    if (this.state.handleTravelDistance === 0) {
      this._measureTravel();
    }
    const travelled = deltaX / this.state.handleTravelDistance;
    const progress = this.state.startProgress + travelled;
    this.state.dragProgress = Math.max(0, Math.min(1, progress));
    const handleOffset = this.state.dragProgress * this.state.handleTravelDistance;
    const currentX = this.state.padding + handleOffset;
    if (this.ticking) return;
    this.moveRafId = window.requestAnimationFrame(() => {
      if (!this.state.isDragging) {
        this.ticking = false;
        return;
      }
      if (!this.wrapper.classList.contains('is-dragging')) {
        this.wrapper.classList.add('is-dragging');
        void this.wrapper.offsetWidth;
      }
      this.wrapper.style.setProperty('--drag-progress', this.state.dragProgress.toFixed(3));
      this.wrapper.style.setProperty('--drag-x', `${currentX.toFixed(1)}px`);
      this.ticking = false;
    });
    this.ticking = true;
  }

  // a release: past half way is on, and the drag swallows its click
  _onPointerUp(e) {
    if (!this.state.isDragging) return;
    this.state.isDragging = false;
    this._releasePointer();
    if (this.state.hasMoved) {
      const newState = this.state.dragProgress >= 0.5;
      if (this.checkbox.checked !== newState) {
        this.checkbox.checked = newState;
        this.checkbox.dispatchEvent(new Event('change'));
      }
      this.state.blockNextClick = true;
      this.clickBlockTimeout = setTimeout(() => {
        this.state.blockNextClick = false;
      }, CLICK_BLOCK_MS);
      this.wrapper.style.setProperty('--drag-progress', newState ? '1' : '0');
      // two frames: the committed value paints, then the drag styling goes, so the
      // handle does not jump back first
      this.upRafId = window.requestAnimationFrame(() => {
        this.upRafId = window.requestAnimationFrame(() => this._clearDragStyle());
      });
    } else {
      this._clearDragStyle();
    }
    this.state.hasMoved = false;
  }

  // the checkbox changed: reports it
  _onChange(e) {
    let toggleState = 'off';
    if (e.target.checked) {
      toggleState = 'on';
    }
    const payload = {
      host: this.cmdHost,
      cmd: `${this.command}${toggleState}`
    };
    this.onToggleCallback(payload);
  }

  // a click flips the switch, unless a drag just ended
  _onClick(e) {
    e.preventDefault();
    e.stopPropagation();
    if (this.state.blockNextClick) {
      this.state.blockNextClick = false;
      clearTimeout(this.clickBlockTimeout);
      return;
    }
    this.checkbox.checked = !this.checkbox.checked;
    this.checkbox.dispatchEvent(new Event('change'));
  }
}

// default action for a menu toggle: sends the command to the device
export function toggleSwitchCallback(payload) {
  sendCmd('main', payload.host, payload.cmd);
}
