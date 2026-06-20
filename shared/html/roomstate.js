import { sendCmd } from './utils.js'

export default class RoomStateManager {
  constructor(containerId) {
    this.containerId = containerId;
    this.container = document.getElementById(containerId);
    this.timerID = null;
    this.isRunning = false;
    this.roomState = null;
    this.devices = [];
    this.roomStateClick = this.roomStateClick.bind(this);
    this.showRoomState = this.showRoomState.bind(this);
    this.hideRoomState = this.hideRoomState.bind(this);
    this.stopRoomStateTimer = this.stopRoomStateTimer.bind(this);
    this.buildHTML();
  }

  // Dynamically constructs the SVG using the proper XML namespace
  buildHTML() {
    if (!this.container) return;
    this.container.innerHTML = ''; // Clear contents
    const xmlns = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(xmlns, "svg");
    svg.setAttribute("viewBox", "0 0 50 50");
    svg.setAttribute("class", "roomstatus-svgs");
    const trafficLightIcon = document.createElementNS(xmlns, "circle");
    trafficLightIcon.setAttribute("id", "trafficLightIcon");
    trafficLightIcon.setAttribute("cx", "25");
    trafficLightIcon.setAttribute("cy", "25");
    trafficLightIcon.setAttribute("r", "17");
    trafficLightIcon.setAttribute("stroke", "none");
    const spinnerCircle = document.createElementNS(xmlns, "circle");
    spinnerCircle.setAttribute("id", "spinnerCircle");
    spinnerCircle.setAttribute("class", "spinner-path");
    spinnerCircle.setAttribute("fill", "none");
    spinnerCircle.setAttribute("stroke-linecap", "round");
    spinnerCircle.setAttribute("pathLength", "100");
    spinnerCircle.setAttribute("cx", "25");
    spinnerCircle.setAttribute("cy", "25");
    spinnerCircle.setAttribute("r", "23");
    svg.appendChild(trafficLightIcon);
    svg.appendChild(spinnerCircle);
    this.container.appendChild(svg);
    // Attach self-contained click listener
    this.container.addEventListener('click', this.roomStateClick);
  }

  async showRoomState() {
    if (this.isRunning) return;
    this.isRunning = true;
    const lightIcon = document.getElementById("trafficLightIcon");
    this.container.classList.remove('status-loading');
    let room = null; 
    let cmd = null;
    // Check global state for the command context
    if (App.ctlCommand === 'lr' || App.ctlCommand === 'subs') {
      room = "living room"; 
      cmd = 'lrstatus';
    } else if (App.ctlCommand === 'br') {
      room = "bedroom"; 
      cmd = 'brstatus';
    }
    if (cmd === null) {
      this.hideRoomState();
      return;
    }
    lightIcon.classList.add("status-pending");
    try {
      const resp = await sendCmd('main', cmd, '');
      this.devices = [];
      this.roomState = null;
      lightIcon.classList.remove("status-error", "status-pending");
      if (resp === null) {
        lightIcon.classList.add("status-error");
        this.hideRoomState();
        return;
      } else {
        this.roomState = resp.roomState;
        this.devices = resp.devices;
        if (this.roomState === true) {
          lightIcon.classList.remove("status-off");
          lightIcon.classList.add("status-on");
        } else if (this.roomState === false) { 
          lightIcon.classList.remove("status-on");
          lightIcon.classList.add("status-off");
        }
      }
      this.container.style.setProperty('--spin-duration', `${App.REFRESH_INTERVAL}s`);    
      void this.container.offsetWidth; // Trigger reflow
      requestAnimationFrame(() => {
        this.container.classList.add('status-loading');
        this.timerID = setTimeout(() => {
          this.isRunning = false;
          this.showRoomState();
        }, App.REFRESH_INTERVAL * 1000);
      });
    } catch (err) {
      lightIcon.classList.add("status-error");
      this.hideRoomState();
    }
  }

  async roomStateClick(e) {
    if (e) {
      e.stopPropagation();
      e.preventDefault(); 
    }
    const menu = 'roomstatus';
    let _elem = document.getElementById(menu);
    if (_elem && _elem.style.display === 'block') {
      _elem.style.display = 'none';
      return;
    }
    // Reference the globally exposed window function
    if (typeof window.removeDropdowns === 'function') window.removeDropdowns();
    this.stopRoomStateTimer();
    await this.showRoomState();
    let menuData = '';
    if (this.roomState === null) {
      menuData += `${menu}|noind|Error checking devices.\n`;
    } else {
      for (const device of this.devices) {
        const name = device.name;
        const status = device.status;
        let ind = status === false ? 'blkind' : (status === true ? 'grnind' : 'redind');
        menuData += `${menu}|${ind}|${name}\n`;
      }
    }
    // Reference the globally exposed window function
    if (typeof window.drawMenu === 'function') window.drawMenu(menuData, menu);
    // Re-fetch in case drawMenu rewrote the DOM
    _elem = document.getElementById(menu);
    if (_elem) _elem.style.display = 'block';
  }

  hideRoomState() {
    this.stopRoomStateTimer();
    const lightIcon = document.getElementById("trafficLightIcon");
    this.container.classList.remove('status-loading');
    if (lightIcon) {
      lightIcon.classList.remove("status-pending", "status-off", "status-on");
    }
  }

  stopRoomStateTimer() {
    if (!this.isRunning) return;
    clearTimeout(this.timerID);
    this.isRunning = false;
  }
}