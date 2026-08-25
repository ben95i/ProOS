export default class WeatherWidget {
  constructor(apiUrl = '/api?var=&arg=weather&action=main') {
    this.apiUrl = apiUrl; // Ensure your API endpoint returns the specified JSON
    // 60,000ms. The old expression (10 * 60 * 100) read like ten minutes and
    // was in fact one, which is the interval that has been running; the value
    // is unchanged, only written so it says what it does.
    this.refreshInterval = 60 * 1000; // 1 minute in milliseconds
    this.timerId = null;
    this.weatherData = null;
    this.container = null;
    this.dropdown = null;
    this.isOpen = false;
    // set by remove(); a reply that lands afterwards is dropped rather than
    // written into elements the page has already taken away
    this.removed = false;
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
    this.timerId = setInterval(() => {
      console.log(`[WeatherWidget] Auto-refresh triggered at ${new Date().toLocaleTimeString()}`);
      this.fetchWeather();
    }, this.refreshInterval);
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

  async fetchWeather() {
    if (this.removed) return;
    if (this.iconElement && this.spinnerElement) {
      this.iconElement.style.display = 'none';
      this.spinnerElement.style.display = 'inline-block';
    }
    try {
      const response = await fetch(this.apiUrl);
      if (!response.ok) throw new Error('API Network Error');
      const data = await response.json(); 
      if (this.removed) return; // torn down while the request was in flight
      this.weatherData = data;
      this._updateWeatherUI();
    } catch (error) {
      if (this.removed) return;
      console.error('Failed to fetch weather data:', error);
      if (this.iconElement && this.spinnerElement) {
        this.spinnerElement.style.display = 'none';
        this.iconElement.style.display = '';
        this.iconElement.className = 'fad fa-exclamation-triangle';
      }
      if (this.badgeElement) {
        this.badgeElement.style.display = 'none';
      }
      this._updateDropdownContentError();
    }
  }

  _updateWeatherUI() {
    if (!this.weatherData) return;
    if (this.iconElement && this.spinnerElement) {
      this.spinnerElement.style.display = 'none';
      this.iconElement.style.display = ''; 
    }
    let iconClass = 'fad fa-clouds'; 
    const cond = (this.weatherData.condition || '').toLowerCase();
    const time = this.weatherData.timeOfDay || 'day';
    if (cond.includes('sun') || cond.includes('clear')) {
      if (time === 'sunset') 
        iconClass = 'fad fa-sunset';
      else if (time === 'night') 
        iconClass = 'fad fa-moon-stars'; 
      else iconClass = 'fad fa-sun';
    } 
    else if (cond.includes('cloud')) {
      iconClass = (time === 'night') ? 'fad fa-cloud-moon' : 'fad fa-clouds';
    }
    else if (cond.includes('rain') || cond.includes('drizzle')) 
      iconClass = 'fad fa-cloud-rain';
    else if (cond.includes('snow')) 
      iconClass = 'fad fa-cloud-snow';
    else if (cond.includes('storm') || cond.includes('thunder')) 
      iconClass = 'fad fa-thunderstorm';
    this.iconElement.className = iconClass;
    if (this.weatherData.temperature !== undefined) {
      this.badgeElement.innerText = `${this.weatherData.temperature}°`;
      this.badgeElement.style.display = 'block';
    }
    this._updateDropdownContent();
  }

  _renderIcon() {
    this.container = document.getElementById('weather_anchor');
    if (!this.container) {
      console.error("weather_anchor not found in the DOM.");
      return;
    }
    // Create inner elements
    this.iconElement = document.createElement('i');
    this.iconElement.className = 'fad fa-clouds'; 
    this.badgeElement = document.createElement('span');
    this.badgeElement.className = 'weather-badge';
    this.badgeElement.style.display = 'none';
    // Create the new CSS spinner element
    this.spinnerElement = document.createElement('span');
    this.spinnerElement.className = 'right-nav-spinner';
    this.spinnerElement.style.display = 'none'; // Hidden by default
    // Clear existing content and append new elements
    this.container.innerHTML = '';
    this.container.appendChild(this.iconElement);
    this.container.appendChild(this.spinnerElement);
    this.container.appendChild(this.badgeElement);
    // Add required styles and listeners
    this.container.classList.add('weather-icon-container');
    this.container.addEventListener('click', this.toggleMenu);
  }

 _renderDropdown() {
    this.dropdown = document.createElement('div');
    this.dropdown.className = 'dd-content dd-menu weather-dropdown';
    this.dropdown.id = 'weatherMenu';
    this.dropdown.style.display = 'none';
    this.dropdown.innerHTML = '<a>Loading...</a>';
    document.body.appendChild(this.dropdown);
  }

  _updateDropdownContent() {
    if (!this.dropdown || !this.weatherData) return;
    this.dropdown.innerHTML = '';
    const createItem = (icon, text) => {
      const a = document.createElement('a');
      a.innerHTML = `<i class="${icon}" style="width: 25px; text-align: center;"></i> ${text}`;
      return a;
    };
    this.dropdown.appendChild(createItem('fad fa-map-marker-alt', this.weatherData.location || 'Unknown'));
    this.dropdown.appendChild(createItem('fad fa-thermometer-half', `${this.weatherData.temperature}°   ${this.weatherData.condition}`));
    this.dropdown.appendChild(createItem('fad fa-humidity', `${this.weatherData.humidity}% Humidity`));
  }

  _updateDropdownContentError() {
    if (!this.dropdown) return;
    this.dropdown.innerHTML = '';
    const errorItem = document.createElement('a');
    errorItem.innerHTML = `<i class="fad fa-exclamation-triangle" style="color: var(--red-ind);"></i> Error Loading Weather`;
    this.dropdown.appendChild(errorItem);
  }

  toggleMenu(event) {
    if (event) event.stopPropagation();
    if (this.isOpen) { 
      this.dropdown.style.display = 'none';
      this.isOpen = false;
    } else {
      if (typeof window.removeDropdowns === 'function') {
        window.removeDropdowns(); 
      }      
      this.fetchWeather();
      const rect = this.container.getBoundingClientRect();
      this.dropdown.style.position = 'fixed'; 
      this.dropdown.style.top = `${rect.bottom + 5}px`;
      this.dropdown.style.left = `${rect.left + (rect.width / 2)}px`;
      this.dropdown.style.transform = 'translateX(-80%)';
      this.dropdown.style.zIndex = '9999';
      this.dropdown.style.display = 'block';
      this.isOpen = true;
    }
  }

  _handleOutsideClick(event) {
    if (this.isOpen && this.container && !this.container.contains(event.target) && !this.dropdown.contains(event.target)) {
      this.dropdown.style.display = 'none';
      this.isOpen = false;
    }
  }
}