import { sendCmd } from './utils.js';

export default class ToggleSwitch {
  constructor(parent, config = {}) {
    this.parent = typeof parent === 'string' ? document.getElementById(parent) : parent;
    if (!this.parent) throw new Error(`Parent element not found`);
    this.config = config;
    // a template literal is always truthy, so the fallback below used to be
    // unreachable and an id-less switch became "input-undefined" -- two of
    // those on one page would share an id and one label would drive both
    this.toggleId = config.id
      ? `input-${config.id}`
      : `input-${Math.random().toString(36).substring(2, 11)}`;
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
    // One frame each, rather than one field shared between them: a pending
    // move frame used to be overwritten by the release frame, so destroy()
    // could only ever cancel whichever happened to be current.
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
    window.removeEventListener('pointermove', this);
    window.removeEventListener('pointerup', this);
    window.removeEventListener('pointercancel', this);
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

  _onPointerMove(e) {
    if (!this.state.isDragging) return;
    const deltaX = e.clientX - this.state.startX;
    if (Math.abs(deltaX) > 3) {
      this.state.hasMoved = true;
      if (this.state.handleTravelDistance === 0) {
        const trackRect = this.label.getBoundingClientRect();
        this.state.height = trackRect.height;
        this.state.handleSize = this.state.height * 0.85;
        this.state.padding = this.state.height * 0.075;
        this.state.handleTravelDistance = 
          trackRect.width - this.state.handleSize - (2 * this.state.padding);
      }
      let progress = this.state.startProgress + (deltaX / this.state.handleTravelDistance);
      this.state.dragProgress = Math.max(0, Math.min(1, progress));
      let currentX = this.state.padding + (this.state.dragProgress * this.state.handleTravelDistance);
      if (!this.ticking) {
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
    }
  }

  _onPointerUp(e) {
    if (!this.state.isDragging) return;
    this.state.isDragging = false;
    window.removeEventListener('pointermove', this);
    window.removeEventListener('pointerup', this);
    window.removeEventListener('pointercancel', this);
    if (this.state.hasMoved) {
      const newState = this.state.dragProgress >= 0.5;
      if (this.checkbox.checked !== newState) {
        this.checkbox.checked = newState; 
        this.checkbox.dispatchEvent(new Event('change')); 
      }
      this.state.blockNextClick = true;
      this.clickBlockTimeout = setTimeout(() => { this.state.blockNextClick = false; }, 300);
      this.wrapper.style.setProperty('--drag-progress', newState ? '1' : '0');
      this.upRafId = window.requestAnimationFrame(() => {
        this.upRafId = window.requestAnimationFrame(() => {
          this.wrapper.classList.remove('is-dragging');
          this.wrapper.style.removeProperty('--drag-progress'); 
          this.wrapper.style.removeProperty('--drag-x'); 
        });
      });
    } else {
      this.wrapper.classList.remove('is-dragging');
      this.wrapper.style.removeProperty('--drag-progress'); 
      this.wrapper.style.removeProperty('--drag-x'); 
    }
    this.state.hasMoved = false;
  }

  _onChange(e) {
    const toggleState = e.target.checked ? 'on' : 'off';
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

export async function toggleSwitchCallback(payload) {
  sendCmd('main', payload.host, payload.cmd);
}