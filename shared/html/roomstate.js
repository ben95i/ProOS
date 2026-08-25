import { sendCmd, refreshInterval } from './utils.js'

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

  // Dynamically constructs the SVG using the proper XML namespace
  buildHTML() {
    if (!this.container) return;
    this.container.innerHTML = ''; // Clear contents
    const xmlns = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(xmlns, "svg");
    svg.setAttribute("viewBox", "0 0 50 50");
    svg.setAttribute("class", "roomstatus-svgs");
    svg.classList.add('nohide');
    const trafficLightIcon = document.createElementNS(xmlns, "circle");
    trafficLightIcon.setAttribute("id", "trafficLightIcon");
    trafficLightIcon.setAttribute("cx", "25");
    trafficLightIcon.setAttribute("cy", "25");
    trafficLightIcon.setAttribute("r", "17");
    trafficLightIcon.setAttribute("stroke", "none");
    // Idle hatch, laid over the status light while the reading it shows is
    // no longer being refreshed. Same circle as the light itself, filled with
    // diagonal lines; the CSS fades it in only when the light goes idle, so
    // the last known colour still reads through it.
    const hatchDefs = document.createElementNS(xmlns, "defs");
    const hatchPattern = document.createElementNS(xmlns, "pattern");
    hatchPattern.setAttribute("id", "statusHatchPattern");
    hatchPattern.setAttribute("patternUnits", "userSpaceOnUse");
    hatchPattern.setAttribute("patternTransform", "rotate(45)");
    hatchPattern.setAttribute("width", "5");
    hatchPattern.setAttribute("height", "5");
    const hatchLine = document.createElementNS(xmlns, "line");
    hatchLine.setAttribute("x1", "0");
    hatchLine.setAttribute("y1", "0");
    hatchLine.setAttribute("x2", "0");
    hatchLine.setAttribute("y2", "5");
    hatchLine.setAttribute("stroke", "#fff");
    hatchLine.setAttribute("stroke-width", "1.6");
    hatchPattern.appendChild(hatchLine);
    hatchDefs.appendChild(hatchPattern);
    const hatchCircle = document.createElementNS(xmlns, "circle");
    hatchCircle.setAttribute("id", "statusHatch");
    hatchCircle.setAttribute("cx", "25");
    hatchCircle.setAttribute("cy", "25");
    hatchCircle.setAttribute("r", "17");
    hatchCircle.setAttribute("stroke", "none");
    hatchCircle.setAttribute("fill", "url(#statusHatchPattern)");
    const spinnerCircle = document.createElementNS(xmlns, "circle");
    spinnerCircle.setAttribute("id", "spinnerCircle");
    spinnerCircle.setAttribute("class", "spinner-path");
    spinnerCircle.setAttribute("fill", "none");
    spinnerCircle.setAttribute("stroke-linecap", "round");
    spinnerCircle.setAttribute("pathLength", "100");
    spinnerCircle.setAttribute("cx", "25");
    spinnerCircle.setAttribute("cy", "25");
    spinnerCircle.setAttribute("r", "23");
    svg.appendChild(hatchDefs);
    svg.appendChild(trafficLightIcon);
    // sits after the light, which is what the CSS sibling rule keys on
    svg.appendChild(hatchCircle);
    svg.appendChild(spinnerCircle);
    this.container.appendChild(svg);
    // Refresh spinner, riding the same circle as the countdown ring while a
    // status request is in flight. Semi-transparent so the status light still
    // reads through it, and click-through so it never steals the tap.
    const refreshSpinner = document.createElement('span');
    refreshSpinner.id = 'roomStatusRefresh';
    refreshSpinner.className = 'roomstatus-spinner';
    this.container.appendChild(refreshSpinner);
    // Attach self-contained click listener
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

  async showRoomState() {
    if (this.isRunning || this.dead) return;
    this.isRunning = true;
    const gen = ++this.requestGen;
    const lightIcon = document.getElementById("trafficLightIcon");
    this.container.classList.remove('status-loading');
    let cmd = App.currentRoom + 'status';
    this.setRefreshing(true);
    try {
      const resp = await sendCmd('main', cmd, '');
      // Superseded by a newer request (a room switch), or torn down while
      // this one was in flight. Either way the answer is no longer ours:
      // isRunning belongs to whoever replaced us, so it is left alone.
      if (this.dead || gen !== this.requestGen) {
        return;
      }
      this.setRefreshing(false);
      this.devices = [];
      this.roomState = null;
      lightIcon.classList.remove("status-error", "status-idle");
      if (resp.roomState === null) {
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
      const refreshRate = refreshInterval();
      this.container.style.setProperty('--spin-duration', `${refreshRate}s`);    
      void this.container.offsetWidth; // Trigger reflow
      requestAnimationFrame(() => {
        if (this.dead || gen !== this.requestGen) {
          return; // torn down or superseded between frames
        }
        this.container.classList.add('status-loading');
        this.timerID = setTimeout(() => {
          this.isRunning = false;
          this.showRoomState();
        }, refreshRate * 1000);
      });
    } catch (err) {
      if (this.dead || gen !== this.requestGen) {
        return;
      }
      this.setRefreshing(false);
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

  // Room switch: drop the pending countdown and re-fetch straight away. The
  // light is deliberately left alone -- it holds the last room's value until
  // the new status lands, so switching rooms never flashes the idle dot,
  // which is reserved for a real refresh timeout.
  async restartRoomState() {
    this.stopRoomStateTimer();
    await this.showRoomState();
  }

  hideRoomState() {
    this.stopRoomStateTimer();
    const lightIcon = document.getElementById("trafficLightIcon");
    this.container.classList.remove('status-loading');
    this.setRefreshing(false);
    if (lightIcon) {
      // An error keeps its own colour and drops the state it replaced.
      // Otherwise the light holds the last state it read and takes the idle
      // hatch over it, marking that reading as no longer current. With
      // nothing read yet there is no state to hold, and the CSS falls back
      // to the idle dot on its own.
      if (lightIcon.classList.contains("status-error")) {
        lightIcon.classList.remove("status-off", "status-on");
      } else {
        lightIcon.classList.add("status-idle");
      }
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
  // in showRoomState(), so the refresh chain cannot re-arm itself on a
  // manager the page has already replaced.
  destroy() {
    this.dead = true;
    this.stopRoomStateTimer();
    this.requestGen++;
    if (this.container) {
      this.container.removeEventListener('click', this.roomStateClick);
    }
  }
}