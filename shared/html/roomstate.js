// Automate - room status light in the title bar and the device list behind it.
// new RoomStateManager('status_anchor'); showRoomState() reads the room and re-arms every
// REFRESH_INTERVAL seconds, hideRoomState() stops, destroy() ends the instance.

import { sendCmd, REFRESH_INTERVAL, svgElement, menuRow,
  isBlockDisplayed } from './utils.js'
import { drawMenu, removeDropdowns } from './dropdowns.js';

// the dropdown this draws into, and the host column its rows carry
const STATUS_MENU = 'roomstatus';
const LIGHT_ID = 'trafficLightIcon';

export default class RoomStateManager {

  // draws the light into its container
  constructor(containerId) {
    this.container = document.getElementById(containerId);
    this.timerID = null;
    this.isRunning = false;
    // set by destroy(); a late response is dropped instead of re-arming the refresh
    this.dead = false;
    // bumped per request, so a reply for a room switched away from cannot repaint the light
    this.requestGen = 0;
    this.roomState = null;
    this.devices = [];
    this.roomStateClick = this.roomStateClick.bind(this);
    this.buildHTML();
  }

  // Builds the SVG light: the indicator circle, the idle hatch over it, the countdown
  // ring, and the refresh spinner beside them. The hatch has a dark and a light line per
  // cell, so it reads against any indicator colour.
  buildHTML() {
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
    // after the light, which is what the CSS sibling rule keys on
    svg.appendChild(svgElement("circle", {
      id: "statusHatch", cx: "25", cy: "25", r: "17", stroke: "none",
      fill: "url(#statusHatchPattern)"
    }));
    svg.appendChild(svgElement("circle", {
      id: "spinnerCircle", class: "spinner-path", fill: "none",
      "stroke-linecap": "round", pathLength: "100", cx: "25", cy: "25", r: "23"
    }));
    this.container.appendChild(svg);
    // rides the countdown ring's circle while a request is in flight
    const refreshSpinner = document.createElement('span');
    refreshSpinner.id = 'roomStatusRefresh';
    refreshSpinner.className = 'roomstatus-spinner';
    this.container.appendChild(refreshSpinner);
    this.container.addEventListener('click', this.roomStateClick);
  }

  // Shows or hides the refresh ring; the countdown ring goes first, so only one turns.
  setRefreshing(on) {
    if (on === true) {
      this.container.classList.remove('status-loading');
    }
    this.container.classList.toggle('status-refreshing', on === true);
  }

  // is this answer overtaken: torn down, or a newer request (a room switch)
  stale(gen) {
    return this.dead || gen !== this.requestGen;
  }

  // reads the room's status and paints the light
  async showRoomState() {
    if (this.isRunning || this.dead) return;
    // a countdown from the previous cycle may still be armed
    this.stopRoomStateTimer();
    this.isRunning = true;
    const gen = ++this.requestGen;
    const lightIcon = document.getElementById(LIGHT_ID);
    this.container.classList.remove('status-loading');
    this.setRefreshing(true);
    try {
      // the light turns red on a failure, so no popup as well
      const resp = await sendCmd('main', App.currentRoom + 'status', '',
        { quiet: true });
      // isRunning belongs to whoever replaced this request
      if (this.stale(gen)) {
        return;
      }
      this.setRefreshing(false);
      this.devices = [];
      this.roomState = null;
      lightIcon.classList.remove("status-error", "status-idle");
      if (resp.roomState === null) {
        lightIcon.classList.add("status-error");
        // an open menu says so too, rather than keep the last good rows
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
      // an open dropdown is showing the reading this one just replaced
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
    this.container.style.setProperty('--spin-duration', `${REFRESH_INTERVAL}s`);
    void this.container.offsetWidth; // trigger reflow
    requestAnimationFrame(() => {
      if (this.stale(gen)) {
        return;
      }
      this.container.classList.add('status-loading');
      this.timerID = setTimeout(() => {
        this.isRunning = false;
        this.showRoomState();
      }, REFRESH_INTERVAL * 1000);
    });
  }

  // The status dropdown's rows from the reading held: roomstatus|grnind|Desk Lamp. A device
  // with its own indicator ({ name: 'Fan On', indicator: 'ylwind' }) is drawn with it.
  statusMenuData() {
    if (this.roomState === null) {
      return menuRow(STATUS_MENU, 'noind', 'Error checking devices.');
    }
    let menuData = '';
    for (const device of this.devices) {
      let ind = 'redind';
      if (device.indicator) {
        ind = device.indicator;
      } else if (device.status === false) {
        ind = 'blkind';
      } else if (device.status === true) {
        ind = 'grnind';
      }
      menuData += menuRow(STATUS_MENU, ind, device.name);
    }
    return menuData;
  }

  // draws the rows and shows the menu
  drawStatusMenu() {
    drawMenu(this.statusMenuData(), STATUS_MENU);
    document.getElementById(STATUS_MENU).style.display = 'block';
  }

  // Replaces the rows of a menu already open, without drawMenu()'s scroll.
  refreshStatusMenu() {
    if (!isBlockDisplayed(STATUS_MENU)) {
      return;
    }
    drawMenu(this.statusMenuData(), STATUS_MENU, { MenuScroll: false });
  }

  // the light pressed: closes the menu, or reads the room and opens it
  async roomStateClick(e) {
    e.stopPropagation();
    e.preventDefault();
    if (isBlockDisplayed(STATUS_MENU)) {
      document.getElementById(STATUS_MENU).style.display = 'none';
      return;
    }
    removeDropdowns();
    this.stopRoomStateTimer();
    await this.showRoomState();
    this.drawStatusMenu();
  }

  // Room switch: re-reads straight away. The light holds the last room's value until
  // the new status lands, so switching never flashes the idle dot.
  async restartRoomState() {
    this.stopRoomStateTimer();
    await this.showRoomState();
  }

  // stops refreshing, marking the reading as no longer current
  hideRoomState() {
    this.stopRoomStateTimer();
    this.container.classList.remove('status-loading');
    this.setRefreshing(false);
    const lightIcon = document.getElementById(LIGHT_ID);
    // An error keeps its colour and drops the state it replaced; otherwise the light
    // holds the last state with the idle hatch over it.
    if (lightIcon.classList.contains("status-error")) {
      lightIcon.classList.remove("status-off", "status-on");
    } else {
      lightIcon.classList.add("status-idle");
    }
  }

  // drops the pending refresh, including one armed mid-request
  stopRoomStateTimer() {
    clearTimeout(this.timerID);
    this.timerID = null;
    this.isRunning = false;
  }

  // Ends the instance; an answer in flight is dropped by stale().
  destroy() {
    this.dead = true;
    this.stopRoomStateTimer();
    this.requestGen++;
    this.container.removeEventListener('click', this.roomStateClick);
  }
}
