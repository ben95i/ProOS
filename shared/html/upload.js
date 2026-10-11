// Automate Website - file uploader
//
// File upload, in 1MB chunks, with a progress window. Every file type is
// accepted except the few in BLOCKED_EXT; the other limit is MAX_FILE_BYTES.
//
//   const uploader = new Uploader();
//   uploader.pick();     // opens the file dialog
//   uploader.destroy();  // page teardown
//
// One file at a time and one chunk at a time, in order: a phone on wifi
// copes badly with several large uploads at once, and a single stream in
// order is what lets the API append each chunk as it lands rather than
// holding a whole video in memory.
//
// The API answers the folder the files land in; it is a constant on that
// side, not something the browser gets to choose.

import { html, showPopup, makeElement, promptWindow, onButtons,
  removeWindow, base64URLSafeEncode, sleep,
  gridButtonTemplate } from './utils.js';

const CHUNK_BYTES = 1024 * 1024; // 1MB per request
const MAX_FILE_BYTES = 1024 * 1024 * 1024; // 1GB, per file
const CHUNK_ATTEMPTS = 3;    // tries per chunk before the file is given up on
const RETRY_PAUSE_MS = 750;  // between those tries
// File types the API refuses. Kept in step with BLOCKED_EXT in api/upload.js
// and checked here as well, so a file is turned away before anything is sent
// rather than after a round trip.
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
const MAX_SHOWN_NAME = 50; // longer filenames are cut with an ellipsis
const VISIBLE_ROWS = 5;    // rows on screen at once; the rest slide up
// element IDs
const WIN_ID = 'upl__win';
const TITLE_ID = 'upl__title';
const LIST_ID = 'upl__list';
const BAR_ID = 'upl__bar';
const PCT_ID = 'upl__pct';
const CLOSE_BTN_ID = 'upl__btn-close';
// The button's wording has a span of its own beside the icon, as it changes
// from Cancel to Close once the uploads are done.
const CLOSE_LABEL_ID = 'upl__btn-close-label';
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

// Is this one of the file types that will be refused?
// isBlockedType('setup.EXE') -> true
function isBlockedType(name) {
  const text = String(name);
  const lastDot = text.lastIndexOf('.');
  if (lastDot < 0) {
    return false; // no extension at all, so nothing to match on
  }
  const ext = text.substring(lastDot).toLowerCase();
  return BLOCKED_EXT.includes(ext);
}

// A name too long for one row: cut, with an ellipsis marking the cut.
// shortName('a very long filename.jpg') -> 'a very long filen...'
function shortName(name) {
  const text = String(name);
  if (text.length <= MAX_SHOWN_NAME) {
    return text;
  }
  return `${text.substring(0, MAX_SHOWN_NAME)}...`;
}

export default class Uploader {

  // options: onDone(summary), called once a run finishes
  constructor(options = {}) {
    this.onDone = function () {};
    if (typeof options.onDone === 'function') {
      this.onDone = options.onDone;
    }
    this.input = null;        // the hidden file input
    this.queue = [];          // [{ file, state, sent }]
    this.running = false;
    this.cancelled = false;
    this.totalBytes = 0;      // across every file in this run
    this.sentBytes = 0;       // chunks the server has taken
    this.controller = null;   // aborts the request in flight
    this.holdPage = null;     // beforeunload guard while a run is going
  }

  //// picking files ////

  // Opens the file dialog. The input is built on demand and thrown away
  // afterwards: a re-used input keeps its last selection, and picking the
  // same files again then fires no change event at all.
  pick() {
    if (this.running === true) {
      showPopup("[upload] an upload is already running.");
      return;
    }
    this.dropInput();
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    // no accept list: everything the picker can see is offered, so nothing
    // is greyed out because the browser did not recognise its type
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

  dropInput() {
    if (this.input) {
      this.input.remove();
      this.input = null;
    }
  }

  //// running an upload ////

  // Takes the chosen files and sends them one after another.
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
    this.drawWindow();
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
        // the bytes of a file that failed part way are not counted, so the
        // bar reads what actually reached the folder
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

  // what the window says once the run is over, and the popup that goes with it
  finishRun(done, failed) {
    const summary = {
      done: done,
      failed: failed,
      cancelled: this.cancelled === true
    };
    let heading = `Uploaded ${done} of ${this.queue.length}`;
    if (summary.cancelled === true) {
      heading = `Cancelled - ${done} of ${this.queue.length} uploaded`;
    }
    this.setTitle(heading);
    this.setCloseLabel('Close');
    if (failed > 0) {
      showPopup(`[upload] ${failed} file(s) failed to upload.`);
    } else if (summary.cancelled !== true && done > 0) {
      showPopup(`[upload] ${done} file(s) uploaded.`);
    }
    this.onDone(summary);
  }

  // Cancel button. The request in flight is dropped and the API is told to
  // throw away what it already holds for that file.
  cancel() {
    if (this.running !== true) {
      this.close();
      return;
    }
    this.cancelled = true;
    if (this.controller) {
      this.controller.abort();
    }
  }

  close() {
    if (this.running === true) {
      this.cancel();
      return;
    }
    removeWindow(WIN_ID);
  }

  destroy() {
    this.cancelled = true;
    if (this.controller) {
      this.controller.abort();
    }
    this.guardPage(false);
    this.dropInput();
    removeWindow(WIN_ID);
    this.running = false;
  }

  // Warns before the page is torn down mid-upload. The chunks already sent
  // are on the server as a part file, and nothing reassembles them if the
  // browser never sends the rest.
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

  // Opens a session, sends every chunk in order, then closes it. False when
  // anything went wrong, and the API is told to drop what it has.
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

  // One chunk, retried a few times: a phone that loses wifi for a moment
  // should not lose the whole video with it.
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

  // One request to the upload API. Answers the parsed JSON, or null when the
  // request failed -- the caller decides whether that is worth a retry.
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
      return null; // aborted, or the server could not be reached
    } finally {
      this.controller = null;
    }
  }

  //// the progress window ////

  drawWindow() {
    removeWindow(WIN_ID);
    const win = promptWindow(WIN_ID, html`
      <div class="upl__title" id="${TITLE_ID}">Uploading</div>
      <div class="upl__list scroll-bar-style" id="${LIST_ID}"></div>
      <div class="progress_grid">
        <div class="bar_progress" id="${BAR_ID}"></div>
      </div>
      <div class="upl__pct" id="${PCT_ID}">0%</div>
      ${gridButtonTemplate({
        buttonId: CLOSE_BTN_ID,
        icon: 'fad fa-times-octagon right-padding',
        label: `<span id="${CLOSE_LABEL_ID}">Cancel</span>`,
        buttonClass: 'button__med'
      })}
    `);
    // the window sits over the page, and a click inside it must not close
    // the menus underneath
    win.classList.add('nohide');
    onButtons(win, (button) => {
      if (button.id === CLOSE_BTN_ID) {
        this.close();
      }
    });
    this.drawRows();
    this.drawProgress();
  }

  setTitle(text) {
    const title = document.getElementById(TITLE_ID);
    if (title) {
      title.innerText = text;
    }
  }

  setCloseLabel(text) {
    const button = document.getElementById(CLOSE_BTN_ID);
    if (!button) {
      return;
    }
    const label = document.getElementById(CLOSE_LABEL_ID);
    if (label) {
      label.innerText = text;
    }
  }

  // One row per file, built once: an icon for where it has got to, and its
  // name. Rows are changed in place afterwards, so the panel holds the place
  // it is scrolled to instead of jumping back to the top on every file.
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
      // the whole name is still there to hover over when it has been cut
      label.title = entry.file.name;
      row.appendChild(label);
      list.appendChild(row);
      entry.icon = icon;
    });
  }

  // swaps one row's icon for the state it has just reached
  updateRow(entry) {
    if (entry.icon) {
      entry.icon.className = ROW_ICONS[entry.state];
    }
  }

  // Keeps the file being sent on the bottom line of the visible rows, so the
  // queue slides up as it is worked through. The row height is measured
  // rather than assumed, so the stylesheet stays the one place it is set.
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
      target = 0; // the first few files need no scrolling at all
    }
    // scrollTo with a behaviour is not everywhere (older WebViews, and the
    // test harness), so the plain property is the fallback
    if (typeof list.scrollTo === 'function') {
      list.scrollTo({ top: target, behavior: 'smooth' });
      return;
    }
    list.scrollTop = target;
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
