// Automate - LAN Scanner (server page only, from Server > Options > Network): a window
// listing every host on the LAN, filled in live from the API's streamed scan
// (GET /api/netscan, nodeapi/netscan.js), each host showing the moment it is found. Every
// host in menus/trackedhosts.txt is listed, checked for SSH (a blue SSH indicator) before
// a ping (green), and greyed out as Offline when neither answers. Four boxes over the list
// count All, Online, Offline and Tracked, and pick which the list shows. State is on
// App.lanScanner while open.

import { makeElement, promptWindow, onButtons, gridButtonTemplate } from './utils.js';
import { HttpError, reportError, reportException, reportNotConnected } from './error.js';

// The scan, one JSON event per line:
//   {"type":"start","range":"192.168.1.0/24"}
//   {"type":"host","host":{"key":"tracked:ledwall.home","ip":"192.168.1.12",
//     "hostname":"ledwall.home","vendor":"Raspberry Pi Trading","mac":"B8:27:EB:00:00:01",
//     "online":true,"tracked":true,"ssh":true}}   (sent again, same key, when it changes)
//   {"type":"done"}  or  {"type":"error","message":"Network scan failed: ..."}
const LANSCAN_PATH = 'api/netscan';
const LANSCAN_EVENT_START = 'start';
const LANSCAN_EVENT_HOST = 'host';
const LANSCAN_EVENT_DONE = 'done';
const LANSCAN_EVENT_ERROR = 'error';
const LANSCAN_WHAT = 'scanning the network';
const LANSCAN_CUT_SHORT = 'The scan stopped before it finished.';

const LANSCAN_PROMPT_ID = 'lanscan__prompt';
const LANSCAN_TITLE = 'LAN Scanner';
const LANSCAN_TITLE_ID = 'lanscan__title';
const LANSCAN_LIST_ID = 'lanscan__list';
const LANSCAN_MESSAGE_ID = 'lanscan__message';
const LANSCAN_REFRESH_ICON_ID = 'lanscan__refresh-icon';
const LANSCAN_SPIN_CLASS = 'fa-spin';
// the window's two buttons (sized in lanscan.css)
const LANSCAN_BUTTON_CLASS = 'button_left_txtwin';

// the list's one line of text while it has no rows to show
const LANSCAN_SCANNING_TEXT = 'Scanning...';
const LANSCAN_NONE_TEXT = 'No hosts';
const LANSCAN_FAILED_TEXT = 'Scan failed';
const LANSCAN_MESSAGE_ERROR_CLASS = 'lanscan__message--error';

// shown in place of a field the scan did not report, as the old log report did
const LANSCAN_NO_VALUE = '---';
// a host's status: answered SSH (tracked hosts only), up, or tracked and did not answer
const LANSCAN_SSH_TEXT = 'SSH';
const LANSCAN_ONLINE_TEXT = 'Online';
const LANSCAN_OFFLINE_TEXT = 'Offline';
// a row's indicator, as lanscan__ind--<indicator>: blue with SSH in it, green, or a ring
const LANSCAN_INDICATOR_SSH = 'ssh';
const LANSCAN_INDICATOR_ONLINE = 'online';
const LANSCAN_INDICATOR_OFFLINE = 'offline';
// an offline tracked host's row is greyed out
const LANSCAN_OFFLINE_CLASS = 'is-offline';
// a row the picked box leaves out
const LANSCAN_ROW_HIDE_CLASS = 'lanscan__row--hide';
const LANSCAN_TRACKED_ICON = 'fad fa-thumbtack';

// The boxes over the list: what each counts and shows when picked. The first is picked on
// opening.
const LANSCAN_FILTERS = [
  { key: 'all', icon: 'fad fa-network-wired', label: 'All', shows: () => true },
  { key: 'online', icon: 'fad fa-signal', label: 'Online', shows: row => row.online },
  { key: 'offline', icon: 'fad fa-power-off', label: 'Offline', shows: row => !row.online },
  { key: 'tracked', icon: 'fad fa-thumbtack', label: 'Tracked', shows: row => row.tracked }
];
const LANSCAN_FILTER_PICKED_CLASS = 'is-picked';

// A device's icon, from its maker's name as nmap gives it; the first match wins.
const LANSCAN_VENDOR_ICONS = [
  { match: 'Raspberry Pi', icon: 'fab fa-raspberry-pi' },
  { match: 'Proxmox', icon: 'fad fa-server' },
  { match: 'Apple', icon: 'fab fa-apple' },
  { match: 'Ubiquiti', icon: 'fad fa-wifi' },
  { match: 'Espressif', icon: 'fad fa-microchip' },
  { match: 'Intel', icon: 'fad fa-desktop' },
  { match: 'ASUSTek', icon: 'fad fa-desktop' },
  { match: 'Lenovo', icon: 'fad fa-desktop' },
  { match: 'Dell', icon: 'fad fa-desktop' },
  { match: 'Hewlett Packard', icon: 'fad fa-desktop' }
];
// a maker not in the table, or none reported
const LANSCAN_DEFAULT_ICON = 'fad fa-network-wired';

//// window ////

// Opens the window and starts a scan.
export function openLanScanner() {
  if (App.lanScanner) {
    return;
  }
  const statButtons = LANSCAN_FILTERS.map(filter => gridButtonTemplate({
    buttonId: `lanscan__filter-${filter.key}`,
    icon: filter.icon,
    label: `<span class="lanscan__count" id="lanscan__count-${filter.key}">0</span>` +
      `<span class="lanscan__caption">${filter.label}</span>`,
    buttonClass: 'lanscan__stat'
  })).join('');
  const win = promptWindow(LANSCAN_PROMPT_ID, `
    <fieldset class="btn-group lanscan__panel">
      <div class="btn-group-label"><i class="fad fa-chart-network"></i><span id="${LANSCAN_TITLE_ID}">${LANSCAN_TITLE}</span></div>
      <div class="lanscan__stats">${statButtons}</div>
    </fieldset>
    <div class="lanscan__list scroll-bar-style" id="${LANSCAN_LIST_ID}">
      <div class="lanscan__message" id="${LANSCAN_MESSAGE_ID}"></div>
    </div>
    <div class="popup-actions lanscan__actions">
      ${gridButtonTemplate({
        buttonId: 'lanscan__btn-close',
        icon: 'fas fa-times',
        buttonClass: LANSCAN_BUTTON_CLASS
      })}
      ${gridButtonTemplate({
        buttonId: 'lanscan__btn-refresh',
        icon: 'fad fa-sync-alt',
        iconId: LANSCAN_REFRESH_ICON_ID,
        buttonClass: LANSCAN_BUTTON_CLASS
      })}
    </div>`);
  const lanScanner = {
    win: win,
    title: document.getElementById(LANSCAN_TITLE_ID),
    list: document.getElementById(LANSCAN_LIST_ID),
    message: document.getElementById(LANSCAN_MESSAGE_ID),
    refreshIcon: document.getElementById(LANSCAN_REFRESH_ICON_ID),
    // key -> { row, element }, every host drawn
    hosts: new Map(),
    filter: LANSCAN_FILTERS[0],
    // the scan being read: its fetch, and whether it ended in an error
    abortController: null,
    scanning: false,
    failed: false,
    // bumped per scan, so an older scan's late lines are dropped
    scanSeq: 0
  };
  App.lanScanner = lanScanner;
  onButtons(win, (button) => {
    if (button.id === 'lanscan__btn-close') {
      closeLanScanner();
      return;
    }
    if (button.id === 'lanscan__btn-refresh') {
      startLanScan(lanScanner);
      return;
    }
    const filter = LANSCAN_FILTERS.find(entry => button.id === `lanscan__filter-${entry.key}`);
    if (filter) {
      pickFilter(lanScanner, filter);
    }
  });
  pickFilter(lanScanner, lanScanner.filter);
  startLanScan(lanScanner);
}

// Takes the window down and stops reading the scan (which the server finishes anyway).
export function closeLanScanner() {
  const lanScanner = App.lanScanner;
  if (!lanScanner) {
    return;
  }
  App.lanScanner = null;
  lanScanner.scanSeq += 1;
  if (lanScanner.abortController) {
    lanScanner.abortController.abort();
    lanScanner.abortController = null;
  }
  lanScanner.win.remove();
}

//// the scan ////

// the scan's address: http://automate.home/api/netscan
function lanScanURL() {
  return `${location.protocol}//${location.hostname}/${LANSCAN_PATH}`;
}

// Clears the list and reads a new scan, each host drawn as its line arrives; the Refresh
// icon turns until the scan ends. A scan already running on the server is joined.
async function startLanScan(lanScanner) {
  lanScanner.scanSeq += 1;
  const sequence = lanScanner.scanSeq;
  if (lanScanner.abortController) {
    lanScanner.abortController.abort();
  }
  const abortController = new AbortController();
  lanScanner.abortController = abortController;
  clearRows(lanScanner);
  lanScanner.failed = false;
  setScanning(lanScanner, true);
  let finished = false;
  try {
    const response = await fetch(lanScanURL(), {
      cache: 'no-store',
      signal: abortController.signal
    });
    if (!response.ok || !response.body) {
      const answerText = await response.text();
      throw new HttpError(response.status, answerText);
    }
    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let pending = '';
    while (!finished) {
      const { value, done } = await reader.read();
      if (sequence !== lanScanner.scanSeq) {
        return;
      }
      if (done) {
        break;
      }
      pending += decoder.decode(value, { stream: true });
      const lines = pending.split('\n');
      // the last piece is a line still arriving
      pending = lines.pop();
      for (const line of lines) {
        if (line.trim() !== '' && readScanLine(lanScanner, line)) {
          finished = true;
        }
      }
    }
    if (!finished) {
      reportNotConnected(LANSCAN_WHAT, LANSCAN_CUT_SHORT);
      lanScanner.failed = true;
    }
  } catch (error) {
    // closed, refreshed, or replaced by a newer scan
    if (sequence !== lanScanner.scanSeq || error.name === 'AbortError') {
      return;
    }
    reportError(LANSCAN_WHAT, error);
    lanScanner.failed = true;
  }
  if (sequence !== lanScanner.scanSeq) {
    return;
  }
  lanScanner.abortController = null;
  setScanning(lanScanner, false);
}

// Acts on one line of the scan; true once it is the last ('done' or 'error').
// A line that is not JSON throws, ending the read as an exception.
function readScanLine(lanScanner, line) {
  const scanEvent = JSON.parse(line);
  switch (scanEvent.type) {
    case LANSCAN_EVENT_START:
      lanScanner.title.textContent = `${LANSCAN_TITLE} · ${fieldText(scanEvent.range)}`;
      return false;
    case LANSCAN_EVENT_HOST:
      showHost(lanScanner, scanEvent.host);
      return false;
    case LANSCAN_EVENT_DONE:
      return true;
    case LANSCAN_EVENT_ERROR:
      // "Network scan failed: ..."
      reportException(LANSCAN_WHAT, scanEvent.message);
      lanScanner.failed = true;
      return true;
    default:
      return false;
  }
}

// the Refresh icon turning, and the list's line, while a scan is read
function setScanning(lanScanner, scanning) {
  lanScanner.scanning = scanning;
  lanScanner.refreshIcon.classList.toggle(LANSCAN_SPIN_CLASS, scanning);
  showMessage(lanScanner);
}

//// rows ////

// A field as drawn: the scan's text, or the placeholder when it has none.
function fieldText(value) {
  if (typeof value !== 'string' || value === '') {
    return LANSCAN_NO_VALUE;
  }
  return value;
}

// a device's icon from its maker: 'Raspberry Pi Trading' -> 'fab fa-raspberry-pi'
function vendorIcon(vendor) {
  if (typeof vendor !== 'string') {
    return LANSCAN_DEFAULT_ICON;
  }
  const found = LANSCAN_VENDOR_ICONS.find(entry => vendor.includes(entry.match));
  if (found) {
    return found.icon;
  }
  return LANSCAN_DEFAULT_ICON;
}

// One host as a row:
// { key: "tracked:ledgrid.home", ip: "192.168.1.23", hostname: "ledgrid.home", vendor: null,
//   mac: null, online: false, tracked: true, ssh: false } ->
// { key: "tracked:ledgrid.home", address: "192.168.1.23", name: "ledgrid.home",
//   vendor: "---", mac: "---", icon: "fad fa-network-wired", online: false, tracked: true,
//   ssh: false, indicator: "offline", statusText: "Offline" }
export function lanScanRow(host) {
  const online = host.online === true;
  const ssh = host.ssh === true;
  let indicator = LANSCAN_INDICATOR_OFFLINE;
  let statusText = LANSCAN_OFFLINE_TEXT;
  if (ssh) {
    indicator = LANSCAN_INDICATOR_SSH;
    statusText = LANSCAN_SSH_TEXT;
  } else if (online) {
    indicator = LANSCAN_INDICATOR_ONLINE;
    statusText = LANSCAN_ONLINE_TEXT;
  }
  return {
    key: String(host.key),
    address: fieldText(host.ip),
    name: fieldText(host.hostname),
    vendor: fieldText(host.vendor),
    mac: fieldText(host.mac),
    icon: vendorIcon(host.vendor),
    online: online,
    tracked: host.tracked === true,
    ssh: ssh,
    indicator: indicator,
    statusText: statusText
  };
}

// An IPv4 address as four numbers, or null for anything else: '192.168.1.12' -> [192, 168, 1, 12]
function addressOctets(address) {
  const octets = address.split('.').map(Number);
  if (octets.length !== 4 || octets.some(octet => isNaN(octet))) {
    return null;
  }
  return octets;
}

// Row order: by address, octet by octet; a row without an address goes to the bottom, those
// in order of name.
export function compareRows(a, b) {
  const aOctets = addressOctets(a.address);
  const bOctets = addressOctets(b.address);
  if (aOctets === null && bOctets === null) {
    return a.name.localeCompare(b.name);
  }
  if (aOctets === null) return 1;
  if (bOctets === null) return -1;
  for (let i = 0; i < 4; i++) {
    if (aOctets[i] !== bOctets[i]) {
      return aOctets[i] - bOctets[i];
    }
  }
  return a.name.localeCompare(b.name);
}

// Draws a host, or redraws it under its key, in its place by address.
function showHost(lanScanner, host) {
  const row = lanScanRow(host);
  let drawn = lanScanner.hosts.get(row.key);
  if (!drawn) {
    drawn = { row: row, element: makeElement('div', 'lanscan__row') };
    drawn.element.dataset.key = row.key;
    lanScanner.hosts.set(row.key, drawn);
  }
  drawn.row = row;
  fillRowElement(drawn.element, row);
  placeRow(lanScanner, drawn);
  drawn.element.classList.toggle(LANSCAN_ROW_HIDE_CLASS, !lanScanner.filter.shows(row));
  showCounts(lanScanner);
  showMessage(lanScanner);
}

// A row's parts, built node by node, as names come from the network:
//   indicator | address over MAC | device icon | name over maker (or Offline)
function fillRowElement(element, row) {
  element.classList.toggle(LANSCAN_OFFLINE_CLASS, !row.online);
  const indicatorDot = makeElement('span', `lanscan__ind lanscan__ind--${row.indicator}`);
  if (row.ssh) {
    // its own element, so it can be trimmed to its capitals and centred in the dot
    const indicatorText = makeElement('span', 'lanscan__ind-text');
    indicatorText.textContent = row.statusText;
    indicatorDot.appendChild(indicatorText);
  }
  const address = makeElement('span', 'lanscan__ip');
  address.textContent = row.address;
  const mac = makeElement('span', 'lanscan__mac');
  mac.textContent = row.mac;
  const icon = makeElement('i', `${row.icon} lanscan__icon`);
  const name = makeElement('span', 'lanscan__host');
  if (row.tracked) {
    name.appendChild(makeElement('i', LANSCAN_TRACKED_ICON));
  }
  name.appendChild(document.createTextNode(row.name));
  const vendor = makeElement('span', 'lanscan__vendor');
  if (row.online) {
    vendor.textContent = row.vendor;
  } else {
    vendor.textContent = row.statusText;
  }
  element.replaceChildren(indicatorDot, address, mac, icon, name, vendor);
}

// Moves a row to its place, as the list is kept in order: before the first other row that
// sorts after it.
function placeRow(lanScanner, drawn) {
  let nextElement = null;
  for (const child of lanScanner.list.children) {
    // the message line has no key
    const other = lanScanner.hosts.get(child.dataset.key);
    if (!other || other === drawn) {
      continue;
    }
    if (compareRows(other.row, drawn.row) > 0) {
      nextElement = child;
      break;
    }
  }
  if (nextElement === null) {
    lanScanner.list.appendChild(drawn.element);
  } else if (drawn.element.nextSibling !== nextElement) {
    lanScanner.list.insertBefore(drawn.element, nextElement);
  }
}

// takes every row off the list
function clearRows(lanScanner) {
  lanScanner.hosts.forEach(drawn => drawn.element.remove());
  lanScanner.hosts.clear();
  showCounts(lanScanner);
}

//// boxes and filter ////

// how many rows each box counts: -> { all: 21, online: 19, offline: 2, tracked: 10 }
export function lanScanCounts(rows) {
  const counts = {};
  LANSCAN_FILTERS.forEach(filter => {
    const shownRows = rows.filter(row => filter.shows(row));
    counts[filter.key] = shownRows.length;
  });
  return counts;
}

// writes each box's count
function showCounts(lanScanner) {
  const rows = [...lanScanner.hosts.values()].map(drawn => drawn.row);
  const counts = lanScanCounts(rows);
  LANSCAN_FILTERS.forEach(filter => {
    const countElement = document.getElementById(`lanscan__count-${filter.key}`);
    countElement.textContent = String(counts[filter.key]);
  });
}

// Picks a box: it is marked, and the list shows only its rows.
function pickFilter(lanScanner, filter) {
  lanScanner.filter = filter;
  LANSCAN_FILTERS.forEach(entry => {
    const box = document.getElementById(`lanscan__filter-${entry.key}`);
    box.classList.toggle(LANSCAN_FILTER_PICKED_CLASS, entry === filter);
  });
  lanScanner.hosts.forEach(drawn => {
    drawn.element.classList.toggle(LANSCAN_ROW_HIDE_CLASS, !filter.shows(drawn.row));
  });
  showMessage(lanScanner);
}

// The list's line while no row is shown: Scanning..., Scan failed, or No hosts.
function showMessage(lanScanner) {
  let shownRows = 0;
  lanScanner.hosts.forEach(drawn => {
    if (lanScanner.filter.shows(drawn.row)) {
      shownRows += 1;
    }
  });
  let text = '';
  if (shownRows === 0) {
    text = LANSCAN_NONE_TEXT;
    if (lanScanner.scanning) {
      text = LANSCAN_SCANNING_TEXT;
    } else if (lanScanner.failed) {
      text = LANSCAN_FAILED_TEXT;
    }
  }
  const isError = text === LANSCAN_FAILED_TEXT;
  lanScanner.message.textContent = text;
  lanScanner.message.classList.toggle(LANSCAN_MESSAGE_ERROR_CLASS, isError);
  let display = '';
  if (text === '') {
    display = 'none';
  }
  lanScanner.message.style.display = display;
}
