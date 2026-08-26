// Automate Website - Lists Window
//
// Two classes live here:
//   ListStore - the list data, the file on the server and the edit lock
//   Lists     - the window itself (default export, used by main.js)
//
// Centered window holding a set of lists; selecting one swaps the window over
// to the items inside it. Both levels support add, rename, multi-select delete
// and drag re-ordering. Items add a round completed button: un-checked items
// float to the top of the list, checked items sit below in their saved order,
// and only checked items can be dragged.
//
// Delete mode uses that same round button rather than a second set of boxes,
// turned red to say it is deleting and not completing. On the lists view it
// stands in for the drag grip and the right hand arrow steps aside; on the
// items view it takes over the completed button for as long as the mode is on
// (the grip stays in place, hidden, so the rows do not shift sideways). What
// is selected lives in memory, not in the rows, so a re-draw keeps it.
//
// Saved file (lists), one line per item:
//   LIST_ID|LIST_NAME|ITEM_ID|ITEM_NAME|CHECK_STATE
// The order of the rows in the file IS the order of the lists and their items,
// so nothing has to carry a position number. A list with no items keeps its
// line with the item fields left empty.
//
// The first row of the file is the edit lock, not a list:
//   LOCK|STATE|OWNER_TOKEN|TIMESTAMP|0
// Whoever opens the window first claims the lock and edits; anyone opening it
// while the lock is held gets a read only window. The lock is released on
// close and expires on its own if a browser goes away without closing (the
// owner re-stamps the timestamp while the window is open).
//
// Every write is checked: the dot in the top right corner is green while the
// server is answering and red once a write fails. A failed write changes
// nothing on the server, the edits stay in memory, and closing the window
// offers to retry or discard them.
//
// Same file API as the bookmarks menu: GET /api/read?file=lists.save and
// POST /api/write?file=lists.save (text/plain body).

import { splitMenuData, cleanString, isNumeric, removeWindow, showPopup,
  dragGripElement, DragSort, FileLock, readFile, writeFile, html,
  LOCK_BEAT_MS, LOCK_EXPIRE_MS } from './utils.js';

const FILE_NAME = 'lists.save';
const WIN_TITLE = 'Lists';
// element IDs
const WIN_ID = 'list__win';
const LIST_ID = 'list__list';
const TITLE_ID = 'list__title';
const DOT_ID = 'list__dot';
const STATUS_ID = 'list__status';
const OVERLAY_ID = 'list__overlay';
const BACK_BTN_ID = 'list__btn-back';
const ADD_BTN_ID = 'list__btn-add';
const DEL_BTN_ID = 'list__btn-del';
const CLOSE_BTN_ID = 'list__btn-close';
const RETRY_BTN_ID = 'list__btn-retry';
const DISCARD_BTN_ID = 'list__btn-discard';
const DEL_ICON_ID = 'list__del-icon';
const DEL_ICON_ON_CLASS = 'list__del-icon--on';
// file layout
const COL_DELIMITER = '|'; // separates the columns of a row
// the file is never written more often than this; edits made in between are
// folded into the next write, and closing the window writes straight away
const SAVE_INTERVAL_MS = 10000;
// stored names are printable ASCII only, with the column separator "|" and
// the path characters "/" and "\" swapped out
const MAX_NAME_LENGTH = 64;
const COMBINING_MARKS = /[\u0300-\u036f]/g;
const NON_ASCII = /[^\x20-\x7E]/g;
const RESERVED_CHARS = /[\0\r\n|/\\]/g;
const EXTRA_SPACES = /\s{2,}/g;
// window views
const VIEW_LISTS = 0;
const VIEW_ITEMS = 1;
// list modes
const MODE_OPEN = 0;
const MODE_SELECT = 1;
// shown in place of the rows while the server is being waited on, so a slow
// connection never leaves the window sitting empty
const BUSY_LOAD = 'Loading...';
const BUSY_SAVE = 'Saving...';

//// shared helpers ////

// names are trimmed to printable ASCII and kept to one line of sane length
export function cleanName(value) {
  const text = cleanString(value === null || value === undefined ? '' : String(value));
  const name = (text || '')
    .normalize("NFD")             // split accents off their base letter
    .replace(COMBINING_MARKS, '') // "Cafe" keeps the "e", loses the accent
    .replace(NON_ASCII, '')
    .replace(RESERVED_CHARS, '')
    .replace(EXTRA_SPACES, ' ')   // close up any gaps left behind
    .trim();
  return name.length >= MAX_NAME_LENGTH
    ? `${name.substring(0, MAX_NAME_LENGTH)}...`
    : name;
}

function nextId(entries) {
  return entries.reduce((highest, entry) => Math.max(highest, entry.id), -1) + 1;
}

/*
 *  ListStore - list data, file transfer and the edit lock
 */

export class ListStore {

  constructor(onChange) {
    // called when the store changes something the window has to re-draw
    this.onChange = typeof onChange === 'function' ? onChange : function () {};
    this.data = [];         // [{ id, name, items: [{ id, name, checked }] }]
    this.readOnly = false;  // another user holds the lock, or nothing loaded
    this.online = null;     // last server exchange: true / false / not tried
    this.dirty = false;     // edits in memory that the server has not taken
    this.saving = null;     // in-flight save, so writes cannot overlap
    this.queued = false;
    this.sentBody = null;   // the list text the server is known to hold
    this.loaded = false;    // a parsed list is standing in this.data
    this.lastGoodBody = ""; // last body known to be safe to write back
    this.releasing = null;  // release in flight, so two cannot race
    this.keepAlive = false; // the queued write is the lock re-stamp
    this.lastWrite = 0;     // when the file was last written
    this.lastStamp = 0;     // when the server last took a write (lock stamp)
    this.holdTimer = null;  // waiting out the write interval
    this.holdWake = null;
    this.flushing = false;  // a flush is asking for the write now
    // one editor at a time, shared with the bookmarks menu (utils.js)
    this.lock = new FileLock({
      read: () => this.read(),
      write: (text) => this.write(text),
      body: () => this.safeBody(),
      refresh: () => this.touch(), // goes through the write queue
      onLost: (text) => this.lockLost(text)
    });
  }

  //// server ////

  // both remember whether the server answered, which drives the status dot
  async read() {
    const text = await readFile(FILE_NAME);
    this.online = text !== null;
    return text;
  }

  async write(text) {
    // every write counts against the interval, including the lock claim and
    // the release, so the file really is only sent at most once per window
    this.lastWrite = Date.now();
    this.online = await writeFile(FILE_NAME, text);
    // stamped again on the way out: on a slow link the answer can take most
    // of the interval, and measuring from the start of the exchange would let
    // the next write follow almost immediately behind it
    this.lastWrite = Date.now();
    if (this.online === true) {
      // the lock row went out with it, so the timestamp on the server is now
      // this fresh (see stampFresh)
      this.lastStamp = this.lastWrite;
    }
    return this.online;
  }

  // Every write carries the lock row, so every write is also a heartbeat. A
  // beat landing within one beat of the last confirmed write has nothing to
  // add: the stamp still has a full spare beat in hand before it could
  // expire, so the request is dropped. The second test keeps the margin
  // honest if the timings in utils.js are ever re-tuned.
  stampFresh() {
    return (Date.now() - this.lastStamp) < LOCK_BEAT_MS
      && (LOCK_BEAT_MS * 2) < LOCK_EXPIRE_MS;
  }

  //// file text ////

  // file text -> rows of columns, blank lines dropped
  rows(text) {
    if (typeof text !== 'string' || text === "") {
      return [];
    }
    return splitMenuData(text)
      .map(row => cleanString(row))
      .filter(row => row)
      .map(row => row.split(COL_DELIMITER));
  }

  // 0=ListID, 1=ListName, 2=ItemID, 3=ItemName, 4=CheckState
  // rows arrive in order, and the map keeps them that way
  parse(text) {
    const found = new Map();
    this.rows(text).forEach(cols => {
      if (cols.length < 2 || !isNumeric(cols[0])) {
        return; // not a list row (the lock row lands here)
      }
      const listId = Number(cols[0]);
      let entry = found.get(listId);
      if (!entry) {
        entry = { id: listId, name: cleanName(cols[1]), items: [] };
        found.set(listId, entry);
      }
      // empty item fields mark a list that has no items yet
      const itemName = cleanName(cols[3]);
      if (!isNumeric(cols[2]) || itemName === "") {
        return;
      }
      entry.items.push({
        id: Number(cols[2]),
        name: itemName,
        checked: (cols[4] || '').trim() === '1'
      });
    });
    return [...found.values()];
  }

  // the list rows on their own, without the leading lock row
  buildBody() {
    let _file = "";
    this.data.forEach(entry => {
      const head = `${entry.id}|${entry.name}`;
      if (entry.items.length === 0) {
        _file += head + "|||0\n";
        return;
      }
      entry.items.forEach(item => {
        _file += `${head}|${item.id}|${item.name}|${item.checked === true ? '1' : '0'}\n`;
      });
    });
    return _file;
  }

  // What to write for the list rows. this.data is only worth writing while a
  // list that was actually loaded is standing in it. A session that never
  // loaded -- a read that failed, or one torn down and re-opened while a
  // release was still settling -- holds an empty array, and writing that out
  // would erase the file; the last body known to be good goes back instead,
  // which is what the server already has, so the write changes nothing.
  // A loaded list that the user has genuinely emptied still writes empty.
  safeBody() {
    return this.loaded === true ? this.buildBody() : this.lastGoodBody;
  }

  //// session ////

  // loads the file and takes the edit lock when it is free
  // Loads the file and takes the edit lock when it is free. onData is called
  // as soon as the file is parsed: taking the lock is a second round trip
  // (FileLock.claim writes the lock row back), and on a slow link there is no
  // reason to hold the rows off-screen for it -- nothing below changes what
  // they say, only whether they can be edited.
  async open(onData) {
    // a release from a previous session can still be settling, and it builds
    // its body when it resumes -- let it finish before this one resets the
    // list out from under it
    if (this.releasing) {
      try {
        await this.releasing;
      } catch (err) {
        // a failed release is already reported through the status dot
      }
    }
    this.dirty = false;
    this.sentBody = null;
    this.loaded = false; // nothing trustworthy in this.data until the read is in
    const { text, busy } = await this.lock.check();
    if (text === null) {
      // nothing loaded, so there is nothing safe to write back
      this.data = [];
      this.readOnly = true;
      return;
    }
    this.data = this.parse(text);
    this.loaded = true;
    // What the server holds: claim() writes this same buildBody() text under
    // the lock row, so once the claim lands the file really is this. A save
    // that builds the same text again has nothing to say and is dropped
    // rather than put on the wire.
    this.sentBody = this.buildBody();
    this.lastGoodBody = this.sentBody;
    if (typeof onData === 'function') {
      onData();
    }
    if (busy) {
      this.readOnly = true;
      return;
    }
    this.readOnly = false;
    if (await this.lock.claim(text)) {
      // runs in the background so the window draws without waiting on it
      this.lock.confirm();
    } else {
      this.readOnly = true;
    }
  }

  // a competing window won the lock in the moment we both claimed it
  lockLost(text) {
    this.readOnly = true;
    this.dirty = false;
    this.sentBody = null;
    this.data = this.parse(text);
    showPopup(`[${WIN_TITLE}] another user is editing, opened read only.`);
    this.onChange();
  }

  canWrite() {
    return this.lock.owned === true && this.readOnly !== true;
  }

  // Saves the list. Repeat calls while a write is in flight, or while the
  // write interval is being waited out, are folded into one later write
  // instead of stacking up.
  save() {
    this.dirty = true;
    return this.queueWrite();
  }

  // the lock asking for its timestamp to be re-stamped. It goes out through
  // the same queue, but it is not an edit: marking the window dirty for it
  // would have the close button flushing, and offering to retry, over a file
  // that already matches what is in memory.
  touch() {
    this.keepAlive = true;
    return this.queueWrite();
  }

  queueWrite() {
    if (!this.canWrite()) {
      return Promise.resolve(true);
    }
    this.queued = true;
    if (!this.saving) {
      this.saving = this.runQueue();
    }
    return this.saving;
  }

  async runQueue() {
    let sent = true;
    while (this.queued) {
      await this.holdOff();
      this.queued = false;
      if (!this.canWrite()) {
        // the lock was handed back while the interval was being waited out,
        // and whoever took it back has written the list already
        break;
      }
      const keepAlive = this.keepAlive;
      this.keepAlive = false;
      const body = this.safeBody();
      // Nothing to send: the edits since the last write cancelled each other
      // out (an item ticked and un-ticked, a rename typed and put back), so
      // the file on the server is already this text. A lock heartbeat still
      // goes out, unless a write just re-stamped the lock for it.
      if (body === this.sentBody && (!keepAlive || this.stampFresh())) {
        this.dirty = false;
        continue;
      }
      sent = await this.write(this.lock.line(this.lock.owned) + body);
      this.dirty = !sent;
      if (sent) {
        this.sentBody = body;
        this.lastGoodBody = body; // the server took it, so it is safe to redo
      }
    }
    this.saving = null;
    return sent;
  }

  // waits out whatever is left of the write interval, unless a flush is
  // asking for the write now
  holdOff() {
    const remaining = SAVE_INTERVAL_MS - (Date.now() - this.lastWrite);
    if (remaining <= 0 || this.flushing === true) {
      return Promise.resolve();
    }
    return new Promise(resolve => {
      this.holdWake = resolve;
      this.holdTimer = setTimeout(() => this.wake(), remaining);
    });
  }

  // stops waiting on the interval
  wake() {
    if (this.holdTimer) {
      clearTimeout(this.holdTimer);
      this.holdTimer = null;
    }
    if (this.holdWake) {
      const resume = this.holdWake;
      this.holdWake = null;
      resume();
    }
  }

  // Writes anything outstanding right now, ignoring the interval. Used when
  // the window is closing, and to retry after a failed write.
  async flush() {
    if (!this.canWrite()) {
      return true;
    }
    this.flushing = true;
    if (this.dirty === true) {
      this.queueWrite();
    }
    this.wake();
    const sent = this.saving ? await this.saving : true;
    this.flushing = false;
    return sent;
  }

  // Hands the lock back; unsaved edits are never pushed out on the way (the
  // server is unreachable in that case anyway) -- the lock expires instead.
  // Only ever one at a time: destroy() fires this without waiting, so a
  // re-opened window can call it again while the first is still settling,
  // and two of them would each build their own body.
  release() {
    if (this.releasing) {
      return this.releasing;
    }
    const running = this.runRelease();
    this.releasing = running;
    running.finally(() => {
      if (this.releasing === running) {
        this.releasing = null;
      }
    });
    return running;
  }

  async runRelease() {
    this.queued = false;
    this.keepAlive = false;
    this.wake();
    // A queued write can already be on the wire, carrying the lock row with
    // the lock still held. If that one landed after the release below, the
    // file was left claimed by a window that had closed, and stayed that way
    // until the lock expired. Waiting for it puts the two writes in order.
    if (this.saving) {
      try {
        await this.saving;
      } catch (err) {
        // a failed save is already reported through the status dot
      }
    }
    // The release write carries the list, so it saves whatever the interval
    // was still holding. It is only skipped when the server is known to be
    // unreachable -- then there is nothing to write to, and the lock expires.
    return this.lock.release(this.online !== false);
  }

  // throws the session away without writing anything
  abandon() {
    this.queued = false;
    this.keepAlive = false;
    this.loaded = false; // nothing in this.data may be written after this
    this.wake();
    this.lock.abandon();
    this.dirty = false;
    this.sentBody = null;
  }
}

/*
 *  Lists - the window
 */

export default class Lists {

  constructor() {
    this.store = new ListStore(() => this.render());
    this.view = VIEW_LISTS;
    this.mode = MODE_OPEN;
    this.openId = null;   // list being viewed
    this.editing = false; // inline name box is open
    this.busy = null;     // label shown while the server is being waited on
    this.settling = false; // rows are up, the edit lock has not answered yet
    // rows picked out for deletion, by id. Kept here rather than read back
    // out of the rows, so a re-draw in the middle of selecting keeps them.
    this.selected = new Set();
    // shared drag engine (same grip and behaviour as the bookmarks menu)
    this.dragSort = new DragSort({
      rowClass: 'list__row',
      onDrop: () => this.commitOrder()
    });
    // window level listeners, attached as a set
    this.listeners = {
      click: this.handleClick.bind(this),
      // no re-ordering while a name box is open, or while rows are being
      // picked out for deletion
      pointerdown: (event) => {
        if (!this.editing && this.mode !== MODE_SELECT) {
          this.dragSort.start(event);
        }
      },
      pointermove: (event) => this.dragSort.move(event),
      pointerup: (event) => this.dragSort.end(event),
      pointercancel: (event) => this.dragSort.end(event)
    };
  }

  //// window lifecycle ////

  isOpen() {
    return document.getElementById(WIN_ID) !== null;
  }

  // opens the window, or closes it again if it is already on-screen
  async activate() {
    if (this.isOpen()) {
      this.closeWindow();
      return;
    }
    this.view = VIEW_LISTS;
    this.mode = MODE_OPEN;
    this.openId = null;
    this.drawWindow();
    this.settling = true;
    try {
      // the rows go up on the first round trip; the lock takes the second
      await this.runBusy(BUSY_LOAD, () => this.store.open(() => this.showRows()));
    } finally {
      this.settling = false;
    }
    if (!this.isOpen()) {
      this.store.release(); // closed while loading, hand the lock straight back
      return;
    }
    if (this.store.online === false) {
      showPopup(`[${WIN_TITLE}] server connection not available.`);
    }
    this.render();
  }

  // Close button. Anything the write interval was still holding goes out
  // now; only a write the server refuses stops the window closing.
  async closeWindow() {
    if (this.store.dirty === true) {
      const sent = await this.runBusy(BUSY_SAVE, () => this.store.flush());
      this.updateDot();
      if (!sent) {
        this.showOverlay(true);
        return;
      }
    }
    this.destroy();
  }

  destroy() {
    this.busy = null;
    this.settling = false;
    this.dragSort.cancel();
    this.store.release();
    removeWindow(WIN_ID);
    this.editing = false;
    this.view = VIEW_LISTS;
    this.mode = MODE_OPEN;
    this.openId = null;
    this.selected.clear();
  }

  // Runs an exchange with the server with the label up in place of the rows,
  // so nothing that waits on a slow connection leaves the window blank. Every
  // path that awaits the store goes through here, and the label comes down
  // again whether the exchange worked or threw. A caller that has already
  // taken the label down itself (showRows) is not re-drawn over.
  async runBusy(label, action) {
    this.busy = label;
    this.render();
    try {
      return await action();
    } finally {
      const wasBusy = this.busy !== null;
      this.busy = null;
      if (wasBusy && this.isOpen()) {
        this.render();
      }
    }
  }

  // the file is in and parsed: the rows replace the label straight away, and
  // the buttons stay off until the lock says whether they can be used
  showRows() {
    this.busy = null;
    if (this.isOpen()) {
      this.render();
    }
  }

  // nothing on a row may be edited until the edit lock has answered
  rowsLocked() {
    return this.settling === true || this.store.readOnly === true;
  }

  drawWindow() {
    if (this.isOpen()) {
      return;
    }
    const markup = html`
<div id="${WIN_ID}" class="prompt_win list__win nohide">
  <span class="list__dot" id="${DOT_ID}"></span>
  <div class="list__header">
    <button type="button" class="list__navbtn" id="${BACK_BTN_ID}">
      <i class="fad fa-chevron-left"></i>
    </button>
    <div class="list__title" id="${TITLE_ID}">${WIN_TITLE}</div>
  </div>
  <div class="list__list scroll-bar-style" id="${LIST_ID}"></div>
  <div class="list__actions">
    <div class="list__status" id="${STATUS_ID}">
      <i class="fad fa-lock"></i>Read/Only
    </div>
    <button type="button" class="button button__sml list__btn" id="${CLOSE_BTN_ID}">
      <span class="button__text"><i class="fad fa-times-octagon"></i></span>
    </button>
    <button type="button" class="button button__sml list__btn" id="${DEL_BTN_ID}">
      <span class="button__text"><i class="fad fa-trash-alt" id="${DEL_ICON_ID}"></i></span>
    </button>
    <button type="button" class="button button__sml list__btn" id="${ADD_BTN_ID}">
      <span class="button__text"><i class="fad fa-folder-plus"></i></span>
    </button>
  </div>
  <div class="list__overlay" id="${OVERLAY_ID}">
    <div class="list__dialog">
      <div class="list__dialog-msg">Server connection not available</div>
      <div class="list__dialog-btns">
        <button type="button" class="button list__dlg-btn" id="${DISCARD_BTN_ID}">
          <span class="button__text">Discard</span>
        </button>
        <button type="button" class="button list__dlg-btn" id="${RETRY_BTN_ID}">
          <span class="button__text">Retry</span>
        </button>
      </div>
    </div>
  </div>
</div>`;
    document.body.insertAdjacentHTML('beforeend', markup);
    const win = document.getElementById(WIN_ID);
    Object.keys(this.listeners).forEach(name => {
      win.addEventListener(name, this.listeners[name]);
    });
  }

  //// list data ////

  openList() {
    if (this.openId === null) {
      return null;
    }
    return this.store.data.find(entry => entry.id === this.openId) || null;
  }

  // the array the current view works on
  activeList() {
    if (this.view !== VIEW_ITEMS) {
      return this.store.data;
    }
    const entry = this.openList();
    return entry ? entry.items : [];
  }

  setActiveList(entries) {
    if (this.view !== VIEW_ITEMS) {
      this.store.data = entries;
      return;
    }
    const entry = this.openList();
    if (entry) {
      entry.items = entries;
    }
  }

  findEntry(id) {
    return this.activeList().find(entry => entry.id === id) || null;
  }

  // un-checked items are floated to the top; both groups keep the order they
  // are saved in, so checking an item drops it straight back where it was
  visibleRows() {
    const entries = this.activeList();
    if (this.view !== VIEW_ITEMS) {
      return entries;
    }
    return entries.filter(item => item.checked !== true)
      .concat(entries.filter(item => item.checked === true));
  }

  // items still un-checked, shown on the right of a list row
  openCount(entry) {
    return entry.items.filter(item => item.checked !== true).length;
  }

  // only checked items can be dragged; everything in the list view can
  isMovable(entry) {
    return this.view !== VIEW_ITEMS || entry.checked === true;
  }

  // re-draws, writes the list out and refreshes the connection dot
  commit() {
    this.render();
    this.store.save().then(() => this.updateDot());
  }

  //// drawing ////

  listElem() {
    return document.getElementById(LIST_ID);
  }

  rowById(id) {
    const list = this.listElem();
    return list ? list.querySelector(`.list__row[data-id="${id}"]`) : null;
  }

  render() {
    const list = this.listElem();
    if (!list) {
      return;
    }
    const items = this.view === VIEW_ITEMS;
    const entry = this.openList();
    // title / back button
    document.getElementById(TITLE_ID).innerText =
      items && entry ? entry.name : WIN_TITLE;
    document.getElementById(BACK_BTN_ID).style.visibility =
      items ? "visible" : "hidden";
    // rows
    list.innerHTML = "";
    if (this.busy !== null) {
      // same small text the count uses, until there is a count to show. This
      // stands in for the rows in every view, so moving into a list while the
      // file is still on its way shows the label and not an empty window.
      const note = document.createElement('div');
      note.className = "list__total";
      note.innerText = this.busy;
      list.appendChild(note);
      this.renderActions();
      this.updateDot();
      return;
    }
    const rows = this.visibleRows();
    if (rows.length === 0) {
      const empty = document.createElement('div');
      empty.className = "list__empty";
      empty.innerText = items ? "No items yet" : "No lists yet";
      list.appendChild(empty);
    } else {
      rows.forEach(row => list.appendChild(this.createRow(row)));
      // total sitting at the bottom of the rows: the items in the open list,
      // or every item across all the lists in the main menu
      const count = items
        ? rows.length
        : this.store.data.reduce((sum, entry) => sum + entry.items.length, 0);
      const total = document.createElement('div');
      total.className = "list__total";
      total.innerText = this.totalText(count, items ? 'item' : 'total item');
      list.appendChild(total);
    }
    this.renderActions();
    this.updateDot();
  }

  createRow(entry) {
    const items = this.view === VIEW_ITEMS;
    const select = this.mode === MODE_SELECT;
    const row = document.createElement('div');
    row.className = "list__row";
    row.dataset.id = entry.id;
    if (select) {
      // The round button is the whole left hand side while the mode is on. On
      // a list row it stands in for the grip; on an item row the grip is kept
      // but hidden, so the rounds stay on the column they were already in and
      // the rows do not jump sideways when the mode comes on.
      if (items) {
        row.appendChild(dragGripElement(true));
      }
      row.appendChild(this.roundElement(this.selected.has(entry.id), true));
    } else {
      // a read only window moves nothing
      row.appendChild(dragGripElement(
        this.rowsLocked() || !this.isMovable(entry)));
      if (items) {
        row.appendChild(this.roundElement(entry.checked === true, false));
      }
    }
    const label = document.createElement('span');
    label.className = "list__label";
    label.innerText = entry.name;
    row.appendChild(label);
    // the label is the rename target, so the rest of the row is a separate
    // filler that carries the open / complete tap
    row.appendChild(this.createCell('span', "list__fill", ""));
    if (!items) {
      // how many items in that list are still un-checked
      const count = document.createElement('span');
      count.className = "list__count";
      let unchecked = this.openCount(entry);
      if (unchecked == 0) unchecked = '';
      count.innerText = unchecked;
      row.appendChild(count);
    }
    // the arrow only says "this opens", which delete mode is not doing
    if (!items && !select) {
      row.appendChild(this.createCell('i', "fad fa-chevron-right list__arrow", ""));
    }
    return row;
  }

  // The round button, in both of its jobs: the completed toggle on an item
  // row, and the delete marker on any row while select mode is on. The delete
  // one is the same button in red, so the two never look like the same thing.
  roundElement(on, forDelete) {
    return this.createCell('span', "list__round"
      + (forDelete === true ? " list__round--del" : "")
      + (on === true ? " list__round--on" : ""),
      '<i class="fad fa-check"></i>');
  }

  totalText(count, noun) {
    return `${count} ${noun}${count === 1 ? '' : 's'}`;
  }

  createCell(tag, className, inner) {
    const cell = document.createElement(tag);
    cell.className = className;
    if (inner) {
      cell.innerHTML = inner;
    }
    return cell;
  }

  renderActions() {
    // read only: everything that would write the file is taken off-screen.
    // While the server is being waited on they go too, since the list they
    // would change is not settled yet -- but without the read only notice,
    // which is not the reason.
    const settling = this.settling === true;
    const readOnly = !settling && this.store.readOnly === true;
    const waiting = readOnly || settling || this.busy !== null;
    document.getElementById(STATUS_ID).style.display = readOnly ? "flex" : "none";
    [ADD_BTN_ID, DEL_BTN_ID].forEach(id => {
      document.getElementById(id).style.display = waiting ? "none" : "";
    });
    if (waiting) {
      return;
    }
    // the delete button becomes an X once something is selected, and the
    // icon carries the same red as the rings for as long as the mode is on
    const select = this.mode === MODE_SELECT;
    const confirm = this.selected.size > 0;
    const icon = document.getElementById(DEL_ICON_ID);
    icon.classList.toggle("fa-times", confirm);
    icon.classList.toggle("fa-trash-alt", !confirm);
    icon.classList.toggle(DEL_ICON_ON_CLASS, select);
  }

  // green while the server is answering, red once a write has failed
  updateDot() {
    const dot = document.getElementById(DOT_ID);
    if (!dot) {
      return;
    }
    const online = this.store.online;
    dot.style.display = online === null ? "none" : "block";
    dot.classList.toggle("list__dot--off", online === false);
  }

  showOverlay(show) {
    const overlay = document.getElementById(OVERLAY_ID);
    if (overlay) {
      overlay.style.display = show === true ? "flex" : "none";
    }
  }

  //// actions ////

  // Changing mode always drops what was picked out, so leaving delete mode
  // and coming back never starts with old rows still marked. redraw is off
  // for the callers that are about to draw anyway.
  setMode(mode, redraw = true) {
    this.mode = mode;
    this.selected.clear();
    if (redraw === true) {
      this.render();
    }
  }

  showItems(id) {
    this.openId = id;
    this.view = VIEW_ITEMS;
    this.setMode(MODE_OPEN);
  }

  showLists() {
    this.openId = null;
    this.view = VIEW_LISTS;
    this.setMode(MODE_OPEN);
  }

  addEntry() {
    this.setMode(MODE_OPEN, false);
    const entries = this.activeList();
    const entry = this.view === VIEW_ITEMS
      ? { id: nextId(entries), name: "New Item", checked: false }
      : { id: nextId(entries), name: "New List", items: [] };
    entries.push(entry);
    this.render();
    // open the name box on the new row
    const row = this.rowById(entry.id);
    if (row && typeof row.scrollIntoView === 'function') {
      row.scrollIntoView({ block: 'nearest' });
    }
    this.beginRename(entry.id, true);
  }

  // first press turns on the selection check boxes, the second either deletes
  // what is selected or leaves select mode again; either way the list ends up
  // back in its normal state
  deleteAction() {
    if (this.mode !== MODE_SELECT) {
      this.setMode(MODE_SELECT);
      return;
    }
    if (this.selected.size === 0) {
      this.setMode(MODE_OPEN);
      return;
    }
    const picked = this.selected;
    this.setActiveList(this.activeList().filter(entry => !picked.has(entry.id)));
    this.setMode(MODE_OPEN, false); // the delete is done, commit draws it
    this.commit();
  }

  // marks a row for deletion, or takes the mark off again. The button is
  // turned over in place rather than through a re-draw, so a long list does
  // not rebuild every time one row is picked.
  toggleSelected(id, row) {
    const on = !this.selected.has(id);
    if (on) {
      this.selected.add(id);
    } else {
      this.selected.delete(id);
    }
    const round = row.querySelector('.list__round');
    if (round) {
      round.classList.toggle("list__round--on", on);
    }
    this.renderActions();
  }

  toggleItem(id) {
    const item = this.findEntry(id);
    if (item) {
      item.checked = !item.checked;
      this.commit();
    }
  }

  beginRename(id, isNew = false) {
    const row = this.rowById(id);
    const entry = this.findEntry(id);
    const label = row ? row.querySelector('.list__label') : null;
    if (!label || !entry || this.editing || this.rowsLocked()) {
      return;
    }
    this.editing = true;
    const namebox = document.createElement('input');
    namebox.type = "text";
    namebox.className = "list__namebox";
    namebox.setAttribute("autocorrect", "off");
    namebox.setAttribute("autocapitalize", "none");
    namebox.value = entry.name;
    this.sizeNamebox(namebox, row, label);
    label.replaceWith(namebox);
    namebox.focus();
    namebox.select();
    let closed = false;
    const finish = (save) => {
      if (closed) {
        return;
      }
      closed = true;
      this.editing = false;
      const name = save === true ? cleanName(namebox.value) : "";
      if (name !== "") {
        entry.name = name;
      } else if (isNew === true) {
        // a new entry left un-named is dropped again
        this.setActiveList(this.activeList().filter(other => other.id !== id));
      } else {
        this.render(); // nothing changed, no write needed
        return;
      }
      this.commit();
    };
    namebox.addEventListener('blur', () => finish(true));
    namebox.addEventListener('keydown', (key_event) => {
      if (key_event.key === 'Enter' || key_event.key === 'Escape') {
        key_event.preventDefault();
        finish(key_event.key === 'Enter');
      }
    });
  }

  // The box takes over the space the name and the filler beside it already
  // occupy, so the row keeps the width and height it had before the rename
  // started (rows are max-content, so a box that sized itself would drag the
  // whole window wider). The filler steps aside for the duration.
  sizeNamebox(namebox, row, label) {
    const fill = row.querySelector('.list__fill');
    const labelBox = label.getBoundingClientRect();
    const rightEdge = fill ? fill.getBoundingClientRect().right : labelBox.right;
    const width = Math.round(rightEdge - labelBox.left);
    const height = Math.round(labelBox.height);
    if (width <= 0 || height <= 0) {
      return; // nothing rendered to measure against
    }
    namebox.style.width = `${width}px`;
    namebox.style.height = `${height}px`;
    if (fill) {
      // the box now covers the filler and the gap that was between them
      fill.style.display = "none";
    }
  }

  //// unsaved changes dialog ////

  async retrySave() {
    const sent = await this.runBusy(BUSY_SAVE, () => this.store.flush());
    this.updateDot();
    if (sent) {
      this.showOverlay(false);
      this.destroy();
    }
  }

  discardChanges() {
    this.store.abandon();
    this.showOverlay(false);
    this.destroy();
  }

  //// click handling ////

  handleClick(event) {
    const button = event.target.closest('button');
    if (button) {
      this.handleButton(button.id);
      return;
    }
    const row = event.target.closest('.list__row');
    if (!row || this.editing || event.target.closest('.drag-grip')) {
      return;
    }
    const id = Number(row.dataset.id);
    if (this.mode === MODE_SELECT) {
      // tapping anywhere on the row, the round button included, selects it
      this.toggleSelected(id, row);
      return;
    }
    // tapping the name (or the few pixels of padding around it) renames
    if (!this.rowsLocked() && event.target.closest('.list__label')) {
      this.beginRename(id, false);
      return;
    }
    if (this.view !== VIEW_ITEMS) {
      this.showItems(id);
      return;
    }
    if (!this.rowsLocked()) {
      this.toggleItem(id); // checking an item writes the file
    }
  }

  handleButton(id) {
    switch (id) {
      // the unsaved changes dialog owns the window while it is up
      case RETRY_BTN_ID:
        this.retrySave();
        return;
      case DISCARD_BTN_ID:
        this.discardChanges();
        return;
      case CLOSE_BTN_ID:
        this.closeWindow();
        return;
      case BACK_BTN_ID:
        this.showLists();
        return;
    }
    // read only, and the wait for the lock, allow nothing that writes
    if (this.rowsLocked()) {
      return;
    }
    if (id === ADD_BTN_ID) {
      this.addEntry();
    } else if (id === DEL_BTN_ID) {
      this.deleteAction();
    }
  }

  //// drag to re-order ////

  // reads the new order back out of the window and saves it; the rows that
  // cannot be dragged keep the slots they already hold, so an un-checked item
  // stays where it is until it is checked again
  commitOrder() {
    const list = this.listElem();
    if (!list) {
      return;
    }
    const entries = this.activeList();
    const slots = [];
    entries.forEach((entry, idx) => {
      if (this.isMovable(entry)) { slots.push(idx); }
    });
    const moved = [...list.getElementsByClassName('list__row')]
      .map(row => entries.find(entry => entry.id === Number(row.dataset.id)))
      .filter(entry => entry && this.isMovable(entry));
    if (moved.length !== slots.length) {
      return;
    }
    moved.forEach((entry, idx) => { entries[slots[idx]] = entry; });
    this.commit();
  }
}
