// Automate Website - toggle switch
//
// A checkbox drawn as a sliding switch that can also be dragged across.
// new ToggleSwitch(parentElement, { id, checked, cmdHost, command, onToggle })
// then .activate() to bind, .destroy() to unbind and remove.
// onToggle is handed { host, cmd }, where cmd is the command plus 'on' / 'off'.

import { sendCmd } from './utils.js';

const DRAG_THRESHOLD_PX = 3;   // movement before a press counts as a drag
const HANDLE_HEIGHT_RATIO = 0.85;
const HANDLE_PAD_RATIO = 0.075;
const CLICK_BLOCK_MS = 300;    // a drag swallows the click that follows it

export default class ToggleSwitch {

  constructor(parent, config = {}) {
    this.parent = parent;
    if (typeof parent === 'string') {
      this.parent = document.getElementById(parent);
    }
    if (!this.parent) throw new Error(`Parent element not found`);
    this.config = config;
    // a template literal is always truthy, so an id-less switch used to
    // become "input-undefined" -- two of those on one page would share an id
    // and one label would drive both
    if (config.id) {
      this.toggleId = `input-${config.id}`;
    } else {
      this.toggleId = `input-${Math.random().toString(36).substring(2, 11)}`;
    }
    this.cmdHost = config.cmdHost || 'none';
    this.command = config.command || 'none';
    this.onToggleCallback = config.onToggle || null;
    this.state = {
      isDragging: false,
      hasMoved: false,
      blockNextClick: false,
      startX: 0,
      startProgress: 0,
      dragProgress: 0,
      handleTravelDistance: 0,
      height: 0,
      handleSize: 0,
      padding: 0
    };
    this.ticking = false;
    // One frame each: a pending move frame used to be overwritten by the
    // release frame, so destroy() could only cancel whichever was current.
    this.moveRafId = null;
    this.upRafId = null;
    this.clickBlockTimeout = null;
    this._render();
  }

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
    // paints the "on" colour once with transitions off, so a switch drawn in
    // the on state does not animate into it
    const track = this.wrapper.querySelector('.toggle-track');
    track.style.transition = 'none';
    track.style.backgroundColor = 'var(--local-color-on)';
    void track.offsetHeight;
    track.style.backgroundColor = '';
    track.style.transition = '';
  }

  activate() {
    this.label.addEventListener('pointerdown', this);
    this.label.addEventListener('click', this);
    this.checkbox.addEventListener('change', this);
  }

  destroy() {
    this.label.removeEventListener('pointerdown', this);
    this.label.removeEventListener('click', this);
    this.checkbox.removeEventListener('change', this);
    this._releasePointer();
    if (this.moveRafId) cancelAnimationFrame(this.moveRafId);
    if (this.upRafId) cancelAnimationFrame(this.upRafId);
    if (this.clickBlockTimeout) clearTimeout(this.clickBlockTimeout);
    if (this.wrapper && this.wrapper.parentNode) {
      this.wrapper.parentNode.removeChild(this.wrapper);
    }
  }

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

  // How far across the track the handle can travel: the label's width less
  // the handle itself and the padding at both ends. Measured on the first
  // move of a drag, when the switch is known to be laid out.
  _measureTravel() {
    const trackRect = this.label.getBoundingClientRect();
    this.state.height = trackRect.height;
    this.state.handleSize = this.state.height * HANDLE_HEIGHT_RATIO;
    this.state.padding = this.state.height * HANDLE_PAD_RATIO;
    const bothEdges = 2 * this.state.padding;
    this.state.handleTravelDistance = trackRect.width - this.state.handleSize - bothEdges;
  }

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

  _onPointerUp(e) {
    if (!this.state.isDragging) return;
    this.state.isDragging = false;
    this._releasePointer();
    if (this.state.hasMoved) {
      // past the half way point is on
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
      // two frames: the committed value is painted, then the drag styling is
      // dropped, so the handle does not jump back before the class goes
      this.upRafId = window.requestAnimationFrame(() => {
        this.upRafId = window.requestAnimationFrame(() => this._clearDragStyle());
      });
    } else {
      this._clearDragStyle();
    }
    this.state.hasMoved = false;
  }

  _onChange(e) {
    let toggleState = 'off';
    if (e.target.checked) {
      toggleState = 'on';
    }
    const payload = {
      host: this.cmdHost,
      cmd: `${this.command}${toggleState}`
    };
    if (typeof this.onToggleCallback === 'function') {
      this.onToggleCallback(payload);
    }
  }

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

// default action for a menu toggle: send the command to the device
export async function toggleSwitchCallback(payload) {
  sendCmd('main', payload.host, payload.cmd);
}
