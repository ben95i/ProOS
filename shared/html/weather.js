// Automate Website - weather widget
//
// The title bar icon and the drop-down behind it. new WeatherWidget() then
// .activate() to draw and start polling, .remove() to take it away.
// The API answers { location, temperature, condition, humidity, timeOfDay }.

import { makeElement } from './utils.js';
import { logError } from './error.js';

const REFRESH_MS = 60 * 1000; // one minute
const ANCHOR_ID = 'weather_anchor';

// condition text -> the icon that stands for it. Checked in order, so the
// first match wins; day / night pairs pick by the reading's timeOfDay.
const CONDITION_ICONS = [
  {
    match: ['sun', 'clear'],
    icon: 'fad fa-sun',
    byTime: { sunset: 'fad fa-sunset', night: 'fad fa-moon-stars' }
  },
  { match: ['cloud'], icon: 'fad fa-clouds', byTime: { night: 'fad fa-cloud-moon' } },
  { match: ['rain', 'drizzle'], icon: 'fad fa-cloud-rain' },
  { match: ['snow'], icon: 'fad fa-cloud-snow' },
  { match: ['storm', 'thunder'], icon: 'fad fa-thunderstorm' }
];
const DEFAULT_ICON = 'fad fa-clouds';
const ERROR_ICON = 'fad fa-exclamation-triangle';

export default class WeatherWidget {

  constructor(apiUrl = '/api?var=&arg=weather&action=main') {
    this.apiUrl = apiUrl;
    this.refreshInterval = REFRESH_MS;
    this.timerId = null;
    this.weatherData = null;
    this.container = null;
    this.dropdown = null;
    this.isOpen = false;
    // set by remove(); a reply that lands afterwards is dropped rather than
    // written into elements the page has already taken away
    this.removed = false;
    // bumped per request: the auto-refresh and opening the menu can each have
    // one in the air, and the slower of the two used to repaint over the
    // fresher answer
    this.requestGen = 0;
    this._handleOutsideClick = this._handleOutsideClick.bind(this);
    this.toggleMenu = this.toggleMenu.bind(this);
  }

  activate() {
    if (this.container) return;
    this._renderIcon();
    // no anchor on this page: nothing was drawn, so nothing is polled either
    if (!this.container) return;
    this.removed = false;
    this._renderDropdown();
    this.fetchWeather();
    this.timerId = setInterval(() => this.fetchWeather(), this.refreshInterval);
    document.addEventListener('click', this._handleOutsideClick);
  }

  closeMenu() {
    if (this.isOpen && this.dropdown) {
      this.dropdown.style.display = 'none';
      this.isOpen = false;
    }
  }

  remove() {
    this.removed = true;
    // anything still in flight is dropped by the generation check as well
    this.requestGen++;
    if (this.timerId) {
      clearInterval(this.timerId);
      this.timerId = null;
    }
    if (this.container) {
      this.container.remove();
      this.container = null;
    }
    if (this.dropdown) {
      this.dropdown.remove();
      this.dropdown = null;
    }
    document.removeEventListener('click', this._handleOutsideClick);
  }

  // this answer is no longer the current one: torn down, or overtaken
  _stale(gen) {
    return this.removed || gen !== this.requestGen;
  }

  _setSpinner(on) {
    if (!this.iconElement || !this.spinnerElement) return;
    if (on === true) {
      this.iconElement.style.display = 'none';
      this.spinnerElement.style.display = 'inline-block';
    } else {
      this.spinnerElement.style.display = 'none';
      this.iconElement.style.display = '';
    }
  }

  async fetchWeather() {
    if (this.removed) return;
    const gen = ++this.requestGen;
    this._setSpinner(true);
    try {
      const response = await fetch(this.apiUrl);
      if (!response.ok) throw new Error('API Network Error');
      const data = await response.json();
      if (this._stale(gen)) return;
      this.weatherData = data;
      this._updateWeatherUI();
    } catch (error) {
      if (this._stale(gen)) return;
      logError('reading the weather', error);
      this._setSpinner(false);
      if (this.iconElement) {
        this.iconElement.className = ERROR_ICON;
      }
      if (this.badgeElement) {
        this.badgeElement.style.display = 'none';
      }
      this._updateDropdownContentError();
    }
  }

  // the icon for the reading currently held
  _weatherIcon() {
    const condition = (this.weatherData.condition || '').toLowerCase();
    const time = this.weatherData.timeOfDay || 'day';
    for (const entry of CONDITION_ICONS) {
      const hit = entry.match.some(word => condition.includes(word));
      if (!hit) continue;
      if (entry.byTime && entry.byTime[time]) {
        return entry.byTime[time];
      }
      return entry.icon;
    }
    return DEFAULT_ICON;
  }

  _updateWeatherUI() {
    if (!this.weatherData) return;
    this._setSpinner(false);
    this.iconElement.className = this._weatherIcon();
    if (this.weatherData.temperature !== undefined) {
      this.badgeElement.innerText = `${this.weatherData.temperature}\u00B0`;
      this.badgeElement.style.display = 'block';
    }
    this._updateDropdownContent();
  }

  _renderIcon() {
    this.container = document.getElementById(ANCHOR_ID);
    if (!this.container) {
      logError('drawing the weather icon', `${ANCHOR_ID} not found in the DOM.`);
      return;
    }
    this.iconElement = makeElement('i', DEFAULT_ICON);
    this.badgeElement = makeElement('span', 'weather-badge');
    this.badgeElement.style.display = 'none';
    this.spinnerElement = makeElement('span', 'right-nav-spinner');
    this.spinnerElement.style.display = 'none';
    this.container.innerHTML = '';
    this.container.appendChild(this.iconElement);
    this.container.appendChild(this.spinnerElement);
    this.container.appendChild(this.badgeElement);
    this.container.classList.add('weather-icon-container');
    this.container.addEventListener('click', this.toggleMenu);
  }

  _renderDropdown() {
    this.dropdown = makeElement('div', 'dd-content dd-menu weather-dropdown',
      '<a>Loading...</a>');
    this.dropdown.id = 'weatherMenu';
    this.dropdown.style.display = 'none';
    document.body.appendChild(this.dropdown);
  }

  // one row of the drop-down: an icon and its text
  _menuRow(icon, text) {
    return makeElement('a', '',
      `<i class="${icon}" style="width: 25px; text-align: center;"></i> ${text}`);
  }

  _updateDropdownContent() {
    if (!this.dropdown || !this.weatherData) return;
    const reading = this.weatherData;
    this.dropdown.innerHTML = '';
    this.dropdown.appendChild(
      this._menuRow('fad fa-map-marker-alt', reading.location || 'Unknown'));
    this.dropdown.appendChild(this._menuRow('fad fa-thermometer-half',
      `${reading.temperature}\u00B0   ${reading.condition}`));
    this.dropdown.appendChild(
      this._menuRow('fad fa-humidity', `${reading.humidity}% Humidity`));
  }

  _updateDropdownContentError() {
    if (!this.dropdown) return;
    this.dropdown.innerHTML = '';
    this.dropdown.appendChild(makeElement('a', '',
      `<i class="${ERROR_ICON}" style="color: var(--red-ind);"></i> Error Loading Weather`));
  }

  // places the drop-down under the icon and shows it
  _placeDropdown() {
    const rect = this.container.getBoundingClientRect();
    const style = this.dropdown.style;
    style.position = 'fixed';
    style.top = `${rect.bottom + 5}px`;
    style.left = `${rect.left + (rect.width / 2)}px`;
    style.transform = 'translateX(-80%)';
    style.zIndex = '9999';
    style.display = 'block';
  }

  toggleMenu(event) {
    if (event) event.stopPropagation();
    if (this.isOpen) {
      this.closeMenu();
      return;
    }
    if (typeof window.removeDropdowns === 'function') {
      window.removeDropdowns();
    }
    this.fetchWeather();
    this._placeDropdown();
    this.isOpen = true;
  }

  _handleOutsideClick(event) {
    if (!this.isOpen || !this.container) return;
    if (this.container.contains(event.target)) return;
    if (this.dropdown.contains(event.target)) return;
    this.closeMenu();
  }
}
