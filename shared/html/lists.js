// Automate - lists window. ListStore holds the data, the file and the edit lock; Lists is
// the window (default export). Both levels support add, rename, multi-select delete and drag
// re-order. Unchecked items float to the top; checked ones drag, and every item does while
// none is checked. A list of over SEARCH_MIN_ITEMS items has a search box that hides rows
// in place. Add puts an unsaved placeholder row first; once named it keeps that place.
//
// lists.save, one line per item: LIST_ID|LIST_NAME|ITEM_ID|ITEM_NAME|CHECK_STATE. Row order is
// the order; an empty list's line has empty item fields. The first window to open claims
// lists-lock.save; others are read only. Nothing saves on a timer: only on close, hide and
// unload. The corner dot: green taken, yellow outstanding, red after a failed write.
//
// An encrypted list is one row, LIST_ID|!ENC|0|E1.<iv>.<ciphertext>|0. While locked, its
// blob sits in memory undrawn and is written back untouched. A list added while the encrypted
// lists are open is one of them.

import { fileRows, boundedText, cleanString, isNumeric, removeWindow,
  dragGripElement, DragSort, FileLock, readFile, writeFile, sendFileBeacon,
  html, searchRowElement, searchTerm, searchMatches, makeElement,
  singleFlight, trackPromise, awaitQuiet, gridButtonTemplate,
  reportFileFailure } from './utils.js';
import { showPopup, reportException } from './error.js';
import { FileCrypt, cryptoReady, isCryptBlob, cryptContext,
  encryptText } from './encrypt.js';

const FILE_NAME = 'lists.save';
const LOCK_FILE = 'lists-lock.save';
const PWD_FILE = 'lists-pwd.save';
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
const ROW_CLASS = 'list__row';
const LABEL_CLASS = 'list__label';
const LABEL_NEW_CLASS = 'list__label--new';
const ROUND_CLASS = 'list__round';
const ROUND_ON_CLASS = 'list__round--on';
// the search box on a long list
const SEARCH_BOX_ID = 'list__search-box';
const NO_MATCH_ID = 'list__nomatch';
const ROW_HIDE_CLASS = 'list__row--hide';
const LIST_FIND_CLASS = 'list__list--find';
// items a list must hold before the box is offered
const SEARCH_MIN_ITEMS = 30;
// the lock button and its password prompt
const LOCK_BTN_ID = 'list__btn-lock';
const LOCK_ICON_ID = 'list__lock-icon';
const LOCK_ICON_ON_CLASS = 'list__lock-icon--on';
const PWD_OVERLAY_ID = 'list__pwd';
const PWD_MSG_ID = 'list__pwd-msg';
const PWD_BOX_ID = 'list__pwd-box';
const PWD_BOX2_ID = 'list__pwd-box2';
const PWD_BOX3_ID = 'list__pwd-box3';
const PWD_ERR_ID = 'list__pwd-err';
const PWD_OK_BTN_ID = 'list__btn-pwd-ok';
const PWD_CANCEL_BTN_ID = 'list__btn-pwd-cancel';
const PWD_BOX_IDS = [PWD_BOX_ID, PWD_BOX2_ID, PWD_BOX3_ID];
const ROW_LOCK_CLASS = 'list__rowlock';
const LOCK_HOLD_CLASS = 'list__lock-icon--hold';
// how long the lock is held down to offer a password change
const LOCK_HOLD_MS = 2500;
const DOT_UNSAVED_CLASS = 'list__dot--unsaved';
const DOT_SAVE_CLASS = 'list__dot--save';
const SPINNER_ID = 'list__spinner';
const SPINNER_ON_CLASS = 'list__spinner--on';
const DOT_ERROR_CLASS = 'list__dot--error';
const COL_DELIMITER = '|';
// dot states
const DOT_HIDDEN = 0;  // nothing tried yet
const DOT_SAVED = 1;   // the server holds what is on screen
const DOT_UNSAVED = 2; // edits the server has not been given
const DOT_ERROR = 3;   // a write failed
// Stored names are printable ASCII without "|". An item may hold several lines, stored
// with "~" for each newline, so "~" is taken out of the text. List names stay on one line.
const MAX_NAME_LENGTH = 64;
const MAX_ITEM_LENGTH = 1000;
const NEWLINE_MARK = '~';
const COMBINING_MARKS = /[\u0300-\u036f]/g;
const NON_ASCII = /[^\x20-\x7E]/g;
const NON_ASCII_LINES = /[^\x20-\x7E\n]/g; // with newlines kept
const RESERVED_CHARS = /[\0\r\n|~]/g;
const RESERVED_CHARS_LINES = /[\0|~]/g;    // with newlines kept
const EXTRA_SPACES = /\s{2,}/g;
const EXTRA_BLANKS = /[^\S\n]{2,}/g;       // newlines spared
// window views
const VIEW_LISTS = 0;
const VIEW_ITEMS = 1;
// list modes
const MODE_OPEN = 0;
const MODE_SELECT = 1;
// the name column of an encrypted row
const ENC_MARK = '!ENC';
// password prompt modes
// password prompt modes
const PWD_SETUP = 0;  // pick one, twice
const PWD_UNLOCK = 1; // enter it
const PWD_CHANGE = 2; // the old one and the new one, twice
// what the prompt says in each, and how many boxes it shows
const PWD_FORMS = {
  [PWD_SETUP]: { heading: "Set a lists password", action: "Set", boxes: 2,
    first: "New password", second: "Repeat password", fill: "new-password" },
  [PWD_UNLOCK]: { heading: "Enter lists password", action: "Unlock", boxes: 1,
    first: "Password", second: "Repeat password", fill: "current-password" },
  [PWD_CHANGE]: { heading: "Change lists password", action: "Change", boxes: 3,
    first: "Current password", second: "New password", fill: "current-password" }
};

//// shared helpers ////

// The cleaner both names go through: printable ASCII, runs of spaces closed up. An item
// keeps its line breaks (as "\n") and may run longer. cleanName(" Café | x ") -> "Cafe x"
function cleanText(value, multi) {
  let raw = '';
  if (value !== null && value !== undefined) {
    raw = String(value);
  }
  let limit = MAX_NAME_LENGTH;
  let outside = NON_ASCII;       // characters the file cannot carry
  let reserved = RESERVED_CHARS; // characters it uses for itself
  let runs = EXTRA_SPACES;       // whitespace to close up
  if (multi === true) {
    limit = MAX_ITEM_LENGTH;
    outside = NON_ASCII_LINES;
    reserved = RESERVED_CHARS_LINES;
    runs = EXTRA_BLANKS;
    // both CRLF and a lone CR become one newline
    raw = raw.replace(/\r\n|\r/g, '\n');
  } else {
    raw = cleanString(raw) || '';
  }
  const name = raw
    .normalize("NFD")             // accents split off their base letter
    .replace(COMBINING_MARKS, '')
    .replace(outside, '')
    .replace(reserved, '')
    .replace(runs, ' ')
    .trim();
  return boundedText(name, limit);
}

// a list name, cleaned
function cleanName(value) {
  return cleanText(value, false);
}

// an item name, cleaned
function cleanItemName(value) {
  return cleanText(value, true);
}

// item text -> its stored row form
function encodeLines(value) {
  return String(value).split('\n').join(NEWLINE_MARK);
}

// stored row form -> item text
function decodeLines(value) {
  return String(value).split(NEWLINE_MARK).join('\n');
}

// the next free id in a list of entries
function nextId(entries) {
  return entries.reduce((highest, entry) => Math.max(highest, entry.id), -1) + 1;
}

// document.getElementById, short
function byId(id) {
  return document.getElementById(id);
}

// Shows or hides an element, by id or itself; shown takes the display mode given.
function setShown(target, shown, mode = "") {
  let element = target;
  if (typeof target === 'string') {
    element = byId(target);
  }
  if (!element) {
    return;
  }
  let display = "none";
  if (shown === true) {
    display = mode;
  }
  element.style.display = display;
}

// raises a popup with the window's name
function warn(text) {
  showPopup(`[${WIN_TITLE}] ${text}`);
}

//// ListStore: list data, file transfer and the edit lock ////

class ListStore {

  // onChange() is called when the store changes something the window must re-draw
  constructor(onChange) {
    this.onChange = onChange;
    this.data = [];         // [{ id, name, items: [{ id, name, checked }] }]
    this.readOnly = false;  // another user holds the lock, or nothing loaded
    this.online = null;     // last list exchange: true, false or not tried
    this.dirty = false;     // edits the server has not been given
    this.saving = null;     // save in flight, so writes cannot overlap
    this.sentBody = null;   // the list text the server is known to hold
    this.wireBody = null;   // the list text on the wire right now
    this.loaded = false;    // a parsed list stands in this.data
    this.lastGoodBody = ""; // last body safe to write back
    this.releasing = null;  // release in flight, so two cannot race
    this.sealing = null;    // re-encrypt in flight, awaited before a write
    // the encrypted lists' password and key; starts locked
    this.crypt = new FileCrypt({ file: PWD_FILE });
    // The lock's own traffic stays out of this.online: the dot reports the list only.
    this.lock = new FileLock({
      file: LOCK_FILE,
      onLost: () => this.lockLost()
    });
  }

  //// server ////

  // reads the file, noting whether the server answered
  async read() {
    const text = await readFile(FILE_NAME);
    this.online = text !== null;
    return text;
  }

  // writes the file, noting whether the server answered
  async write(text) {
    this.online = await writeFile(FILE_NAME, text);
    return this.online;
  }

  //// file text ////

  // File text -> rows of columns. Characters outside printable ASCII are transit damage
  // and are taken out first.
  rows(text) {
    if (typeof text !== 'string' || text === "") {
      return [];
    }
    return fileRows(text)
      .map(row => row.replace(NON_ASCII, ''))
      .filter(row => row)
      .map(row => row.split(COL_DELIMITER));
  }

  // An encrypted row -> its entry, kept whether or not it can be opened, so a locked
  // window writes it back as it arrived.
  sealedEntry(listId, cols) {
    const blob = (cols[3] || '').trim();
    if (blob === "") {
      return null;
    }
    return {
      id: listId,
      name: "",
      items: [],
      enc: true,     // held encrypted
      open: false,   // its blob is decrypted into name / items
      stale: false,  // edited since the blob was made
      bad: !isCryptBlob(blob),
      blob: blob
    };
  }

  // file text -> the list data: 0=ListID, 1=ListName, 2=ItemID, 3=ItemName, 4=CheckState
  parse(text) {
    const found = new Map();
    this.rows(text).forEach(cols => {
      if (cols.length < 2 || !isNumeric(cols[0])) {
        return; // not a list row (an old lock row lands here)
      }
      const listId = Number(cols[0]);
      if (cols[1] === ENC_MARK) {
        if (!found.has(listId)) {
          const sealed = this.sealedEntry(listId, cols);
          if (sealed) {
            found.set(listId, sealed);
          }
        }
        return;
      }
      let entry = found.get(listId);
      if (!entry) {
        entry = { id: listId, name: cleanName(cols[1]), items: [] };
        found.set(listId, entry);
      }
      if (entry.enc === true) {
        return; // a plain row cannot join an encrypted list
      }
      // empty item fields mark a list with no items yet
      const itemName = cleanItemName(decodeLines(cols[3]));
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

  // the list rows, ready to write
  buildBody() {
    let _file = "";
    this.data.forEach(entry => {
      // an unnamed placeholder is not saved
      if (entry.pending === true) {
        return;
      }
      if (entry.enc === true) {
        // one row, the blob all of it; a blob gone missing takes its row
        if (typeof entry.blob === 'string' && entry.blob !== "") {
          _file += `${entry.id}|${ENC_MARK}|0|${entry.blob}|0\n`;
        }
        return;
      }
      const head = `${entry.id}|${entry.name}`;
      const items = this.savedItems(entry);
      if (items.length === 0) {
        _file += head + "|||0\n";
        return;
      }
      items.forEach(item => {
        let checked = '0';
        if (item.checked === true) {
          checked = '1';
        }
        // newlines go in the file as "~"
        _file += `${head}|${item.id}|${encodeLines(item.name)}|${checked}\n`;
      });
    });
    return _file;
  }

  // the items worth writing: not the placeholder
  savedItems(entry) {
    return entry.items.filter(item => item.pending !== true);
  }

  // What to write: this.data once a list has loaded, or the last good body, so a session
  // that never loaded cannot write an empty array over the file.
  safeBody() {
    if (this.loaded === true) {
      return this.buildBody();
    }
    return this.lastGoodBody;
  }

  //// encrypted lists ////

  // is the key held?
  unlocked() {
    return this.crypt.isUnlocked();
  }

  // is this an unopened encrypted list, which the window must not draw?
  isHidden(entry) {
    return entry.enc === true && entry.open !== true;
  }

  // the encrypted lists, in whatever state
  sealed() {
    return this.data.filter(entry => entry.enc === true);
  }

  // a list as the one string that goes inside its blob
  sealText(entry) {
    return JSON.stringify({
      n: entry.name,
      i: this.savedItems(entry).map(item => {
        let checked = 0;
        if (item.checked === true) {
          checked = 1;
        }
        return { i: item.id, n: item.name, c: checked };
      })
    });
  }

  // and back again; anything not that shape is refused
  readSealed(entry, text) {
    let held = null;
    try {
      held = JSON.parse(text);
    } catch (error) {
      return false;
    }
    if (!held || typeof held.n !== 'string' || !Array.isArray(held.i)) {
      return false;
    }
    entry.name = cleanName(held.n);
    entry.items = held.i
      .filter(item => item && isNumeric(String(item.i)))
      .map(item => ({
        id: Number(item.i),
        name: cleanItemName(String(item.n)),
        checked: Number(item.c) === 1
      }))
      .filter(item => item.name !== "");
    return true;
  }

  // Opens every sealed list the key fits; one that will not open stays sealed and counted.
  async unsealAll() {
    let opened = 0;
    let failed = 0;
    if (!this.unlocked()) {
      return { opened: opened, failed: failed };
    }
    for (const entry of this.sealed()) {
      if (entry.open === true) {
        continue;
      }
      const text = await this.crypt.decrypt(entry.blob, cryptContext(FILE_NAME, entry.id));
      if (text === null || !this.readSealed(entry, text)) {
        failed += 1;
        continue;
      }
      entry.open = true;
      entry.stale = false;
      opened += 1;
    }
    return { opened: opened, failed: failed };
  }

  // Re-encrypts every open list edited since its blob was made, with a fresh IV.
  async sealOpen() {
    if (!this.unlocked()) {
      return true;
    }
    let done = true;
    for (const entry of this.sealed()) {
      if (entry.open !== true || entry.stale !== true) {
        continue;
      }
      const blob = await this.crypt.encrypt(
        this.sealText(entry), cryptContext(FILE_NAME, entry.id));
      if (blob === null) {
        done = false; // the key went meanwhile
        continue;
      }
      entry.blob = blob;
      entry.stale = false;
    }
    return done;
  }

  // Seals in the background after an edit, as the unload beacon cannot wait for one.
  sealSoon() {
    return trackPromise(this, 'sealing', this.sealOpen());
  }

  // waits for a background seal; one that threw leaves the entry stale, for the next
  async settleSeal() {
    await awaitQuiet(this.sealing);
  }

  // Shuts the encrypted lists: blobs brought up to date, then plaintext and key go. The
  // caller saves first.
  async lockUp() {
    await this.sealOpen();
    this.sealed().forEach(entry => {
      entry.name = "";
      entry.items = [];
      entry.open = false;
      entry.stale = false;
    });
    this.crypt.lock();
  }

  // Makes a list being added encrypted; without the key it does nothing, leaving it plain.
  // The blob comes with the seal after the first edit.
  setSealed(entry) {
    if (!entry || !this.unlocked()) {
      return false;
    }
    entry.enc = true;
    entry.open = true;
    entry.stale = true;
    entry.blob = null;
    return true;
  }

  //// session ////

  // The file into this.data, unsealed where the key fits. A failed read marks the session
  // unloaded, which stops it being written back.
  async load() {
    const text = await this.read();
    if (text === null) {
      this.loaded = false;
      return false;
    }
    this.data = this.parse(text);
    this.loaded = true;
    // a window re-opened with the key in hand does not ask again
    if (this.unlocked()) {
      await this.unsealAll();
    }
    return true;
  }

  // Loads the list and takes the edit lock when free. onData() runs once parsed; the
  // claim only changes whether rows can be edited.
  async open(onData) {
    // a previous session's release builds its body when it resumes
    await awaitQuiet(this.releasing);
    this.dirty = false;
    this.sentBody = null;
    this.loaded = false;
    // two files, asked for together
    const [ok, lockText] = await Promise.all([this.load(), this.lock.read()]);
    if (!ok) {
      // nothing loaded, so nothing is safe to write back
      this.data = [];
      this.readOnly = true;
      return;
    }
    // what the server holds: a save rebuilding this same text is dropped
    this.sentBody = this.buildBody();
    this.lastGoodBody = this.sentBody;
    if (typeof onData === 'function') {
      onData();
    }
    // a one row write to the lock file
    if (await this.lock.claim(lockText)) {
      this.readOnly = false;
      // in the background, so the window draws without waiting
      this.lock.confirm();
    } else {
      this.readOnly = true;
      // the lock was free and still not taken: the server did not answer
      if (!this.lock.busy(lockText)) {
        this.online = false;
      }
    }
  }

  // Another window won the lock as both claimed: re-reads, as the winner may write.
  async lockLost() {
    this.readOnly = true;
    this.dirty = false;
    this.sentBody = null;
    await this.load();
    warn("another user is editing, opened read only.");
    this.onChange();
  }

  // may this window write?
  canWrite() {
    return this.lock.owned === true && this.readOnly !== true;
  }

  // An edit was made: turns the dot yellow, writing nothing.
  markChanged() {
    this.dirty = true;
    // every open list is marked and re-sealed, keeping blobs ready for a write that cannot wait
    const open = this.sealed().filter(entry => entry.open === true);
    open.forEach(entry => { entry.stale = true; });
    if (open.length > 0) {
      this.sealSoon();
    }
  }

  // Dirty without an edit in memory: a rekey replaced the blobs, already current.
  markBlobsChanged() {
    this.dirty = true;
  }

  // Writes the list if the server is behind; a second caller waits, then writes what is outstanding.
  save() {
    const previous = this.saving;
    return trackPromise(this, 'saving', this.runSave(previous));
  }

  // the save itself
  async runSave(previous) {
    await awaitQuiet(previous);
    if (!this.canWrite()) {
      return true; // read only, or the lock has gone back
    }
    if (this.dirty !== true) {
      return true; // the server already holds this list
    }
    // nothing goes out until every encrypted list is up to date
    await this.settleSeal();
    await this.sealOpen();
    const body = this.safeBody();
    if (body === this.sentBody) {
      // the edits cancelled each other out
      this.dirty = false;
      return true;
    }
    // what is on the wire, so the unload beacon does not send it twice
    this.wireBody = body;
    const sent = await this.write(body);
    this.wireBody = null;
    if (sent === true) {
      this.sentBody = body;
      this.lastGoodBody = body;
      this.dirty = false;
    }
    return sent;
  }

  // Last-chance save as the page goes away, sending the blobs sealSoon() already made.
  saveBeacon() {
    if (!this.canWrite() || this.dirty !== true) {
      return false;
    }
    const body = this.safeBody();
    // the hide's save is often still on the wire
    if (body === this.wireBody) {
      return false;
    }
    const handed = sendFileBeacon(FILE_NAME, body);
    if (handed === true) {
      // taken on trust: no answer to wait for
      this.sentBody = body;
      this.lastGoodBody = body;
      this.dirty = false;
    }
    return handed;
  }

  // Hands the lock back as the page goes away, or it stays held until the stamp expires.
  releaseBeacon() {
    if (this.lock.owned !== true) {
      return false;
    }
    const handed = sendFileBeacon(LOCK_FILE, this.lock.line(false));
    if (handed === true) {
      this.lock.abandon();
      // a page in the back/forward cache comes back; reclaim() undoes this
      this.readOnly = true;
    }
    return handed;
  }

  // takes the lock again after a back/forward cache restore
  async reclaim() {
    if (this.lock.owned === true) {
      return true;
    }
    if (this.loaded !== true) {
      return false; // nothing worth holding the lock for
    }
    const lockText = await this.lock.read();
    if (await this.lock.claim(lockText)) {
      this.readOnly = false;
      this.lock.confirm();
      return true;
    }
    this.readOnly = true;
    return false;
  }

  // Writes anything unsaved and hands the lock back, one at a time.
  release() {
    return singleFlight(this, 'releasing', () => this.runRelease());
  }

  // the release itself
  async runRelease() {
    if (this.online === false) {
      // nothing to write to, so the lock is left to expire
      this.lock.abandon();
      return false;
    }
    // anything unsaved goes first, while the lock is held
    const sent = await this.save();
    // worth freeing even if that write failed: the edits stay in memory
    const freed = await this.lock.release();
    if (freed !== true) {
      return false;
    }
    return sent;
  }

  // throws the session away without writing anything
  abandon() {
    this.loaded = false; // nothing in this.data may be written after this
    this.lock.abandon();
    this.dirty = false;
    this.sentBody = null;
  }
}

//// Lists: the window ////

export default class Lists {

  // set up by activate()
  constructor() {
    this.store = new ListStore(() => this.render());
    this.view = VIEW_LISTS;
    this.mode = MODE_OPEN;
    this.openId = null;    // list being viewed
    this.editing = false;  // inline name box is open
    this.pendingId = null; // placeholder row, not saved
    this.pendingRows = null; // the array it was added to
    this.query = "";       // what is in the search box
    this.pwdMode = null;   // password prompt mode, or not up
    this.holdTimer = null; // the lock is being held down
    this.held = false;     // held long enough, so the press is spent
    this.epoch = 0;        // counts openings, so a late teardown knows its own
    this.busy = false;     // the server is being waited on
    this.settling = false; // rows are up, the edit lock has not answered
    // rows picked for deletion, by id, so a re-draw keeps them
    this.selected = new Set();
    this.dragSort = new DragSort({
      rowClass: ROW_CLASS,
      onDrop: () => this.commitOrder()
    });
    // window level listeners
    const endPress = (event) => {
      this.holdCancel();
      this.dragSort.end(event);
    };
    this.listeners = {
      click: this.handleClick.bind(this),
      pointerdown: (event) => {
        this.holdStart(event);
        if (this.dragAllowed()) {
          this.dragSort.start(event);
        }
      },
      pointermove: (event) => {
        // a finger sliding off the lock has changed its mind
        this.holdCheck(event);
        this.dragSort.move(event);
      },
      pointerup: endPress,
      pointercancel: endPress
    };
    // Page level listeners for the instance's life; each checks the lock first.
    this.pageListeners = {
      hidden: () => this.handleHidden(),
      unload: () => this.handleUnload(),
      restore: (event) => this.handleRestore(event)
    };
    document.addEventListener('visibilitychange', this.pageListeners.hidden);
    window.addEventListener('pagehide', this.pageListeners.unload);
    window.addEventListener('unload', this.pageListeners.unload);
    window.addEventListener('pageshow', this.pageListeners.restore);
  }

  //// saving ////

  // The page went into the background, still alive: the write can be awaited.
  handleHidden() {
    if (document.visibilityState !== 'hidden' || this.store.dirty !== true) {
      return;
    }
    this.store.save().then(() => this.updateDot());
  }

  // The page is being torn down: the browser is handed the list to finish. iOS Safari
  // often skips this, and the lock expiring covers that.
  handleUnload() {
    // the list first, while the lock is held
    this.store.saveBeacon();
    // then the lock, or the next window opens read only
    this.store.releaseBeacon();
  }

  // A back/forward cache restore: the lock went back on the way out, so asks again.
  async handleRestore(event) {
    if (event.persisted !== true) {
      return; // an ordinary load starts from nothing anyway
    }
    if (!this.isOpen()) {
      return;
    }
    await this.store.reclaim();
    this.render();
  }

  //// window lifecycle ////

  // is the window on screen
  isOpen() {
    return byId(WIN_ID) !== null;
  }

  // opens the window, or closes it when open
  async activate() {
    if (this.isOpen()) {
      this.closeWindow();
      return;
    }
    this.epoch += 1;
    this.resetView();
    this.drawWindow();
    this.settling = true;
    try {
      // the rows go up on the first round trip; the lock takes the second
      await this.runBusy(() => this.store.open(() => this.showRows()));
    } finally {
      this.settling = false;
    }
    if (!this.isOpen()) {
      this.store.release(); // closed while loading
      return;
    }
    if (this.store.online === false) {
      reportFileFailure('opening the lists');
    }
    this.render();
  }

  // Writes the list with the ring turning, leaving the dot on the answer.
  async saveOut() {
    const sent = await this.runBusy(() => this.store.save());
    this.updateDot();
    return sent;
  }

  // Close button: anything unsaved goes now; only a refused write stops it closing.
  async closeWindow() {
    if (this.store.dirty === true && !await this.saveOut()) {
      this.showOverlay(true);
      return;
    }
    this.destroy();
  }

  // takes the window down; the page listeners stay
  destroy() {
    this.busy = false;
    this.settling = false;
    this.holdCancel();
    this.held = false;
    this.dragSort.cancel();
    // the release's write needs the key, so the key is dropped after it
    const releasing = this.store.release();
    // a window re-opened meanwhile must not have the key taken off it
    this.epoch += 1;
    const token = this.epoch;
    Promise.resolve(releasing)
      .catch(() => {})
      .then(() => {
        if (this.epoch === token) {
          return this.store.lockUp();
        }
      })
      .catch(() => {});
    removeWindow(WIN_ID);
    this.closePassword();
    this.editing = false;
    this.pendingId = null;
    this.pendingRows = null;
    this.resetView();
  }

  // The state a session starts on, used at both ends.
  resetView() {
    this.view = VIEW_LISTS;
    this.mode = MODE_OPEN;
    this.openId = null;
    this.query = "";
    this.selected.clear();
  }

  // Ends the instance: destroy() plus the page listeners.
  dispose() {
    document.removeEventListener('visibilitychange', this.pageListeners.hidden);
    window.removeEventListener('pagehide', this.pageListeners.unload);
    window.removeEventListener('unload', this.pageListeners.unload);
    window.removeEventListener('pageshow', this.pageListeners.restore);
    this.destroy();
  }

  // Runs a server exchange with the ring turning; a caller that stopped it (showRows) is
  // not re-drawn over.
  async runBusy(action) {
    this.busy = true;
    this.renderBusy();
    try {
      return await action();
    } finally {
      const wasBusy = this.busy === true;
      this.busy = false;
      if (wasBusy) {
        this.renderBusy();
      }
    }
  }

  // Re-draws only the ring and buttons; a full render() blinks the rows on every save.
  renderBusy() {
    if (!this.isOpen()) {
      return;
    }
    this.renderActions();
    this.updateDot();
  }

  // the file is parsed: the rows go up, the buttons wait on the lock
  showRows() {
    this.busy = false;
    if (this.isOpen()) {
      this.render();
    }
  }

  // nothing on a row may be edited until the edit lock has answered
  rowsLocked() {
    return this.settling === true || this.store.readOnly === true;
  }

  // draws the empty window
  drawWindow() {
    if (this.isOpen()) {
      return;
    }
    // the back button starts hidden: the window opens on the lists view
    const markup = html`
<div id="${WIN_ID}" class="prompt_win list__win nohide">
  <span class="list__dot" id="${DOT_ID}"></span>
  <span class="list__spinner" id="${SPINNER_ID}"></span>
  <div class="list__header">
    <button type="button" class="list__navbtn" id="${BACK_BTN_ID}"
      style="display: none;">
      <i class="fad fa-chevron-left"></i>
    </button>
    <button type="button" class="list__navbtn" id="${LOCK_BTN_ID}">
      <i class="fad fa-lock" id="${LOCK_ICON_ID}"></i>
    </button>
    <div class="list__title" id="${TITLE_ID}">${WIN_TITLE}</div>
  </div>
  <div class="list__list scroll-bar-style" id="${LIST_ID}"></div>
  <div class="list__actions">
    ${gridButtonTemplate({
      buttonId: CLOSE_BTN_ID,
      icon: 'fad fa-times-octagon',
      buttonClass: 'button__sml list__btn'
    })}
    ${gridButtonTemplate({
      buttonId: DEL_BTN_ID,
      icon: 'fad fa-trash-alt',
      iconId: DEL_ICON_ID,
      buttonClass: 'button__sml list__btn'
    })}
    ${gridButtonTemplate({
      buttonId: ADD_BTN_ID,
      icon: 'fad fa-folder-plus',
      buttonClass: 'button__sml list__btn'
    })}
    <div class="list__status" id="${STATUS_ID}">
      <i class="fad fa-lock"></i>Read/Only
    </div>
  </div>
  <div class="list__overlay" id="${OVERLAY_ID}">
    <div class="list__dialog">
      <div class="list__dialog-msg">Server connection not available</div>
      <div class="list__dialog-btns">
        ${gridButtonTemplate({
          buttonId: DISCARD_BTN_ID,
          label: 'Discard',
          buttonClass: 'list__dlg-btn'
        })}
        ${gridButtonTemplate({
          buttonId: RETRY_BTN_ID,
          label: 'Retry',
          buttonClass: 'list__dlg-btn'
        })}
      </div>
    </div>
  </div>
  <div class="list__overlay" id="${PWD_OVERLAY_ID}">
    <div class="list__dialog">
      <div class="list__dialog-msg" id="${PWD_MSG_ID}">Enter password</div>
      <input type="password" class="list__namebox list__pwbox" id="${PWD_BOX_ID}"
        autocorrect="off" autocapitalize="none" spellcheck="false">
      <input type="password" class="list__namebox list__pwbox" id="${PWD_BOX2_ID}"
        autocorrect="off" autocapitalize="none" spellcheck="false"
        placeholder="Repeat password">
      <input type="password" class="list__namebox list__pwbox" id="${PWD_BOX3_ID}"
        autocorrect="off" autocapitalize="none" spellcheck="false"
        placeholder="Repeat new password">
      <div class="list__pwerr" id="${PWD_ERR_ID}"></div>
      <div class="list__dialog-btns">
        ${gridButtonTemplate({
          buttonId: PWD_CANCEL_BTN_ID,
          label: 'Cancel',
          buttonClass: 'list__dlg-btn'
        })}
        ${gridButtonTemplate({
          buttonId: PWD_OK_BTN_ID,
          label: 'Unlock',
          buttonClass: 'list__dlg-btn'
        })}
      </div>
    </div>
  </div>
</div>`;
    document.body.insertAdjacentHTML('beforeend', markup);
    const win = byId(WIN_ID);
    Object.keys(this.listeners).forEach(name => {
      win.addEventListener(name, this.listeners[name]);
    });
    // Enter answers the prompt and Escape drops it
    PWD_BOX_IDS.forEach(id => {
      byId(id).addEventListener('keydown', (key_event) => {
        if (key_event.key === 'Enter') {
          key_event.preventDefault();
          this.passwordSubmit();
        } else if (key_event.key === 'Escape') {
          key_event.preventDefault();
          this.closePassword();
        }
      });
    });
  }

  //// list data ////

  // the list being viewed, or null
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
    if (!entry) {
      return [];
    }
    return entry.items;
  }

  // replaces the array the current view works on
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

  // one entry of the current view
  findEntry(id) {
    return this.activeList().find(entry => entry.id === id) || null;
  }

  // unchecked items float to the top; both groups keep their saved order
  visibleRows() {
    const entries = this.activeList();
    if (this.view !== VIEW_ITEMS) {
      // a locked list is not drawn at all
      return entries.filter(entry => !this.store.isHidden(entry));
    }
    return entries.filter(item => item.checked !== true)
      .concat(entries.filter(item => item.checked === true));
  }

  // items still unchecked, shown on a list row
  openCount(entry) {
    return entry.items.filter(item => item.checked !== true).length;
  }

  // Records an edit and re-draws; nothing goes to the server here.
  commit() {
    this.store.markChanged();
    this.render();
  }

  //// drawing ////

  // the rows' panel
  listElem() {
    return byId(LIST_ID);
  }

  // a row's element by id
  rowById(id) {
    const list = this.listElem();
    if (!list) {
      return null;
    }
    return list.querySelector(`.${ROW_CLASS}[data-id="${id}"]`);
  }

  // every row's element
  rowElements() {
    const list = this.listElem();
    if (!list) {
      return [];
    }
    return [...list.getElementsByClassName(ROW_CLASS)];
  }

  // re-draws the header, rows, buttons and dot
  render() {
    const list = this.listElem();
    if (!list) {
      return;
    }
    // lockLost() can re-draw mid-drag, replacing the rows the drag holds
    this.dragSort.cancel();
    // the search box is rebuilt, so its focus is noted and put back
    const oldBox = byId(SEARCH_BOX_ID);
    const searching = oldBox !== null && document.activeElement === oldBox;
    const items = this.view === VIEW_ITEMS;
    this.renderHeader(items);
    list.innerHTML = "";
    if (this.busy === true && this.store.loaded !== true) {
      // waiting on the first read: no rows yet, and the ring says so
      this.renderActions();
      this.updateDot();
      return;
    }
    // the search box, on a list long enough, above the rows
    if (this.searchable()) {
      list.appendChild(this.searchRow());
    } else {
      this.query = "";
    }
    this.renderRows(list, items);
    // the rows are new, so what is in the box goes over them
    this.applyFilter();
    this.clampScroll(list);
    this.renderActions();
    this.updateDot();
    if (searching) {
      this.refocusSearch();
    }
  }

  // the title and the two buttons in the header's left corner
  renderHeader(items) {
    const entry = this.openList();
    let title = WIN_TITLE;
    if (items && entry) {
      title = entry.name;
    }
    byId(TITLE_ID).innerText = title;
    // The back button belongs to the items view and the lock to the lists view; they share
    // the slot, so the centred title does not move.
    const backBtn = byId(BACK_BTN_ID);
    setShown(backBtn, items === true);
    setShown(LOCK_BTN_ID, items !== true);
    this.updateLockIcon();
  }

  // the rows, the no-match placeholder and the total beneath them
  renderRows(list, items) {
    const rows = this.visibleRows();
    if (rows.length === 0) {
      const empty = makeElement('div', "list__empty");
      let emptyText = "No lists yet";
      if (items) {
        emptyText = "No items yet";
      }
      empty.innerText = emptyText;
      list.appendChild(empty);
      return;
    }
    // which rows drag is worked out once per view
    const movable = this.movableRule();
    rows.forEach(entry => list.appendChild(this.createRow(entry, movable(entry))));
    // stands in while a search matches nothing; shown by applyFilter()
    if (this.searchable()) {
      const none = makeElement('div', "list__empty");
      none.id = NO_MATCH_ID;
      none.innerText = "No matches";
      setShown(none, false);
      list.appendChild(none);
    }
    // the items in the open list, or every item across all lists
    let count = rows.length;
    let noun = 'item';
    if (!items) {
      noun = 'total item';
      count = this.store.data.reduce((sum, entry) => sum + entry.items.length, 0);
    }
    const total = makeElement('div', "list__total");
    total.innerText = this.totalText(count, noun);
    list.appendChild(total);
  }

  // puts the caret back in the search box after a re-draw
  refocusSearch() {
    const box = byId(SEARCH_BOX_ID);
    if (!box) {
      return;
    }
    box.focus();
    // focus() alone selects the whole value
    const end = box.value.length;
    box.setSelectionRange(end, end);
  }

  // one row's element
  createRow(entry, movable) {
    const items = this.view === VIEW_ITEMS;
    const select = this.mode === MODE_SELECT;
    // list rows are taller
    let rowClass = ROW_CLASS;
    if (!items) {
      rowClass += " list__row--tall";
    }
    const row = makeElement('div', rowClass);
    row.dataset.id = entry.id;
    // what the search box matches, folded once here
    row.dataset.find = String(entry.name).toLowerCase();
    if (select) {
      // In select mode the round button is the left side. A list row has none of its own;
      // an item row keeps a hidden grip, so the rounds stay in their column.
      if (items) {
        row.appendChild(dragGripElement(true));
      }
      row.appendChild(this.roundElement(this.selected.has(entry.id), true));
    } else {
      // a read only window moves nothing
      row.appendChild(dragGripElement(this.rowsLocked() || movable !== true));
      if (items) {
        row.appendChild(this.roundElement(entry.checked === true, false));
      }
    }
    // the encrypted list's badge; the stylesheet removes it beside a name label
    if (!items && entry.enc === true) {
      row.appendChild(this.rowLockElement());
    }
    // a multi-line item wraps; a single line keeps its ellipsis
    let labelClass = LABEL_CLASS;
    if (String(entry.name).indexOf('\n') !== -1) {
      labelClass += " list__label--multi";
    }
    // a placeholder's name is what to press
    if (entry.pending === true) {
      labelClass += ` ${LABEL_NEW_CLASS}`;
    }
    const label = makeElement('span', labelClass);
    label.innerText = entry.name;
    row.appendChild(label);
    // the label renames, so the filler takes the open / tick tap
    row.appendChild(makeElement('span', "list__fill"));
    if (!items) {
      // how many items are unchecked
      const count = makeElement('span', "list__count");
      let unchecked = this.openCount(entry);
      if (unchecked == 0) unchecked = '';
      count.innerText = unchecked;
      row.appendChild(count);
      // the arrow says "this opens", which delete mode is not doing
      if (!select) {
        row.appendChild(makeElement('i', "fad fa-chevron-right list__arrow"));
      }
    }
    return row;
  }

  // The completed toggle on an item row, or the red delete marker in select mode.
  roundElement(on, forDelete) {
    let className = ROUND_CLASS;
    if (forDelete === true) {
      className += " list__round--del";
    }
    if (on === true) {
      className += ` ${ROUND_ON_CLASS}`;
    }
    return makeElement('span', className, '<i class="fad fa-check"></i>');
  }

  // the lock badge on an encrypted list row, which only reports
  rowLockElement() {
    return makeElement('span', ROW_LOCK_CLASS, '<i class="fad fa-lock"></i>');
  }

  // the header lock: shut while the encrypted lists are, open once they are
  updateLockIcon() {
    const icon = byId(LOCK_ICON_ID);
    if (!icon) {
      return;
    }
    const open = this.store.unlocked();
    icon.classList.toggle("fa-lock-open", open);
    icon.classList.toggle("fa-lock", !open);
    icon.classList.toggle(LOCK_ICON_ON_CLASS, open);
    const button = byId(LOCK_BTN_ID);
    if (button) {
      let title = "Unlock encrypted lists";
      if (open) {
        title = "Lock encrypted lists";
      }
      button.title = title;
    }
  }

  // "1 item" / "4 items"
  totalText(count, noun) {
    if (count === 1) {
      return `${count} ${noun}`;
    }
    return `${count} ${noun}s`;
  }

  // shows the buttons the current state allows
  renderActions() {
    // read only hides what would write, with a notice; waiting hides it without
    const settling = this.settling === true;
    const readOnly = !settling && this.store.readOnly === true;
    // a save leaves the buttons alone
    const loading = this.busy === true && this.store.loaded !== true;
    const waiting = readOnly || settling || loading;
    setShown(STATUS_ID, readOnly, "flex");
    [ADD_BTN_ID, DEL_BTN_ID].forEach(id => setShown(id, !waiting));
    if (waiting) {
      return;
    }
    // the delete button becomes an X once something is picked
    const select = this.mode === MODE_SELECT;
    const confirm = this.selected.size > 0;
    const icon = byId(DEL_ICON_ID);
    icon.classList.toggle("fa-times", confirm);
    icon.classList.toggle("fa-trash-alt", !confirm);
    icon.classList.toggle(DEL_ICON_ON_CLASS, select);
  }

  // green once the server has the edits, yellow until then, red after a failed write
  dotState() {
    if (this.store.online === null) {
      return DOT_HIDDEN;
    }
    if (this.store.online === false) {
      return DOT_ERROR;
    }
    if (this.store.dirty === true) {
      return DOT_UNSAVED;
    }
    return DOT_SAVED;
  }

  // paints the ring and the dot
  updateDot() {
    // the ring turns while the server is waited on
    const spinner = byId(SPINNER_ID);
    if (spinner) {
      spinner.classList.toggle(SPINNER_ON_CLASS, this.busy === true);
    }
    const dot = byId(DOT_ID);
    if (!dot) {
      return;
    }
    const state = this.dotState();
    if (state === DOT_HIDDEN) {
      setShown(dot, false);
      return;
    }
    setShown(dot, true, "block");
    dot.classList.toggle(DOT_UNSAVED_CLASS, state === DOT_UNSAVED);
    dot.classList.toggle(DOT_ERROR_CLASS, state === DOT_ERROR);
    // pressing it saves, so it takes presses only with something to write
    const canSave = this.store.dirty === true && this.store.canWrite();
    dot.classList.toggle(DOT_SAVE_CLASS, canSave);
    if (canSave) {
      dot.title = "Save changes";
    } else {
      dot.removeAttribute('title');
    }
  }

  // the dot pressed: writes now
  async saveNow() {
    if (this.store.dirty !== true || !this.store.canWrite()) {
      return;
    }
    await this.saveOut();
  }

  // shows or hides the unsaved changes dialog
  showOverlay(show) {
    setShown(OVERLAY_ID, show === true, "flex");
  }

  //// searching ////

  // Only the items view, and only a list too long to scan by eye.
  searchable() {
    if (this.view !== VIEW_ITEMS) {
      return false;
    }
    const entry = this.openList();
    return entry !== null && entry.items.length > SEARCH_MIN_ITEMS;
  }

  // what is being searched for, ready to match
  searchText() {
    return searchTerm(this.query);
  }

  // are rows being held back
  filtering() {
    return this.searchable() && this.searchText() !== "";
  }

  // A long list's top row; applyFilter() hides rows in place, so the box keeps its focus.
  searchRow() {
    return searchRowElement({
      rowClass: "list__search",
      boxId: SEARCH_BOX_ID,
      maxLength: MAX_NAME_LENGTH,
      // a re-draw brings back what was typed
      value: this.query,
      onSearch: (value) => {
        this.query = value;
        this.applyFilter();
      }
    });
  }

  // Hides the rows that do not match, leaving ids, selection and saved order untouched.
  applyFilter() {
    const list = this.listElem();
    if (!list) {
      return;
    }
    if (!this.searchable()) {
      list.classList.remove(LIST_FIND_CLASS);
      return;
    }
    const query = this.searchText();
    const rows = this.rowElements();
    let shown = 0;
    rows.forEach(row => {
      const hit = searchMatches(row.dataset.find, query);
      row.classList.toggle(ROW_HIDE_CLASS, !hit);
      if (hit) {
        shown += 1;
      }
    });
    // the grips go too: what is on screen is not the whole list
    list.classList.toggle(LIST_FIND_CLASS, query !== "");
    setShown(NO_MATCH_ID, query !== "" && shown === 0, "block");
    // the count follows what is on screen
    const total = list.querySelector('.list__total');
    if (total) {
      if (query === "") {
        total.innerText = this.totalText(rows.length, 'item');
      } else {
        total.innerText = `${shown} of ${rows.length} items`;
      }
    }
    // the rows taken away can leave the panel scrolled past its end
    this.clampScroll(list);
  }

  //// actions ////

  // Changes mode, dropping what was picked; redraw is off for callers about to draw.
  setMode(mode, redraw = true) {
    this.mode = mode;
    this.selected.clear();
    if (redraw === true) {
      this.render();
    }
  }

  // Into a list or back out: an empty search, nothing picked, the panel at the top.
  showView(view, id = null) {
    this.query = "";
    this.openId = id;
    this.view = view;
    this.setMode(MODE_OPEN);
    this.resetScroll();
  }

  // Every view starts at the top. On iOS a fling overrides a scrollTop write, so the
  // overflow is taken away for a moment to end it.
  resetScroll() {
    const list = this.listElem();
    if (!list) {
      return;
    }
    const overflow = list.style.overflowY;
    list.style.overflowY = "hidden";
    list.scrollTop = 0;
    // reading a layout value applies the change now
    void list.offsetHeight;
    list.style.overflowY = overflow;
    list.scrollTop = 0;
  }

  // Rows can leave without the view changing, leaving the panel past its end.
  clampScroll(list) {
    let furthest = list.scrollHeight - list.clientHeight;
    if (furthest < 0) {
      // the rows no longer fill the panel
      furthest = 0;
    }
    if (list.scrollTop > furthest) {
      list.scrollTop = furthest;
    }
  }

  // The add button: a placeholder, unsaved and gone unless pressed and named.
  addEntry() {
    // the search goes first, or the row could arrive out of view
    this.query = "";
    this.setMode(MODE_OPEN, false);
    this.dropPending();
    const entries = this.activeList();
    let entry = { id: nextId(entries), name: "New list", items: [], pending: true };
    if (this.view === VIEW_ITEMS) {
      entry = { id: nextId(entries), name: "New item", checked: false, pending: true };
    } else {
      // encrypted if added while the encrypted lists are open
      this.store.setSealed(entry);
    }
    // first in the saved order, so first on screen
    entries.unshift(entry);
    // the row to name, and the array it sits in, as the view can change under it
    this.pendingId = entry.id;
    this.pendingRows = entries;
    this.render();
    // it is the first row
    this.resetScroll();
  }

  // Takes the placeholder away, spliced in place; the caller re-draws.
  dropPending() {
    if (this.pendingId === null) {
      return false;
    }
    const id = this.pendingId;
    const rows = this.pendingRows;
    this.pendingId = null;
    this.pendingRows = null;
    if (!Array.isArray(rows)) {
      return false;
    }
    const at = rows.findIndex(entry => entry.id === id);
    if (at === -1) {
      return false;
    }
    rows.splice(at, 1);
    return true;
  }

  // First press turns the check boxes on; the second deletes what is picked, or leaves.
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
    this.setMode(MODE_OPEN, false);
    this.commit();
  }

  // Marks a row for deletion or unmarks it, in place, so a long list does not rebuild.
  toggleSelected(id, row) {
    const on = !this.selected.has(id);
    if (on) {
      this.selected.add(id);
    } else {
      this.selected.delete(id);
    }
    const round = row.querySelector(`.${ROUND_CLASS}`);
    if (round) {
      round.classList.toggle(ROUND_ON_CLASS, on);
    }
    this.renderActions();
  }

  // ticks or unticks an item
  toggleItem(id) {
    const item = this.findEntry(id);
    if (item) {
      item.checked = !item.checked;
      this.commit();
    }
  }

  // the box a name is edited in: an input for a list, a textarea for an item
  makeNamebox(multi, value) {
    let namebox = null;
    let className = "list__namebox";
    if (multi) {
      namebox = document.createElement('textarea');
      namebox.rows = 1;
      namebox.maxLength = MAX_ITEM_LENGTH;
      className += " list__namebox--multi";
    } else {
      namebox = document.createElement('input');
      namebox.type = "text";
    }
    namebox.className = className;
    namebox.setAttribute("autocorrect", "off");
    namebox.setAttribute("autocapitalize", "none");
    namebox.value = value;
    return namebox;
  }

  // opens the inline name box on a row
  beginRename(id) {
    const row = this.rowById(id);
    const entry = this.findEntry(id);
    let label = null;
    if (row) {
      label = row.querySelector(`.${LABEL_CLASS}`);
    }
    if (!label || !entry || this.editing || this.rowsLocked()) {
      return;
    }
    this.editing = true;
    const multi = this.view === VIEW_ITEMS;
    // a placeholder opens empty: its words are a prompt, not a name
    const pending = entry.pending === true;
    let value = entry.name;
    if (pending) {
      value = "";
    }
    const namebox = this.makeNamebox(multi, value);
    this.sizeNamebox(namebox, row, label);
    label.replaceWith(namebox);
    if (multi) {
      // opens at the height its text needs
      this.growNamebox(namebox);
    }
    namebox.focus();
    if (!pending) {
      namebox.select();
    }
    let closed = false;
    const finish = (save) => {
      if (closed) {
        return;
      }
      closed = true;
      this.editing = false;
      let name = "";
      if (save === true) {
        if (multi) {
          name = cleanItemName(namebox.value);
        } else {
          name = cleanName(namebox.value);
        }
      }
      if (name !== "") {
        entry.name = name;
        if (pending) {
          // named at last, so this is the edit that saves it
          delete entry.pending;
          this.pendingId = null;
          this.pendingRows = null;
        }
      } else if (pending) {
        // left unnamed and never written
        this.dropPending();
        this.render();
        return;
      } else {
        this.render();
        return;
      }
      this.commit();
    };
    namebox.addEventListener('blur', () => finish(true));
    if (multi) {
      namebox.addEventListener('input', () => this.growNamebox(namebox));
    }
    namebox.addEventListener('keydown', (key_event) => {
      if (key_event.key === 'Escape') {
        key_event.preventDefault();
        finish(false);
        return;
      }
      if (key_event.key !== 'Enter') {
        return;
      }
      if (multi) {
        // Enter opens the next line of an item; tapping away commits
        return;
      }
      key_event.preventDefault();
      finish(true);
    });
  }

  // Sets the item box to the height its text needs. Height is cleared first, or a grown box
  // never measures smaller; border-box adds the border back on top of scrollHeight.
  growNamebox(namebox) {
    namebox.style.height = 'auto';
    const border = namebox.offsetHeight - namebox.clientHeight;
    namebox.style.height = `${namebox.scrollHeight + border}px`;
  }

  // The box takes the space of the name and filler, so the row keeps its size; the
  // filler steps aside.
  sizeNamebox(namebox, row, label) {
    const fill = row.querySelector('.list__fill');
    const labelBox = label.getBoundingClientRect();
    let rightEdge = labelBox.right;
    if (fill) {
      rightEdge = fill.getBoundingClientRect().right;
    }
    const width = Math.round(rightEdge - labelBox.left);
    const height = Math.round(labelBox.height);
    if (width <= 0 || height <= 0) {
      return; // nothing rendered to measure
    }
    namebox.style.width = `${width}px`;
    namebox.style.height = `${height}px`;
    if (fill) {
      // the box now covers the filler
      fill.style.display = "none";
    }
  }

  //// unsaved changes dialog ////

  // the dialog's Retry
  async retrySave() {
    const sent = await this.saveOut();
    if (sent) {
      this.showOverlay(false);
      this.destroy();
    }
  }

  // the dialog's Discard
  discardChanges() {
    this.store.abandon();
    this.showOverlay(false);
    this.destroy();
  }

  //// encrypted lists ////

  // WebCrypto needs a secure origin; warns when it is missing.
  cryptoOk() {
    if (cryptoReady()) {
      return true;
    }
    warn("encryption needs an https connection.");
    return false;
  }

  // warns when another user holds the lock
  writable() {
    if (this.store.canWrite()) {
      return true;
    }
    warn("another user is editing, opened read only.");
    return false;
  }

  // whether a password has been set, or null if the record could not be read
  async cryptState() {
    const state = await this.runBusy(() => this.store.crypt.status());
    if (state === null) {
      reportFileFailure('reading the lists password record');
    }
    return state;
  }

  // The lock in the corner: unlocked, it locks; locked, it asks for the password, or a new one.
  async lockPress() {
    if (!this.cryptoOk()) {
      return;
    }
    if (this.store.unlocked()) {
      await this.lockLists();
      return;
    }
    const state = await this.cryptState();
    if (state === null) {
      return;
    }
    if (state.ready === true) {
      this.showPassword(PWD_UNLOCK);
      return;
    }
    // no password yet, and setting one writes a file
    if (!this.writable()) {
      return;
    }
    this.showPassword(PWD_SETUP);
  }

  // Shuts the encrypted lists, saving first as the key is the only way to encrypt what is in
  // memory. A failed write leaves everything open.
  async lockLists() {
    if (this.store.dirty === true && !await this.saveOut()) {
      reportFileFailure('saving the lists before locking them');
      return false;
    }
    await this.store.lockUp();
    // the list being viewed may have just gone
    const entry = this.openList();
    if (this.view === VIEW_ITEMS && (!entry || this.store.isHidden(entry))) {
      this.showView(VIEW_LISTS);
      return true;
    }
    this.setMode(this.mode, false);
    this.render();
    return true;
  }

  //// password prompt ////

  // the three boxes, top to bottom
  pwdBoxes() {
    return PWD_BOX_IDS.map(id => byId(id));
  }

  // Shows the prompt for a mode from PWD_FORMS. The first box is always the known password.
  showPassword(mode) {
    const form = PWD_FORMS[mode];
    if (!form || !byId(PWD_OVERLAY_ID)) {
      return;
    }
    this.pwdMode = mode;
    const [first, second, third] = this.pwdBoxes();
    byId(PWD_MSG_ID).innerText = form.heading;
    byId(PWD_OK_BTN_ID).querySelector('.button__text').innerText = form.action;
    this.pwdBoxes().forEach(box => { box.value = ""; });
    first.placeholder = form.first;
    second.placeholder = form.second;
    first.setAttribute("autocomplete", form.fill);
    second.setAttribute("autocomplete", "new-password");
    third.setAttribute("autocomplete", "new-password");
    setShown(second, form.boxes > 1);
    setShown(third, form.boxes > 2);
    this.showPasswordError("");
    setShown(PWD_OVERLAY_ID, true, "flex");
    first.focus();
  }

  // A wrong answer: the reason shows, the boxes named are emptied, the first takes focus.
  showPasswordError(text, ...clear) {
    const line = byId(PWD_ERR_ID);
    if (line) {
      line.innerText = text || "";
    }
    clear.forEach(id => { byId(id).value = ""; });
    if (clear.length > 0) {
      byId(clear[0]).focus();
    }
  }

  // closes the prompt, leaving no password in the boxes
  closePassword() {
    setShown(PWD_OVERLAY_ID, false);
    this.pwdBoxes().forEach(box => {
      if (box) {
        box.value = "";
      }
    });
    this.pwdMode = null;
    this.showPasswordError("");
  }

  // answers the prompt
  async passwordSubmit() {
    if (this.pwdMode === null) {
      return;
    }
    const [first, second, third] = this.pwdBoxes();
    const password = first.value;
    const setup = this.pwdMode === PWD_SETUP;
    if (this.pwdMode === PWD_CHANGE) {
      await this.passwordChange(password, second.value, third.value);
      return;
    }
    if (setup && password !== second.value) {
      this.showPasswordError("passwords do not match", PWD_BOX2_ID);
      return;
    }
    // the derivation is deliberately slow
    let out = null;
    if (setup) {
      out = await this.runBusy(() => this.store.crypt.setup(password));
    } else {
      out = await this.runBusy(() => this.store.crypt.unlock(password));
    }
    if (!this.isOpen()) {
      return; // the window went meanwhile
    }
    if (out.ok !== true) {
      this.showPasswordError(out.error, PWD_BOX_ID, PWD_BOX2_ID);
      return;
    }
    this.closePassword();
    // what was already encrypted opens now
    const found = await this.runBusy(() => this.store.unsealAll());
    if (found.failed > 0) {
      reportException('unlocking the lists',
        `${found.failed} encrypted list(s) would not open.`);
    }
    this.render();
  }

  //// changing the password ////

  // A press began: on the lock, it starts the hold that offers a password change.
  holdStart(event) {
    this.holdCancel();
    // a hold that opened the prompt leaves a click to swallow; a fresh press clears it
    this.held = false;
    if (!event.target.closest(`#${LOCK_BTN_ID}`)) {
      return;
    }
    this.holdIcon(true);
    this.holdTimer = setTimeout(() => {
      this.holdTimer = null;
      this.held = true; // the click that follows is spent
      this.holdIcon(false);
      this.changePress();
    }, LOCK_HOLD_MS);
  }

  // a finger that slid off the lock cancels the hold
  holdCheck(event) {
    if (this.holdTimer === null) {
      return;
    }
    if (!event.target.closest(`#${LOCK_BTN_ID}`)) {
      this.holdCancel();
    }
  }

  // cancels the hold
  holdCancel() {
    if (this.holdTimer !== null) {
      clearTimeout(this.holdTimer);
      this.holdTimer = null;
    }
    this.holdIcon(false);
  }

  // the icon warms up over the hold
  holdIcon(on) {
    const icon = byId(LOCK_ICON_ID);
    if (icon) {
      icon.classList.toggle(LOCK_HOLD_CLASS, on === true);
    }
  }

  // the lock was held for its full time
  async changePress() {
    // the re-encrypted lists go back to the server, so this needs the lock
    if (!this.cryptoOk() || !this.writable()) {
      return;
    }
    const state = await this.cryptState();
    if (state === null) {
      return;
    }
    if (state.ready !== true) {
      warn("no list password has been set.");
      return;
    }
    this.showPassword(PWD_CHANGE);
  }

  // changes the password, re-encrypting every list
  async passwordChange(oldPassword, newPassword, repeated) {
    // change() unlocks with the old password, so whether the lists were open is noted first
    const wasOpen = this.store.unlocked();
    if (newPassword !== repeated) {
      this.showPasswordError("new passwords do not match", PWD_BOX3_ID);
      return;
    }
    const out = await this.runBusy(() => this.store.crypt.change(
      oldPassword, newPassword, (nextKey) => this.rekeyLists(nextKey)));
    if (!this.isOpen()) {
      return; // the window went meanwhile
    }
    if (out.ok !== true) {
      this.showPasswordError(out.error, PWD_BOX_ID, PWD_BOX2_ID, PWD_BOX3_ID);
      return;
    }
    this.closePassword();
    // the new key stays only if lists were open
    if (!wasOpen) {
      await this.store.lockUp();
    }
    this.updateDot();
    this.render();
    if (out.error) {
      warn(`${out.error}.`);
    } else {
      warn("password changed.");
    }
  }

  // Handed the new key by change(): opens every encrypted list with the old key, seals it
  // under the new one and writes the file. A write that fails restores the old blobs, so
  // the file and the password record cannot disagree.
  async rekeyLists(nextKey) {
    // blobs brought up to date first, under the old key, after any background seal
    await this.store.settleSeal();
    if (await this.store.sealOpen() !== true) {
      return false; // one would not seal, so nothing is changed
    }
    const swapped = [];
    for (const entry of this.store.sealed()) {
      const context = cryptContext(FILE_NAME, entry.id);
      const text = await this.store.crypt.decrypt(entry.blob, context);
      if (text === null) {
        return false; // one will not open, so nothing is changed
      }
      const blob = await encryptText(nextKey, text, context);
      if (blob === null) {
        return false;
      }
      swapped.push({ entry: entry, next: blob, held: entry.blob });
    }
    if (swapped.length === 0) {
      return true; // nothing encrypted yet
    }
    swapped.forEach(one => { one.entry.blob = one.next; });
    this.store.markBlobsChanged();
    const sent = await this.store.save();
    if (sent !== true) {
      swapped.forEach(one => { one.entry.blob = one.held; });
    }
    return sent;
  }

  //// click handling ////

  // routes a press on the window
  handleClick(event) {
    if (event.target.closest(`#${DOT_ID}`)) {
      this.saveNow();
      return;
    }
    // The placeholder goes when anything else is pressed. The pressed row is read first, as
    // the re-draw replaces its element.
    const pressed = event.target.closest(`.${ROW_CLASS}`);
    let id = null;
    if (pressed) {
      id = Number(pressed.dataset.id);
    }
    if (id !== this.pendingId && this.dropPending()) {
      this.render();
    }
    const button = event.target.closest('button');
    if (button) {
      this.handleButton(button.id);
      return;
    }
    if (id === null) {
      return;
    }
    const row = this.rowById(id);
    if (!row) {
      return;
    }
    if (this.editing || event.target.closest('.drag-grip')) {
      return;
    }
    // a press anywhere on the placeholder names it
    if (id === this.pendingId) {
      if (!this.rowsLocked()) {
        this.beginRename(id);
      }
      return;
    }
    if (this.mode === MODE_SELECT) {
      // anywhere on the row selects it
      this.toggleSelected(id, row);
      return;
    }
    // the name renames
    if (!this.rowsLocked() && event.target.closest(`.${LABEL_CLASS}`)) {
      this.beginRename(id);
      return;
    }
    if (this.view !== VIEW_ITEMS) {
      this.showView(VIEW_ITEMS, id);
      return;
    }
    if (!this.rowsLocked()) {
      this.toggleItem(id);
    }
  }

  // routes a window button
  handleButton(id) {
    switch (id) {
      // the dialogs own the window while up
      case RETRY_BTN_ID:
        this.retrySave();
        return;
      case DISCARD_BTN_ID:
        this.discardChanges();
        return;
      case PWD_OK_BTN_ID:
        this.passwordSubmit();
        return;
      case PWD_CANCEL_BTN_ID:
        this.closePassword();
        return;
      case CLOSE_BTN_ID:
        this.closeWindow();
        return;
      case BACK_BTN_ID:
        this.showView(VIEW_LISTS);
        return;
      // unlocking only reads, so a read only window has it too
      case LOCK_BTN_ID:
        if (this.held === true) {
          // the hold answered this press
          this.held = false;
          return;
        }
        this.lockPress();
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

  // May a press start a drag? Not with a name box open, rows being picked, or a search on.
  dragAllowed() {
    return !this.editing && this.mode !== MODE_SELECT && !this.filtering();
  }

  // The test for which rows in this view drag: never a placeholder; every drawn list; the
  // checked items, or every item while none is checked.
  movableRule() {
    if (this.view !== VIEW_ITEMS) {
      return entry => entry.pending !== true && !this.store.isHidden(entry);
    }
    const noneChecked = this.activeList().every(item => item.checked !== true);
    return entry => entry.pending !== true && (noneChecked || entry.checked === true);
  }

  // Movable rows can only have swapped among their own slots, so each slot takes the movable
  // row now drawn in its turn.
  commitOrder() {
    if (!this.listElem()) {
      return;
    }
    const entries = this.activeList();
    const movable = this.movableRule();
    const slots = [];
    entries.forEach((entry, at) => {
      if (movable(entry)) {
        slots.push(at);
      }
    });
    const held = new Map(entries.map(entry => [entry.id, entry]));
    const drawn = this.rowElements()
      .map(row => held.get(Number(row.dataset.id)))
      .filter(entry => entry && movable(entry));
    // the screen and the list disagree on what moves: leave both
    if (drawn.length !== slots.length) {
      return;
    }
    drawn.forEach((entry, at) => { entries[slots[at]] = entry; });
    this.commit();
  }
}
