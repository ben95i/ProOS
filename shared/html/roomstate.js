// Automate Website - room status light
//
// The traffic light in the title bar and the device list behind it.
// new RoomStateManager('status_anchor') draws it; showRoomState() reads the
// room and re-arms itself every refreshInterval() seconds, hideRoomState()
// stops, destroy() ends the instance.

import { sendCmd, refreshInterval, svgElement, menuRow } from './utils.js'

// the dropdown this manager draws into, and the host column its rows carry
const STATUS_MENU = 'roomstatus';
const LIGHT_ID = 'trafficLightIcon';

export default class RoomStateManager {

  constructor(containerId) {
    this.containerId = containerId;
    this.container = document.getElementById(containerId);
    this.timerID = null;
    this.isRunning = false;
    // set by destroy(); a response that lands afterwards is dropped instead
    // of re-arming the refresh on a manager the page has already let go of
    this.dead = false;
    // bumped per request, so a reply for a room that has since been switched
    // away from cannot repaint the light
    this.requestGen = 0;
    this.roomState = null;
    this.devices = [];
    this.roomStateClick = this.roomStateClick.bind(this);
    this.showRoomState = this.showRoomState.bind(this);
    this.hideRoomState = this.hideRoomState.bind(this);
    this.restartRoomState = this.restartRoomState.bind(this);
    this.stopRoomStateTimer = this.stopRoomStateTimer.bind(this);
    this.buildHTML();
  }

  // Builds the SVG light: the indicator circle, the idle hatch laid over it,
  // and the countdown ring, plus the refresh spinner beside them.
  //
  // The hatch is two lines per cell, not one. A single white line disappeared
  // against a light indicator and a single dark one would disappear against
  // black, so the pair puts one of each side by side; the dark line is the
  // wider of the two, since the colours that were hard to read are the light
  // ones.
  buildHTML() {
    if (!this.container) return;
    this.container.innerHTML = '';
    const svg = svgElement("svg", { viewBox: "0 0 50 50", class: "roomstatus-svgs nohide" });
    const hatchPattern = svgElement("pattern", {
      id: "statusHatchPattern", patternUnits: "userSpaceOnUse",
      patternTransform: "rotate(45)", width: "5", height: "5"
    });
    hatchPattern.appendChild(svgElement("line", {
      x1: "1.2", y1: "0", x2: "1.2", y2: "5", stroke: "#000", "stroke-width": "2.0"
    }));
    hatchPattern.appendChild(svgElement("line", {
      x1: "3.3", y1: "0", x2: "3.3", y2: "5", stroke: "#fff", "stroke-width": "1.2"
    }));
    const hatchDefs = svgElement("defs");
    hatchDefs.appendChild(hatchPattern);
    svg.appendChild(hatchDefs);
    svg.appendChild(svgElement("circle", {
      id: LIGHT_ID, cx: "25", cy: "25", r: "17", stroke: "none"
    }));
    // sits after the light, which is what the CSS sibling rule keys on
    svg.appendChild(svgElement("circle", {
      id: "statusHatch", cx: "25", cy: "25", r: "17", stroke: "none",
      fill: "url(#statusHatchPattern)"
    }));
    svg.appendChild(svgElement("circle", {
      id: "spinnerCircle", class: "spinner-path", fill: "none",
      "stroke-linecap": "round", pathLength: "100", cx: "25", cy: "25", r: "23"
    }));
    this.container.appendChild(svg);
    // Refresh spinner, riding the same circle as the countdown ring while a
    // request is in flight. Semi-transparent so the light still reads through
    // it, and click-through so it never steals the tap.
    const refreshSpinner = document.createElement('span');
    refreshSpinner.id = 'roomStatusRefresh';
    refreshSpinner.className = 'roomstatus-spinner';
    this.container.appendChild(refreshSpinner);
    this.container.addEventListener('click', this.roomStateClick);
  }

  // shows / hides the rotating refresh ring; the countdown ring is put away
  // first so only one of the two is ever turning
  setRefreshing(on) {
    if (!this.container) {
      return;
    }
    if (on === true) {
      this.container.classList.remove('status-loading');
    }
    this.container.classList.toggle('status-refreshing', on === true);
  }

  // this answer is no longer the current one: torn down, or overtaken by a
  // newer request (a room switch)
  stale(gen) {
    return this.dead || gen !== this.requestGen;
  }

  async showRoomState() {
    if (this.isRunning || this.dead) return;
    // A countdown from the previous cycle can still be armed. activityBegin()
    // calls this on every click, and the timeout below used to be assigned
    // straight over the old handle without clearing it: the orphan fired
    // later, set isRunning false and started a second refresh chain.
    this.stopRoomStateTimer();
    this.isRunning = true;
    const gen = ++this.requestGen;
    const lightIcon = document.getElementById(LIGHT_ID);
    // the page was re-rendered underneath this manager: there is nothing left
    // to paint, and going on would throw
    if (!lightIcon || !this.container) {
      this.isRunning = false;
      return;
    }
    this.container.classList.remove('status-loading');
    this.setRefreshing(true);
    try {
      const resp = await sendCmd('main', App.currentRoom + 'status', '');
      // isRunning belongs to whoever replaced us, so it is left alone
      if (this.stale(gen)) {
        return;
      }
      this.setRefreshing(false);
      this.devices = [];
      this.roomState = null;
      lightIcon.classList.remove("status-error", "status-idle");
      if (resp.roomState === null) {
        lightIcon.classList.add("status-error");
        // an open menu says so as well, rather than sitting on the rows of
        // the last reading that worked
        this.refreshStatusMenu();
        this.hideRoomState();
        return;
      }
      this.roomState = resp.roomState;
      this.devices = resp.devices;
      if (this.roomState === true) {
        lightIcon.classList.remove("status-off");
        lightIcon.classList.add("status-on");
      } else if (this.roomState === false) {
        lightIcon.classList.remove("status-on");
        lightIcon.classList.add("status-off");
      }
      // the dropdown, if open, is showing the reading this one just replaced
      this.refreshStatusMenu();
      this.armRefresh(gen);
    } catch (err) {
      if (this.stale(gen)) {
        return;
      }
      this.setRefreshing(false);
      lightIcon.classList.add("status-error");
      this.hideRoomState();
    }
  }

  // starts the countdown ring and books the next read
  armRefresh(gen) {
    const refreshRate = refreshInterval();
    this.container.style.setProperty('--spin-duration', `${refreshRate}s`);
    void this.container.offsetWidth; // trigger reflow
    requestAnimationFrame(() => {
      if (this.stale(gen)) {
        return; // torn down or superseded between frames
      }
      this.container.classList.add('status-loading');
      this.timerID = setTimeout(() => {
        this.isRunning = false;
        this.showRoomState();
      }, refreshRate * 1000);
    });
  }

  // The rows of the status dropdown, built from the reading currently held:
  // roomstatus|grnind|Desk Lamp
  statusMenuData() {
    if (this.roomState === null) {
      return menuRow(STATUS_MENU, 'noind', 'Error checking devices.');
    }
    let menuData = '';
    for (const device of this.devices) {
      let ind = 'redind';
      if (device.status === false) {
        ind = 'blkind';
      } else if (device.status === true) {
        ind = 'grnind';
      }
      menuData += menuRow(STATUS_MENU, ind, device.name);
    }
    return menuData;
  }

  // is the dropdown on screen right now
  statusMenuOpen() {
    const _elem = document.getElementById(STATUS_MENU);
    return !!_elem && _elem.style.display === 'block';
  }

  // draws the rows and shows the menu
  drawStatusMenu() {
    if (typeof window.drawMenu === 'function') {
      window.drawMenu(this.statusMenuData(), STATUS_MENU);
    }
    // re-fetch in case drawMenu rewrote the DOM
    const _elem = document.getElementById(STATUS_MENU);
    if (_elem) _elem.style.display = 'block';
  }

  // A refresh landing while the menu is open: only the rows are replaced, and
  // drawMenu() is asked not to scroll. Its scroll step measures a menu that
  // is position:fixed against the page offset, so re-running it on a menu
  // already on screen walks the page down a little on every refresh.
  refreshStatusMenu() {
    if (!this.statusMenuOpen()) {
      return;
    }
    if (typeof window.drawMenu === 'function') {
      window.drawMenu(this.statusMenuData(), STATUS_MENU,
        { TwoColumns: false, MenuScroll: false });
    }
  }

  async roomStateClick(e) {
    if (e) {
      e.stopPropagation();
      e.preventDefault();
    }
    const _elem = document.getElementById(STATUS_MENU);
    if (_elem && _elem.style.display === 'block') {
      _elem.style.display = 'none';
      return;
    }
    if (typeof window.removeDropdowns === 'function') window.removeDropdowns();
    this.stopRoomStateTimer();
    await this.showRoomState();
    this.drawStatusMenu();
  }

  // Room switch: drop the pending countdown and re-fetch straight away. The
  // light is deliberately left alone -- it holds the last room's value until
  // the new status lands, so switching rooms never flashes the idle dot.
  async restartRoomState() {
    this.stopRoomStateTimer();
    await this.showRoomState();
  }

  hideRoomState() {
    this.stopRoomStateTimer();
    if (this.container) {
      this.container.classList.remove('status-loading');
    }
    this.setRefreshing(false);
    const lightIcon = document.getElementById(LIGHT_ID);
    if (!lightIcon) {
      return;
    }
    // An error keeps its own colour and drops the state it replaced.
    // Otherwise the light holds the last state it read and takes the idle
    // hatch over it, marking that reading as no longer current.
    if (lightIcon.classList.contains("status-error")) {
      lightIcon.classList.remove("status-off", "status-on");
    } else {
      lightIcon.classList.add("status-idle");
    }
  }

  // The timeout is cleared unconditionally: isRunning is held for the whole
  // of an in-flight request, so a guard on it used to leave the pending
  // refresh armed whenever the page was torn down mid-request.
  stopRoomStateTimer() {
    clearTimeout(this.timerID);
    this.timerID = null;
    this.isRunning = false;
  }

  // End of life. Anything still in flight is dropped by the generation check
  // in showRoomState(), so the refresh chain cannot re-arm itself.
  destroy() {
    this.dead = true;
    this.stopRoomStateTimer();
    this.requestGen++;
    if (this.container) {
      this.container.removeEventListener('click', this.roomStateClick);
    }
  }
}
