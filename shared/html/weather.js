// Automate - weather icon in the title bar and the drop-down behind it.
// new WeatherWidget() then .activate() to draw and poll, .remove() to take it away.
// The API answers { location, temperature, condition, humidity, timeOfDay }.

import { makeElement } from './utils.js';
import { logError } from './error.js';
import { removeDropdowns } from './dropdowns.js';

const REFRESH_MS = 60 * 1000;
const ANCHOR_ID = 'weather_anchor';
const API_URL = '/api?var=&arg=weather&action=main';

// Condition text -> its icon, first match winning; day / night pairs pick by timeOfDay.
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

  // set up by activate()
  constructor() {
    this.timerId = null;
    this.weatherData = null;
    this.container = null;
    this.dropdown = null;
    this.isOpen = false;
    // set by remove(); a late reply is dropped
    this.removed = false;
    // bumped per request, so the slower of two cannot repaint over the fresher
    this.requestGen = 0;
    // when the last read went out, so a page coming back can tell if it missed one
    this.lastReadTime = 0;
    this._handleVisibility = this._handleVisibility.bind(this);
    this._handleOutsideClick = this._handleOutsideClick.bind(this);
    this.toggleMenu = this.toggleMenu.bind(this);
  }

  // draws the icon and starts polling
  activate() {
    this._renderIcon();
    this._renderDropdown();
    this.fetchWeather();
    this.timerId = setInterval(() => this._pollWeather(), REFRESH_MS);
    document.addEventListener('visibilitychange', this._handleVisibility);
    document.addEventListener('click', this._handleOutsideClick);
  }

  // the minute's read; nobody sees it while the page is hidden
  _pollWeather() {
    if (!document.hidden) {
      this.fetchWeather();
    }
  }

  // the page came back: reads now if it missed its minute
  _handleVisibility() {
    if (document.hidden) {
      return;
    }
    const readAge = Date.now() - this.lastReadTime;
    if (readAge >= REFRESH_MS) {
      this.fetchWeather();
    }
  }

  // hides the drop-down
  closeMenu() {
    if (this.isOpen && this.dropdown) {
      this.dropdown.style.display = 'none';
      this.isOpen = false;
    }
  }

  // takes the widget away
  remove() {
    this.removed = true;
    // anything in flight is dropped by stale() as well
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
    document.removeEventListener('visibilitychange', this._handleVisibility);
    document.removeEventListener('click', this._handleOutsideClick);
  }

  // is this answer overtaken: removed, or a newer request
  _stale(gen) {
    return this.removed || gen !== this.requestGen;
  }

  // swaps the icon for its spinner
  _setSpinner(on) {
    if (on === true) {
      this.iconElement.style.display = 'none';
      this.spinnerElement.style.display = 'inline-block';
    } else {
      this.spinnerElement.style.display = 'none';
      this.iconElement.style.display = '';
    }
  }

  // reads the weather and shows it, or the error
  async fetchWeather() {
    if (this.removed) return;
    const gen = ++this.requestGen;
    this.lastReadTime = Date.now();
    this._setSpinner(true);
    try {
      const response = await fetch(API_URL);
      if (!response.ok) throw new Error('API Network Error');
      const data = await response.json();
      if (this._stale(gen)) return;
      this.weatherData = data;
      this._updateWeatherUI();
    } catch (error) {
      if (this._stale(gen)) return;
      logError('reading the weather', error);
      this._setSpinner(false);
      this.iconElement.className = ERROR_ICON;
      this.badgeElement.style.display = 'none';
      this._updateDropdownContentError();
    }
  }

  // the icon for the reading held
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

  // paints the reading on the icon, badge and drop-down
  _updateWeatherUI() {
    this._setSpinner(false);
    this.iconElement.className = this._weatherIcon();
    if (this.weatherData.temperature !== undefined) {
      this.badgeElement.innerText = `${this.weatherData.temperature}\u00B0`;
      this.badgeElement.style.display = 'block';
    }
    this._updateDropdownContent();
  }

  // draws the icon, badge and spinner into the anchor
  _renderIcon() {
    this.container = document.getElementById(ANCHOR_ID);
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

  // draws the empty drop-down
  _renderDropdown() {
    this.dropdown = makeElement('div', 'dd-content dd-menu weather-dropdown',
      '<a>Loading...</a>');
    this.dropdown.id = 'weatherMenu';
    this.dropdown.style.display = 'none';
    document.body.appendChild(this.dropdown);
  }

  // one drop-down row: an icon and its text
  _menuRow(icon, text) {
    return makeElement('a', '',
      `<i class="${icon}" style="width: 25px; text-align: center;"></i> ${text}`);
  }

  // fills the drop-down from the reading held
  _updateDropdownContent() {
    const reading = this.weatherData;
    this.dropdown.innerHTML = '';
    this.dropdown.appendChild(
      this._menuRow('fad fa-map-marker-alt', reading.location || 'Unknown'));
    this.dropdown.appendChild(this._menuRow('fad fa-thermometer-half',
      `${reading.temperature}\u00B0   ${reading.condition}`));
    this.dropdown.appendChild(
      this._menuRow('fad fa-humidity', `${reading.humidity}% Humidity`));
  }

  // fills the drop-down with the error
  _updateDropdownContentError() {
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

  // the icon pressed: toggles the drop-down, reading the weather as it opens
  toggleMenu(event) {
    event.stopPropagation();
    if (this.isOpen) {
      this.closeMenu();
      return;
    }
    removeDropdowns();
    this.fetchWeather();
    this._placeDropdown();
    this.isOpen = true;
  }

  // a click elsewhere closes the drop-down
  _handleOutsideClick(event) {
    if (!this.isOpen) return;
    if (this.container.contains(event.target)) return;
    if (this.dropdown.contains(event.target)) return;
    this.closeMenu();
  }
}
