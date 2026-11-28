// Automate - file uploader: 1MB chunks, every type except BLOCKED_EXT, its progress a
// panel of the Server options window, the same size as the main panel: the file rows,
// bar and percentage over Close, Back and Cancel. new Uploader({ showPanel, onClose,
// onBack }) (server.js); .pick() opens the file dialog, or the panel while an upload runs,
// and .destroy() is the teardown. Close and Back leave an upload running. One file and one
// chunk at a time, in order, so the API can append each as it lands.

import { html, makeElement, onButtons, base64URLSafeEncode, sleep,
  gridButtonTemplate } from './utils.js';
import { showPopup, reportException } from './error.js';

const CHUNK_BYTES = 1024 * 1024;
const MAX_FILE_BYTES = 1024 * 1024 * 1024;
const CHUNK_ATTEMPTS = 3;
const RETRY_PAUSE_MS = 750;
// File types the API refuses (BLOCKED_EXT in nodeapi/upload.js), checked here to save a trip.
const BLOCKED_EXT = [
  // executables and libraries
  '.exe', '.dll', '.com', '.scr', '.msi', '.so',
  // scripts a shell or the host runs
  '.bat', '.cmd', '.vbs', '.ps1', '.sh',
  // server-side pages
  '.php', '.phtml', '.php5', '.php7', '.phar',
  // markup and scripts a browser runs
  '.html', '.htm', '.js', '.mjs'
];
const MAX_SHOWN_NAME = 50;
const VISIBLE_ROWS = 5;    // rows on screen at once; the rest slide up
// element IDs
const WIN_ID = 'upl__win';
const TITLE_ID = 'upl__title';
const LIST_ID = 'upl__list';
const BAR_ID = 'upl__bar';
const PCT_ID = 'upl__pct';
const CLOSE_BTN_ID = 'upl__btn-close';
const BACK_BTN_ID = 'upl__btn-back';
const CANCEL_BTN_ID = 'upl__btn-cancel';
// the panel's buttons, the LAN Scanner's size (upload.css)
const PANEL_BUTTON_CLASS = 'upl__btn';
// Cancel once the uploads are done, leaving Close and Back centred
const CANCEL_GONE_CLASS = 'is-gone';
// per-file row states
const ROW_WAITING = 'waiting';
const ROW_SENDING = 'sending';
const ROW_DONE = 'done';
const ROW_FAILED = 'failed';
const ROW_ICONS = {
  waiting: 'fad fa-clock',
  sending: 'fad fa-arrow-up',
  done: 'fad fa-check',
  failed: 'fad fa-exclamation-triangle'
};

// is this a refused file type? isBlockedType('setup.EXE') -> true
function isBlockedType(name) {
  const text = String(name);
  const lastDot = text.lastIndexOf('.');
  if (lastDot < 0) {
    return false;
  }
  const ext = text.substring(lastDot).toLowerCase();
  return BLOCKED_EXT.includes(ext);
}

// a name cut to fit a row: shortName('a very long filename.jpg') -> 'a very long filen...'
function shortName(name) {
  const text = String(name);
  if (text.length <= MAX_SHOWN_NAME) {
    return text;
  }
  return `${text.substring(0, MAX_SHOWN_NAME)}...`;
}

export default class Uploader {

  // panelActions: showPanel(panelTemplate, afterDraw) draws the panel in the options
  // window; onClose and onBack are its Close and Back
  constructor(panelActions) {
    this.panelActions = panelActions;
    this.heading = '';        // the panel's heading, kept while it is not shown
    this.input = null;        // the hidden file input
    this.queue = [];          // [{ file, state, sent }]
    this.running = false;
    this.cancelled = false;
    this.totalBytes = 0;      // across every file in this run
    this.sentBytes = 0;       // chunks the server has taken
    this.controller = null;   // aborts the request in flight
    this.holdPage = null;     // beforeunload guard while a run is going
  }

  // Opens the file dialog on a fresh input, since a re-used one fires no change event
  // for the same files; while an upload runs, its panel instead.
  pick() {
    if (this.running === true) {
      this.showPanel();
      return;
    }
    this.dropInput();
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.style.display = 'none';
    input.addEventListener('change', () => {
      const chosen = [...input.files];
      this.dropInput();
      if (chosen.length > 0) {
        this.start(chosen);
      }
    });
    document.body.appendChild(input);
    this.input = input;
    input.click();
  }

  // removes the file input
  dropInput() {
    if (this.input) {
      this.input.remove();
      this.input = null;
    }
  }

  // Sends the chosen files one after another, after turning away refused ones.
  async start(files) {
    if (this.running === true) {
      return;
    }
    const blocked = [];
    const tooLarge = [];
    const sendable = [];
    files.forEach(file => {
      if (isBlockedType(file.name)) {
        blocked.push(file);
      } else if (file.size > MAX_FILE_BYTES) {
        tooLarge.push(file);
      } else {
        sendable.push(file);
      }
    });
    blocked.forEach(file => {
      showPopup(`[upload] ${file.name} is not an accepted file type.`);
    });
    tooLarge.forEach(file => {
      showPopup(`[upload] ${file.name} is too large to send.`);
    });
    if (sendable.length === 0) {
      return;
    }
    this.running = true;
    this.cancelled = false;
    this.sentBytes = 0;
    this.totalBytes = sendable.reduce((sum, file) => sum + file.size, 0);
    this.queue = sendable.map(file => ({
      file: file,
      state: ROW_WAITING,
      sent: 0
    }));
    this.heading = 'Uploading';
    this.showPanel();
    this.guardPage(true);
    let done = 0;
    let failed = 0;
    for (let index = 0; index < this.queue.length; index++) {
      if (this.cancelled === true) {
        break;
      }
      const entry = this.queue[index];
      entry.state = ROW_SENDING;
      this.updateRow(entry);
      this.scrollToActive(index);
      const sent = await this.sendFile(entry);
      if (sent === true) {
        entry.state = ROW_DONE;
        done += 1;
      } else {
        entry.state = ROW_FAILED;
        failed += 1;
        // a file that failed part way is not counted in the bar
        this.sentBytes -= entry.sent;
        entry.sent = 0;
      }
      this.updateRow(entry);
      this.drawProgress();
    }
    this.guardPage(false);
    this.running = false;
    this.finishRun(done, failed);
  }

  // the window's heading and the popup once the run is over
  finishRun(done, failed) {
    const wasCancelled = this.cancelled === true;
    let heading = `Uploaded ${done} of ${this.queue.length}`;
    if (wasCancelled) {
      heading = `Cancelled - ${done} of ${this.queue.length} uploaded`;
    }
    this.setTitle(heading);
    this.showCancel(false);
    // a run that went well says so in the panel's heading alone
    if (failed > 0) {
      reportException('uploading files',
        `${failed} of ${this.queue.length} file(s) failed to upload.`);
    }
  }

  // Cancel: drops the request in flight, and the API is told to discard what it holds for
  // that file; the files after it are not sent.
  cancel() {
    if (this.running !== true) {
      return;
    }
    this.cancelled = true;
    if (this.controller) {
      this.controller.abort();
    }
  }

  // Ends the instance; its panel goes with the options window's (server.js).
  destroy() {
    this.cancelled = true;
    if (this.controller) {
      this.controller.abort();
    }
    this.guardPage(false);
    this.dropInput();
    this.running = false;
  }

  // Warns before the page goes mid-upload: nothing reassembles the chunks already sent.
  guardPage(on) {
    if (on === true && this.holdPage === null) {
      this.holdPage = (event) => {
        event.preventDefault();
        event.returnValue = '';
      };
      window.addEventListener('beforeunload', this.holdPage);
      return;
    }
    if (on !== true && this.holdPage !== null) {
      window.removeEventListener('beforeunload', this.holdPage);
      this.holdPage = null;
    }
  }

  //// one file ////

  // Opens a session, sends every chunk in order and closes it. False when anything went
  // wrong, and the API is told to drop what it has.
  async sendFile(entry) {
    const file = entry.file;
    const session = await this.call('start', {
      file: base64URLSafeEncode(file.name),
      size: file.size
    });
    if (session === null || !session.id) {
      return false;
    }
    const chunkCount = Math.ceil(file.size / CHUNK_BYTES);
    for (let index = 0; index < chunkCount; index++) {
      if (this.cancelled === true) {
        await this.call('abort', { id: session.id });
        return false;
      }
      const from = index * CHUNK_BYTES;
      const to = Math.min(from + CHUNK_BYTES, file.size);
      const chunk = file.slice(from, to);
      const sent = await this.sendChunk(session.id, index, chunk);
      if (sent !== true) {
        await this.call('abort', { id: session.id });
        return false;
      }
      const chunkBytes = to - from;
      entry.sent += chunkBytes;
      this.sentBytes += chunkBytes;
      this.drawProgress();
    }
    const closed = await this.call('finish', { id: session.id });
    if (closed === null) {
      return false;
    }
    return true;
  }

  // One chunk, retried a few times: wifi lost for a moment should not lose the whole file.
  async sendChunk(id, index, chunk) {
    for (let attempt = 0; attempt < CHUNK_ATTEMPTS; attempt++) {
      if (this.cancelled === true) {
        return false;
      }
      const answer = await this.call('chunk', { id: id, index: index }, chunk);
      if (answer !== null) {
        return true;
      }
      if (attempt + 1 < CHUNK_ATTEMPTS) {
        await sleep(RETRY_PAUSE_MS);
      }
    }
    return false;
  }

  // One request to the upload API: the parsed JSON, or null when it failed.
  async call(action, params, body = null) {
    const query = Object.keys(params)
      .map(name => `${name}=${params[name]}`)
      .join('&');
    const url = `${location.protocol}//${location.hostname}` +
      `/api/upload?action=${action}&${query}`;
    this.controller = new AbortController();
    try {
      const options = {
        method: 'POST',
        signal: this.controller.signal
      };
      if (body !== null) {
        options.headers = { 'Content-Type': 'application/octet-stream' };
        options.body = body;
      }
      const response = await fetch(url, options);
      if (!response.ok) {
        return null;
      }
      return await response.json();
    } catch (error) {
      return null;
    } finally {
      this.controller = null;
    }
  }

  //// the progress panel ////

  // Shows the progress panel in the options window, drawn from the upload's state.
  showPanel() {
    this.panelActions.showPanel(() => this.panelTemplate(), () => this.bindPanel());
  }

  // The panel's markup: the heading, rows, bar and percentage in a box, over a centred
  // row of icon-only buttons the LAN Scanner's size: Close, Back, and Cancel while the
  // upload runs.
  panelTemplate() {
    return html`
    <div class="svropt__panel upl__panel" id="${WIN_ID}">
      <div class="upl__progress">
        <div class="upl__title" id="${TITLE_ID}"></div>
        <div class="upl__list scroll-bar-style" id="${LIST_ID}"></div>
        <div class="progress_grid">
          <div class="bar_progress" id="${BAR_ID}"></div>
        </div>
        <div class="upl__pct" id="${PCT_ID}">0%</div>
      </div>
      <div class="upl__actions">
        ${gridButtonTemplate({
          buttonId: CLOSE_BTN_ID,
          icon: 'fas fa-times',
          buttonClass: PANEL_BUTTON_CLASS
        })}
        ${gridButtonTemplate({
          buttonId: BACK_BTN_ID,
          icon: 'fad fa-arrow-left',
          buttonClass: PANEL_BUTTON_CLASS
        })}
        ${gridButtonTemplate({
          buttonId: CANCEL_BTN_ID,
          icon: 'fad fa-ban',
          buttonClass: PANEL_BUTTON_CLASS
        })}
      </div>
    </div>`;
  }

  // Binds the panel's buttons and fills it in: Close and Back leave the upload running,
  // Cancel stops it and is gone once it has ended.
  bindPanel() {
    const panel = document.getElementById(WIN_ID);
    if (!panel) {
      return;
    }
    onButtons(panel, (button) => {
      if (button.id === CLOSE_BTN_ID) {
        this.panelActions.onClose();
      }
      if (button.id === BACK_BTN_ID) {
        this.panelActions.onBack();
      }
      if (button.id === CANCEL_BTN_ID) {
        this.cancel();
      }
    });
    this.setTitle(this.heading);
    this.showCancel(this.running === true);
    this.drawRows();
    this.drawProgress();
    const sendingIndex = this.queue.findIndex(entry => entry.state === ROW_SENDING);
    if (sendingIndex >= 0) {
      this.scrollToActive(sendingIndex);
    }
  }

  // sets the panel's heading, kept for when it is drawn again
  setTitle(text) {
    this.heading = text;
    const title = document.getElementById(TITLE_ID);
    if (title) {
      title.innerText = text;
    }
  }

  // shows Cancel while the upload runs; once it has ended Close and Back stand alone
  showCancel(shown) {
    const cancelButton = document.getElementById(CANCEL_BTN_ID);
    if (!cancelButton) {
      return;
    }
    if (shown) {
      cancelButton.classList.remove(CANCEL_GONE_CLASS);
    } else {
      cancelButton.classList.add(CANCEL_GONE_CLASS);
    }
  }

  // One row per file, changed in place afterwards so the panel holds its scroll.
  drawRows() {
    const list = document.getElementById(LIST_ID);
    if (!list) {
      return;
    }
    list.innerHTML = '';
    this.queue.forEach(entry => {
      const row = makeElement('div', 'upl__row');
      const icon = makeElement('i', ROW_ICONS[entry.state]);
      row.appendChild(icon);
      const label = makeElement('span', 'upl__name');
      label.innerText = shortName(entry.file.name);
      // the whole name is there to hover over
      label.title = entry.file.name;
      row.appendChild(label);
      list.appendChild(row);
      entry.icon = icon;
    });
  }

  // swaps one row's icon for its new state
  updateRow(entry) {
    if (entry.icon) {
      entry.icon.className = ROW_ICONS[entry.state];
    }
  }

  // Keeps the file being sent on the bottom visible row, so the queue slides up.
  scrollToActive(index) {
    const list = document.getElementById(LIST_ID);
    if (!list) {
      return;
    }
    const row = list.children[index];
    if (!row) {
      return;
    }
    const rowHeight = row.offsetHeight;
    const rowsAbove = index - (VISIBLE_ROWS - 1);
    let target = rowsAbove * rowHeight;
    if (target < 0) {
      target = 0;
    }
    list.scrollTo({ top: target, behavior: 'smooth' });
  }

  // the bar and the percentage beneath it
  drawProgress() {
    const bar = document.getElementById(BAR_ID);
    const pct = document.getElementById(PCT_ID);
    if (!bar || !pct) {
      return;
    }
    let percent = 0;
    if (this.totalBytes > 0) {
      const fraction = this.sentBytes / this.totalBytes;
      percent = Math.round(fraction * 100);
    }
    if (percent > 100) {
      percent = 100;
    }
    bar.style.width = `${percent}%`;
    pct.innerText = `${percent}%  (${this.sizeText(this.sentBytes)} of ` +
      `${this.sizeText(this.totalBytes)})`;
  }

  // 1536 -> "1.5 KB"
  sizeText(bytes) {
    const units = ['B', 'KB', 'MB', 'GB'];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value = value / 1024;
      unit += 1;
    }
    let shown = value.toFixed(1);
    if (unit === 0) {
      shown = String(Math.round(value));
    }
    return `${shown} ${units[unit]}`;
  }
}
