// Automate Website - lists window
//
// ListStore holds the list data, the file and the edit lock; Lists is the
// window itself (default export, used by main.js).
//
// A centred window of lists; selecting one swaps the window over to the items
// inside it. Both levels support add, rename, multi-select delete and drag
// re-order. Items carry a round completed button: un-checked float to the
// top, checked sit below in their saved order. Checked ones drag, and while
// none in the list is checked every item does. Delete mode turns that same
// round button red. A list of more than
// SEARCH_MIN_ITEMS items carries a search box that hides non-matching rows in
// place, without re-drawing or re-ordering anything.
//
// Add puts a placeholder row at the top rather than a row: not saved, not
// counted, not draggable, and gone again unless it is pressed and named. Once
// named it keeps that place, so new lists and items lead their view.
//
// Saved file (lists.save), one line per item:
//   LIST_ID|LIST_NAME|ITEM_ID|ITEM_NAME|CHECK_STATE
// Row order carries the order of the lists and their items, so nothing holds
// a position number. A list with no items keeps its line with the item fields
// empty. A row that is not a list row (an older leading LOCK| row) is dropped
// the first time the list is saved.
//
// The edit lock lives in lists-lock.save (FileLock in utils.js): the first
// window to open claims it, one opened while it is held is read only.
//
// Nothing is written on a timer. The list is saved on the close button, when
// the page is hidden, and when it is unloaded. The dot in the top right
// corner says where those edits stand: green taken, yellow outstanding, red
// after a failed write -- and a failed write keeps the edits in memory, with
// the close offering to retry or discard.
//
// Encrypted lists
//
// An encrypted list is one row holding its name and every item as a single
// AES-256-GCM blob:
//   LIST_ID|!ENC|0|E1.<iv>.<ciphertext>|0
// so nothing about it is in the file as itself, and an older parser reads the
// row as a list with no items rather than as damage. The key comes from a
// password through the utils.js helpers; only a hash of the verifier is
// stored, in lists-pwd.save, and the key never leaves this tab. While locked
// these lists are not drawn, not counted and not opened: they sit in memory
// as the blobs they arrived as and are written back untouched.
//
// A list added while the encrypted lists are open is one of them; one added
// while they are shut is plain. The lock in the corner is the switch.

import { splitMenuData, cleanString, isNumeric, removeWindow, showPopup,
  dragGripElement, DragSort, FileLock, readFile, writeFile, sendFileBeacon,
  html, FileCrypt, cryptoReady, isCryptBlob, cryptContext, encryptText,
  searchRowElement, searchTerm, searchMatches, makeElement, singleFlight,
  trackPromise, awaitQuiet, gridButtonTemplate } from './utils.js';

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
// the search box on a long list, and what it does to the rows below it
const SEARCH_BOX_ID = 'list__search-box';
const NO_MATCH_ID = 'list__nomatch';
const ROW_HIDE_CLASS = 'list__row--hide';
const LIST_FIND_CLASS = 'list__list--find';
// how many items a list has to hold before the box is offered
const SEARCH_MIN_ITEMS = 30;
// lock button and the password prompt it raises
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
// how long the lock has to be held down to offer a password change
const LOCK_HOLD_MS = 2500;
const DOT_UNSAVED_CLASS = 'list__dot--unsaved';
const DOT_SAVE_CLASS = 'list__dot--save';
const SPINNER_ID = 'list__spinner';
const SPINNER_ON_CLASS = 'list__spinner--on';
const DOT_ERROR_CLASS = 'list__dot--error';
const COL_DELIMITER = '|'; // separates the columns of a row
// dot states, in the order they take precedence
const DOT_HIDDEN = 0;  // nothing has been tried yet
const DOT_SAVED = 1;   // green -- the server holds what is on screen
const DOT_UNSAVED = 2; // yellow -- edits the server has not been given
const DOT_ERROR = 3;   // red -- a write failed
// Stored names are printable ASCII with the column separator "|" taken out;
// "/" and "\" are kept. An item may hold more than one line, and the file is
// one row per item, so a newline is stored as "~" -- which is why "~" is
// taken out of the text on the way in. List names stay on one line.
const MAX_NAME_LENGTH = 64;
const MAX_ITEM_LENGTH = 1000;
const NEWLINE_MARK = '~';
const COMBINING_MARKS = /[\u0300-\u036f]/g;
const NON_ASCII = /[^\x20-\x7E]/g;
const NON_ASCII_LINES = /[^\x20-\x7E\n]/g; // the same, with newlines kept
const RESERVED_CHARS = /[\0\r\n|~]/g;
const RESERVED_CHARS_LINES = /[\0|~]/g;    // the same, with newlines kept
const EXTRA_SPACES = /\s{2,}/g;
const EXTRA_BLANKS = /[^\S\n]{2,}/g;       // runs of spaces, newlines spared
// window views
const VIEW_LISTS = 0;
const VIEW_ITEMS = 1;
// list modes
const MODE_OPEN = 0;
const MODE_SELECT = 1;
// What stands in the name column of an encrypted row. No real name comes out
// of cleanName() looking like this.
const ENC_MARK = '!ENC';
// password prompt modes
const PWD_SETUP = 0;  // no password yet -- pick one, twice
const PWD_UNLOCK = 1; // one is set -- enter it
const PWD_CHANGE = 2; // the old one and the new one, twice
// what the prompt reads as in each of them, and how many boxes it puts up
const PWD_FORMS = {
  [PWD_SETUP]: { heading: "Set a lists password", action: "Set", boxes: 2,
    first: "New password", second: "Repeat password", fill: "new-password" },
  [PWD_UNLOCK]: { heading: "Enter lists password", action: "Unlock", boxes: 1,
    first: "Password", second: "Repeat password", fill: "current-password" },
  [PWD_CHANGE]: { heading: "Change lists password", action: "Change", boxes: 3,
    first: "Current password", second: "New password", fill: "current-password" }
};

//// shared helpers ////

// cuts a name to length, marking that it was cut
function boundedText(name, limit) {
  if (name.length >= limit) {
    return `${name.substring(0, limit)}...`;
  }
  return name;
}

// The one cleaner both names go through. A list name is printable ASCII on a
// single line; an item may run to several, so line breaks survive (normalised
// to "\n"), runs of spaces are closed up, and the length allowed is greater.
// cleanName(" Caf\u00e9 | x ") -> "Cafe x"
function cleanText(value, multi) {
  let raw = '';
  if (value !== null && value !== undefined) {
    raw = String(value);
  }
  let limit = MAX_NAME_LENGTH;
  let outside = NON_ASCII;      // characters the file cannot carry
  let reserved = RESERVED_CHARS; // characters it uses for itself
  let runs = EXTRA_SPACES;       // whitespace to close up
  if (multi === true) {
    limit = MAX_ITEM_LENGTH;
    outside = NON_ASCII_LINES;
    reserved = RESERVED_CHARS_LINES;
    runs = EXTRA_BLANKS;
    // CRLF and a lone CR both become the one newline the rest of this expects
    raw = raw.replace(/\r\n|\r/g, '\n');
  } else {
    raw = cleanString(raw) || '';
  }
  const name = raw
    .normalize("NFD")             // split accents off their base letter
    .replace(COMBINING_MARKS, '') // "Cafe" keeps the "e", loses the accent
    .replace(outside, '')
    .replace(reserved, '')
    .replace(runs, ' ')           // close up any gaps left behind
    .trim();
  return boundedText(name, limit);
}

export function cleanName(value) {
  return cleanText(value, false);
}

export function cleanItemName(value) {
  return cleanText(value, true);
}

// item text <-> the one row it is stored on: the newlines it carries are the
// only thing that cannot go in the file as itself
function encodeLines(value) {
  return String(value).split('\n').join(NEWLINE_MARK);
}

function decodeLines(value) {
  return String(value).split(NEWLINE_MARK).join('\n');
}

// the next free id in a list of entries that carry one
function nextId(entries) {
  return entries.reduce((highest, entry) => Math.max(highest, entry.id), -1) + 1;
}

// document.getElementById, at the length it is used at
function byId(id) {
  return document.getElementById(id);
}

// Shows or hides an element, by id or by the element itself. Hidden is always
// display:none; shown takes whatever display mode the caller gives.
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

// every message this window raises carries its name
function warn(text) {
  showPopup(`[${WIN_TITLE}] ${text}`);
}

/*
 *  ListStore - list data, file transfer and the edit lock
 */

export class ListStore {

  // onChange() is called when the store changes something the window has to
  // re-draw
  constructor(onChange) {
    this.onChange = function () {};
    if (typeof onChange === 'function') {
      this.onChange = onChange;
    }
    this.data = [];         // [{ id, name, items: [{ id, name, checked }] }]
    this.readOnly = false;  // another user holds the lock, or nothing loaded
    this.online = null;     // last server exchange: true / false / not tried
    this.dirty = false;     // edits the server has not been given
    this.saving = null;     // in-flight save, so writes cannot overlap
    this.sentBody = null;   // the list text the server is known to hold
    this.wireBody = null;   // the list text on the wire right now, if any
    this.loaded = false;    // a parsed list is standing in this.data
    this.lastGoodBody = ""; // last body known to be safe to write back
    this.releasing = null;  // release in flight, so two cannot race
    this.sealing = null;    // re-encrypt in flight, awaited before a write
    // the password for the encrypted lists, and the key while it is entered.
    // Starts locked, every time.
    this.crypt = new FileCrypt({ file: PWD_FILE });
    // The lock's own traffic -- the claim, the confirm and the re-stamp every
    // LOCK_BEAT_MS -- stays out of this.online. The dot reports the list, and
    // a re-stamp is not the list: one that failed turned the dot red at the
    // next edit with the list untouched, and one that went through turned a
    // failed save back to yellow.
    this.lock = new FileLock({
      file: LOCK_FILE,
      onLost: () => this.lockLost()
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
    this.online = await writeFile(FILE_NAME, text);
    return this.online;
  }

  //// file text ////

  // File text -> rows of columns, blank lines dropped. Characters outside
  // printable ASCII are damage from transit and are taken out first, so the
  // rest of the row can still be read.
  rows(text) {
    if (typeof text !== 'string' || text === "") {
      return [];
    }
    return splitMenuData(text)
      .map(row => cleanString(row))
      .filter(row => row)
      .map(row => row.replace(NON_ASCII, ''))
      .filter(row => row)
      .map(row => row.split(COL_DELIMITER));
  }

  // An encrypted list is one row: the name column says so, the blob holds
  // everything else. It is kept whether or not it can be opened, so a locked
  // window writes it back exactly as it arrived, damaged or not.
  sealedEntry(listId, cols) {
    const blob = (cols[3] || '').trim();
    if (blob === "") {
      return null;
    }
    return {
      id: listId,
      name: "",
      items: [],
      enc: true,     // this list is held encrypted
      open: false,   // its blob has been decrypted into name / items
      stale: false,  // it has been edited since the blob was made
      bad: !isCryptBlob(blob),
      blob: blob
    };
  }

  // 0=ListID, 1=ListName, 2=ItemID, 3=ItemName, 4=CheckState
  // rows arrive in order, and the map keeps them that way
  parse(text) {
    const found = new Map();
    this.rows(text).forEach(cols => {
      if (cols.length < 2 || !isNumeric(cols[0])) {
        return; // not a list row (the old lock row lands here)
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
      // empty item fields mark a list that has no items yet
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
      // a row added but never named is a placeholder, and is not saved
      if (entry.pending === true) {
        return;
      }
      if (entry.enc === true) {
        // one row, and the blob is all of it; sealOpen() has refreshed it. A
        // blob that went missing takes its row rather than write half a list.
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
        // the item keeps its line breaks in memory; the file gets them as "~"
        _file += `${head}|${item.id}|${encodeLines(item.name)}|${checked}\n`;
      });
    });
    return _file;
  }

  // the items of a list worth writing: not the placeholder row, if it has one
  savedItems(entry) {
    return entry.items.filter(item => item.pending !== true);
  }

  // What to write for the list rows. this.data is only worth writing once a
  // list has actually loaded; a session that never loaded would write an
  // empty array over the file, so the last known good body goes back instead.
  safeBody() {
    if (this.loaded === true) {
      return this.buildBody();
    }
    return this.lastGoodBody;
  }

  //// encrypted lists ////
  //
  // While it is open, an encrypted list is the same entry as any other: the
  // window renames and re-orders it without knowing. The difference is only
  // in how it reaches the file.

  unlocked() {
    return this.crypt.isUnlocked();
  }

  // an entry the window may draw: everything but the unopened encrypted lists
  isHidden(entry) {
    return entry.enc === true && entry.open !== true;
  }

  // the encrypted lists, in whatever state they are in
  sealed() {
    return this.data.filter(entry => entry.enc === true);
  }

  // the whole list, as the one string that goes inside the blob
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

  // and back again; anything not the shape it went in as is refused
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

  // Opens every sealed list the key fits. One that will not open is left
  // sealed and counted: it belongs to another password, or it is damaged.
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

  // Re-encrypts every open list edited since its blob was made. A fresh IV
  // every time, so the same list never goes to the file as the same bytes.
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
        done = false; // the key went while this was running
        continue;
      }
      entry.blob = blob;
      entry.stale = false;
    }
    return done;
  }

  // Seals in the background after an edit, so the blobs are fresh by the time
  // anything writes them -- the unload beacon cannot wait for one.
  sealSoon() {
    return trackPromise(this, 'sealing', this.sealOpen());
  }

  async settleSeal() {
    // a seal that threw leaves the entry stale, and the next one redoes it
    await awaitQuiet(this.sealing);
  }

  // Shuts the encrypted lists: the blobs are brought up to date, then the
  // plaintext and the key go together. The caller saves first.
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

  // Turns a list being added into an encrypted one. Without the key it does
  // nothing, which is what makes a list added on a locked window a plain one.
  // The blob is made by the seal that follows the first edit.
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

  // The file into this.data: read, parsed, and unsealed where the key fits.
  // A read that does not answer leaves this.data alone and marks the session
  // unloaded, which is what stops it being written back.
  async load() {
    const text = await this.read();
    if (text === null) {
      this.loaded = false;
      return false;
    }
    this.data = this.parse(text);
    this.loaded = true;
    // a window re-opened while the key is in hand does not ask again
    if (this.unlocked()) {
      await this.unsealAll();
    }
    return true;
  }

  // Loads the list and takes the edit lock when it is free. onData() runs as
  // soon as the list is parsed; claiming the lock is a second round trip and
  // changes only whether the rows can be edited.
  async open(onData) {
    // a release from a previous session builds its body when it resumes
    await awaitQuiet(this.releasing);
    this.dirty = false;
    this.sentBody = null;
    this.loaded = false; // nothing trustworthy in this.data until the read is in
    // two files, asked for together, which costs the same wait as one
    const [ok, lockText] = await Promise.all([this.load(), this.lock.read()]);
    if (!ok) {
      // nothing loaded, so there is nothing safe to write back
      this.data = [];
      this.readOnly = true;
      return;
    }
    // what the server holds: a save that rebuilds this same text is dropped
    this.sentBody = this.buildBody();
    this.lastGoodBody = this.sentBody;
    if (typeof onData === 'function') {
      onData();
    }
    // the claim is a one row write to the lock file; the list is not touched
    if (await this.lock.claim(lockText)) {
      this.readOnly = false;
      // runs in the background so the window draws without waiting on it
      this.lock.confirm();
    } else {
      this.readOnly = true;
      // the lock was free, or could not be read, and still was not taken: the
      // server did not answer, which the dot and the warning should say
      if (!this.lock.busy(lockText)) {
        this.online = false;
      }
    }
  }

  // Another window won the lock as we both claimed it. The list is re-read
  // rather than kept: the winner is the one allowed to write.
  async lockLost() {
    this.readOnly = true;
    this.dirty = false;
    this.sentBody = null;
    await this.load();
    warn("another user is editing, opened read only.");
    this.onChange();
  }

  canWrite() {
    return this.lock.owned === true && this.readOnly !== true;
  }

  // An edit was made. Nothing is written here; this turns the dot yellow.
  markChanged() {
    this.dirty = true;
    // which list was edited is not knowable from here, so every open one is
    // marked and re-sealed, keeping the blobs ready for a write that cannot wait
    const open = this.sealed().filter(entry => entry.open === true);
    open.forEach(entry => { entry.stale = true; });
    if (open.length > 0) {
      this.sealSoon();
    }
  }

  // Dirty without an edit in memory: a rekey replaced the blobs and they are
  // already current. markChanged() would re-seal them under the old key.
  markBlobsChanged() {
    this.dirty = true;
  }

  // Writes the list out if the server is behind. Only one write is ever on
  // the wire; a second caller waits for it, then writes what is outstanding.
  save() {
    const previous = this.saving;
    return trackPromise(this, 'saving', this.runSave(previous));
  }

  async runSave(previous) {
    // a failed write is already reported through the status dot
    await awaitQuiet(previous);
    if (!this.canWrite()) {
      return true; // read only, or the lock has already gone back
    }
    if (this.dirty !== true) {
      return true; // the server is already holding this list
    }
    // nothing goes out until every encrypted list has been brought up to date
    await this.settleSeal();
    await this.sealOpen();
    const body = this.safeBody();
    if (body === this.sentBody) {
      // the edits since the last write cancelled each other out
      this.dirty = false;
      return true;
    }
    // what is on the wire, so the unload beacon does not send it twice
    this.wireBody = body;
    const sent = await this.write(body);
    this.wireBody = null;
    if (sent === true) {
      this.sentBody = body;
      this.lastGoodBody = body; // the server took it, so it is safe to redo
      this.dirty = false;
    }
    return sent;
  }

  // Last-chance save for a page going away: it cannot be awaited or checked,
  // so it only goes when something is really outstanding. Nothing can be
  // encrypted here, so it sends the blobs sealSoon() has already made.
  saveBeacon() {
    if (!this.canWrite() || this.dirty !== true) {
      return false;
    }
    const body = this.safeBody();
    // the save from the hide is often still on the wire, and sending the same
    // text again would put two writes on one file
    if (body === this.wireBody) {
      return false;
    }
    const handed = sendFileBeacon(FILE_NAME, body);
    if (handed === true) {
      // taken on trust: there is no answer to wait for
      this.sentBody = body;
      this.lastGoodBody = body;
      this.dirty = false;
    }
    return handed;
  }

  // Hands the lock back as the page goes away, or the lock file keeps the
  // token of a window that no longer exists until the stamp expires.
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

  // Takes the lock again after a back/forward cache restore.
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

  // Writes out anything unsaved and hands the lock back. Only one at a time:
  // destroy() fires it without waiting, so a re-opened window can call again.
  release() {
    return singleFlight(this, 'releasing', () => this.runRelease());
  }

  async runRelease() {
    if (this.online === false) {
      // nothing to write to, so the lock is left to expire on its own
      this.lock.abandon();
      return false;
    }
    // anything unsaved goes first, while the lock is still held
    const sent = await this.save();
    // worth freeing even if that write failed: the edits stay in memory
    const freed = await this.lock.release(true);
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

/*
 *  Lists - the window
 */

export default class Lists {

  constructor() {
    this.store = new ListStore(() => this.render());
    this.view = VIEW_LISTS;
    this.mode = MODE_OPEN;
    this.openId = null;    // list being viewed
    this.editing = false;  // inline name box is open
    this.pendingId = null; // row added but not named yet, so not saved
    this.pendingRows = null; // the array that row was added to
    this.query = "";       // what is in the search box, if there is one
    this.pwdMode = null;   // password prompt: setup / unlock / change, or not up
    this.holdTimer = null; // the lock is being held down
    this.held = false;     // it was held long enough, so the press is spent
    this.epoch = 0;        // counts openings, so a late teardown knows its own
    this.busy = false;     // an exchange with the server is being waited on
    this.settling = false; // rows are up, the edit lock has not answered yet
    // rows picked out for deletion, by id, so a re-draw mid-select keeps them
    this.selected = new Set();
    this.dragSort = new DragSort({
      rowClass: ROW_CLASS,
      onDrop: () => this.commitOrder()
    });
    // window level listeners, attached as a set
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
        // a finger that slides off the lock has changed its mind
        this.holdCheck(event);
        this.dragSort.move(event);
      },
      // a press that ends and one the browser takes away mean the same here
      pointerup: endPress,
      pointercancel: endPress
    };
    // Page level listeners, for the life of the instance. They cost nothing
    // while the window is shut -- each checks the lock first -- and they are
    // held on `this` so dispose() can take them off again.
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

  // The page is going into the background. It is reliably still alive here,
  // so the write can be awaited and the dot updated from the answer.
  handleHidden() {
    if (document.visibilityState !== 'hidden' || this.store.dirty !== true) {
      return;
    }
    this.store.save().then(() => this.updateDot());
  }

  // The page is being torn down: an awaited write would be cancelled part
  // way, so the browser is handed the list to finish on its own. On iOS
  // Safari this often does not fire, and the lock expiring covers that.
  handleUnload() {
    // the list first, while the lock is still held
    this.store.saveBeacon();
    // then the lock: leaving it held opens the next window read only
    this.store.releaseBeacon();
  }

  // A back/forward cache restore: the window is still on screen, but the lock
  // went back on the way out. Ask for it again.
  async handleRestore(event) {
    if (event.persisted !== true) {
      return; // an ordinary load, which starts from nothing anyway
    }
    if (!this.isOpen()) {
      return;
    }
    await this.store.reclaim();
    this.render();
  }

  //// window lifecycle ////

  isOpen() {
    return byId(WIN_ID) !== null;
  }

  // opens the window, or closes it again if it is already on-screen
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
      this.store.release(); // closed while loading, hand the lock straight back
      return;
    }
    if (this.store.online === false) {
      warn("server connection not available.");
    }
    this.render();
  }

  // Writes the list out with the ring turning by the status dot, and leaves
  // the dot on the answer. Every path that saves on purpose comes here; the
  // ones that save on the way out of the page cannot wait for an answer.
  async saveOut() {
    const sent = await this.runBusy(() => this.store.save());
    this.updateDot();
    return sent;
  }

  // Close button. Anything unsaved goes out now; only a refused write stops
  // the window closing.
  async closeWindow() {
    if (this.store.dirty === true && !await this.saveOut()) {
      this.showOverlay(true);
      return;
    }
    this.destroy();
  }

  destroy() {
    this.busy = false;
    this.settling = false;
    this.holdCancel();
    this.held = false;
    this.dragSort.cancel();
    // the release writes out anything unsaved and that write needs the key,
    // so the key is dropped after it rather than beside it
    const releasing = this.store.release();
    // a window re-opened while that release settles would otherwise have the
    // key taken back off it by the chain below; the count says whose it is
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

  // The state a session starts on. Used at both ends -- opening and tearing
  // down -- so the two cannot drift apart.
  resetView() {
    this.view = VIEW_LISTS;
    this.mode = MODE_OPEN;
    this.openId = null;
    this.query = "";
    this.selected.clear();
  }

  // End of the instance, as opposed to the window: destroy() keeps the page
  // listeners, since a shut window still hands the lock back if the page is
  // hidden while a release is settling.
  dispose() {
    document.removeEventListener('visibilitychange', this.pageListeners.hidden);
    window.removeEventListener('pagehide', this.pageListeners.unload);
    window.removeEventListener('unload', this.pageListeners.unload);
    window.removeEventListener('pageshow', this.pageListeners.restore);
    this.destroy();
  }

  // Runs a server exchange with the ring turning by the status dot. The ring
  // stops whether it worked or threw; a caller that has stopped it itself
  // (showRows) is not re-drawn over.
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

  // Only the ring and the buttons change with a busy state, so only those are
  // re-drawn -- a full render() blinks the rows on every save.
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

  drawWindow() {
    if (this.isOpen()) {
      return;
    }
    // the back button starts hidden: the window opens on the lists view and
    // the first render() is a round trip away
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
    // Enter answers the prompt and Escape drops it. The boxes live for the
    // life of the window, so this is attached once.
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

  // un-checked items float to the top; both groups keep their saved order
  visibleRows() {
    const entries = this.activeList();
    if (this.view !== VIEW_ITEMS) {
      // a locked list is not drawn at all -- no row, no name, no count
      return entries.filter(entry => !this.store.isHidden(entry));
    }
    return entries.filter(item => item.checked !== true)
      .concat(entries.filter(item => item.checked === true));
  }

  // items still un-checked, shown on the right of a list row
  openCount(entry) {
    return entry.items.filter(item => item.checked !== true).length;
  }

  // Records an edit and re-draws. Nothing goes to the server here -- the dot
  // turns yellow and the list is written on close, hide or unload.
  commit() {
    this.store.markChanged();
    this.render();
  }

  //// drawing ////

  listElem() {
    return byId(LIST_ID);
  }

  rowById(id) {
    const list = this.listElem();
    if (!list) {
      return null;
    }
    return list.querySelector(`.${ROW_CLASS}[data-id="${id}"]`);
  }

  rowElements() {
    const list = this.listElem();
    if (!list) {
      return [];
    }
    return [...list.getElementsByClassName(ROW_CLASS)];
  }

  render() {
    const list = this.listElem();
    if (!list) {
      return;
    }
    // a re-draw can arrive mid-drag (lockLost() reaches here through the
    // store's onChange) and would replace the rows the drag still holds
    this.dragSort.cancel();
    // a re-draw rebuilds the search box, so where the focus is now is noted
    // and put back at the end, rather than the caret going with the old box
    const oldBox = byId(SEARCH_BOX_ID);
    const searching = oldBox !== null && document.activeElement === oldBox;
    const items = this.view === VIEW_ITEMS;
    this.renderHeader(items);
    list.innerHTML = "";
    if (this.busy === true && this.store.loaded !== true) {
      // waiting on the first read: no rows to show yet, and the ring says so.
      // Once the list has loaded, a busy state draws as normal.
      this.renderActions();
      this.updateDot();
      return;
    }
    // the search box, on a list long enough to be worth one, above the rows
    if (this.searchable()) {
      list.appendChild(this.searchRow());
    } else {
      this.query = ""; // no box on screen, so nothing is being searched for
    }
    this.renderRows(list, items);
    // the rows are new, so whatever is in the box has to be put over them
    this.applyFilter();
    this.clampScroll(list);
    this.renderActions();
    this.updateDot();
    if (searching) {
      this.refocusSearch();
    }
  }

  // title, and the two buttons that share the left corner of the header
  renderHeader(items) {
    const entry = this.openList();
    let title = WIN_TITLE;
    if (items && entry) {
      title = entry.name;
    }
    byId(TITLE_ID).innerText = title;
    // The back button belongs to the items view and the lock to the lists
    // view. They share the slot, so the header and its centred title do not
    // move between the two.
    const backBtn = byId(BACK_BTN_ID);
    let backVisibility = "hidden";
    if (items) {
      backVisibility = "visible";
    }
    backBtn.style.visibility = backVisibility;
    setShown(backBtn, items === true);
    setShown(LOCK_BTN_ID, items !== true);
    this.updateLockIcon();
  }

  // the rows themselves, the no-match placeholder and the total beneath them
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
    // which rows drag is worked out once for the view, not once per row
    const movable = this.movableRule();
    rows.forEach(entry => list.appendChild(this.createRow(entry, movable(entry))));
    // stands in for the rows while a search matches none of them; built with
    // them and shown by applyFilter()
    if (this.searchable()) {
      const none = makeElement('div', "list__empty");
      none.id = NO_MATCH_ID;
      none.innerText = "No matches";
      setShown(none, false);
      list.appendChild(none);
    }
    // the items in the open list, or every item across all the lists
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
    // focus() on its own selects the whole value; the caret belongs at the end
    const end = box.value.length;
    box.setSelectionRange(end, end);
  }

  createRow(entry, movable) {
    const items = this.view === VIEW_ITEMS;
    const select = this.mode === MODE_SELECT;
    // list rows are taller; item rows stay tight so more of a list fits
    let rowClass = ROW_CLASS;
    if (!items) {
      rowClass += " list__row--tall";
    }
    const row = makeElement('div', rowClass);
    row.dataset.id = entry.id;
    // what the search box matches against, folded once here rather than on
    // every keystroke
    row.dataset.find = String(entry.name).toLowerCase();
    if (select) {
      // The round button is the whole left hand side in select mode. A list
      // row has none of its own; an item row keeps its grip but hidden, so
      // the rounds stay in their column and the rows do not shift sideways.
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
    // The badge on an encrypted list row. The stylesheet takes it off again
    // once the row has a name label beside it, so what shows is the row being
    // added. There is none on an item row: a list is encrypted whole.
    if (!items && entry.enc === true) {
      row.appendChild(this.rowLockElement());
    }
    // a multi-line item wraps; a single line row keeps its ellipsis
    let labelClass = LABEL_CLASS;
    if (String(entry.name).indexOf('\n') !== -1) {
      labelClass += " list__label--multi";
    }
    // the name on a placeholder row is not a name, it is what to press
    if (entry.pending === true) {
      labelClass += ` ${LABEL_NEW_CLASS}`;
    }
    const label = makeElement('span', labelClass);
    label.innerText = entry.name;
    row.appendChild(label);
    // the label is the rename target, so the filler carries the open/tick tap
    row.appendChild(makeElement('span', "list__fill"));
    if (!items) {
      // how many items in that list are still un-checked
      const count = makeElement('span', "list__count");
      let unchecked = this.openCount(entry);
      if (unchecked == 0) unchecked = '';
      count.innerText = unchecked;
      row.appendChild(count);
      // the arrow only says "this opens", which delete mode is not doing
      if (!select) {
        row.appendChild(makeElement('i', "fad fa-chevron-right list__arrow"));
      }
    }
    return row;
  }

  // The completed toggle on an item row, and the delete marker on any row in
  // select mode -- the same button in red, so the two never look alike.
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

  // The lock badge on an encrypted list row. It only reports: whether a list
  // is encrypted is settled when it is added.
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

  renderActions() {
    // Read only takes everything that would write the file off screen. While
    // the server is being waited on they go too, but without the notice.
    const settling = this.settling === true;
    const readOnly = !settling && this.store.readOnly === true;
    // a save leaves the buttons alone; only an unloaded or unsettled list
    // stands them down
    const loading = this.busy === true && this.store.loaded !== true;
    const waiting = readOnly || settling || loading;
    setShown(STATUS_ID, readOnly, "flex");
    [ADD_BTN_ID, DEL_BTN_ID].forEach(id => setShown(id, !waiting));
    if (waiting) {
      return;
    }
    // the delete button becomes an X once something is picked out
    const select = this.mode === MODE_SELECT;
    const confirm = this.selected.size > 0;
    const icon = byId(DEL_ICON_ID);
    icon.classList.toggle("fa-times", confirm);
    icon.classList.toggle("fa-trash-alt", !confirm);
    icon.classList.toggle(DEL_ICON_ON_CLASS, select);
  }

  // green once the server has the edits, yellow while it has not been given
  // them, red after a failed write
  dotState() {
    if (this.store.online === null) {
      return DOT_HIDDEN; // nothing has been sent yet
    }
    if (this.store.online === false) {
      return DOT_ERROR;
    }
    if (this.store.dirty === true) {
      return DOT_UNSAVED;
    }
    return DOT_SAVED;
  }

  updateDot() {
    // the ring turns whenever the server is being waited on
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
    // pressing it writes the list out, so it takes presses -- and shows the
    // hand -- only while there is something to write
    const canSave = this.store.dirty === true && this.store.canWrite();
    dot.classList.toggle(DOT_SAVE_CLASS, canSave);
    if (canSave) {
      dot.title = "Save changes";
    } else {
      dot.removeAttribute('title');
    }
  }

  // the dot pressed: write now rather than on close, hide or unload
  async saveNow() {
    if (this.store.dirty !== true || !this.store.canWrite()) {
      return;
    }
    await this.saveOut();
  }

  showOverlay(show) {
    setShown(OVERLAY_ID, show === true, "flex");
  }

  //// searching ////

  // Only the items view, and only once a list is too long to scan by eye.
  searchable() {
    if (this.view !== VIEW_ITEMS) {
      return false;
    }
    const entry = this.openList();
    return entry !== null && entry.items.length > SEARCH_MIN_ITEMS;
  }

  // what is being searched for, ready to match against
  searchText() {
    return searchTerm(this.query);
  }

  // rows are being held back, as opposed to a box merely being on screen
  filtering() {
    return this.searchable() && this.searchText() !== "";
  }

  // The top row of a long list, the same box the bookmarks menu carries.
  // applyFilter() hides rows in place, so the box keeps its focus and caret
  // through every keystroke.
  searchRow() {
    return searchRowElement({
      rowClass: "list__search",
      boxId: SEARCH_BOX_ID,
      maxLength: MAX_NAME_LENGTH,
      // a re-draw brings the box back with what was typed into it
      value: this.query,
      onSearch: (value) => {
        this.query = value;
        this.applyFilter();
      }
    });
  }

  // Hides the rows that do not match rather than removing them, so the ids on
  // the rows, the selection and the saved order are untouched by a search.
  // Runs on every keystroke, so it does no more than walk the rows.
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
    // the grips go too: what is on screen is not the whole list to re-order
    list.classList.toggle(LIST_FIND_CLASS, query !== "");
    setShown(NO_MATCH_ID, query !== "" && shown === 0, "block");
    // the count at the bottom follows what is actually on screen
    const total = list.querySelector('.list__total');
    if (total) {
      if (query === "") {
        total.innerText = this.totalText(rows.length, 'item');
      } else {
        total.innerText = `${shown} of ${rows.length} items`;
      }
    }
    // the rows a search takes away leave the panel scrolled past its own end
    this.clampScroll(list);
  }

  //// actions ////

  // Changing mode drops whatever was picked out. redraw is off for the
  // callers that are about to draw anyway.
  setMode(mode, redraw = true) {
    this.mode = mode;
    this.selected.clear();
    if (redraw === true) {
      this.render();
    }
  }

  // Into a list, or back out of it. Both ways start with an empty search box,
  // nothing picked out, and the panel back at the top.
  showView(view, id = null) {
    this.query = "";
    this.openId = id;
    this.view = view;
    this.setMode(MODE_OPEN);
    this.resetScroll();
  }

  // A view change swaps the rows for a usually shorter set, and the panel's
  // scrollTop outlives them, so every view starts at the top.
  //
  // On iOS that needs more than writing scrollTop: the panel scrolls on its
  // own thread, and a fling overrides the write on its next update. Taking
  // the overflow away for a moment ends the fling and drops the scroller back
  // to the main thread, so the reset holds.
  resetScroll() {
    const list = this.listElem();
    if (!list) {
      return;
    }
    const overflow = list.style.overflowY;
    list.style.overflowY = "hidden";
    list.scrollTop = 0;
    // reading a layout value applies the change now, which is what ends the fling
    void list.offsetHeight;
    list.style.overflowY = overflow;
    list.scrollTop = 0;
  }

  // Rows can leave without the view changing -- a delete, or items floating
  // up as they are ticked -- which leaves the panel past its own end.
  clampScroll(list) {
    let furthest = list.scrollHeight - list.clientHeight;
    if (furthest < 0) {
      // the rows no longer fill the panel, so there is nothing to scroll
      furthest = 0;
    }
    if (list.scrollTop > furthest) {
      list.scrollTop = furthest;
    }
  }

  // The add button. What it puts on screen is a placeholder, not a list: it
  // is not written, the dot does not move, and it is gone again unless it is
  // pressed and named.
  addEntry() {
    // the placeholder name rarely matches what is in the search box, so the
    // search goes first rather than the row arriving out of view
    this.query = "";
    this.setMode(MODE_OPEN, false);
    this.dropPending(); // only ever one waiting to be named
    const entries = this.activeList();
    let entry = { id: nextId(entries), name: "New list", items: [], pending: true };
    if (this.view === VIEW_ITEMS) {
      entry = { id: nextId(entries), name: "New item", checked: false, pending: true };
    } else {
      // added while the encrypted lists are open, so it is one of them; shut,
      // there is no key and setSealed() does nothing
      this.store.setSealed(entry);
    }
    // new rows lead: first in the saved order, so first on screen as well --
    // un-checked items are drawn ahead of checked ones, and a hidden
    // encrypted list has no row to sit above it
    entries.unshift(entry);
    // the row that is pressed to name it, and the array it sits in, since the
    // view can change under it
    this.pendingId = entry.id;
    this.pendingRows = entries;
    this.render();
    // the first row, so the top of the panel is where it is
    this.resetScroll();
  }

  // Takes the placeholder row away. Nothing about it was saved, so this is a
  // change to the screen and not to the list: no commit, and the caller
  // re-draws. Spliced in place, so a list row and an item row are alike.
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

  // First press turns the check boxes on, the second deletes what is picked
  // out or leaves the mode again.
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

  // Marks a row for deletion, or unmarks it. Turned over in place, so a long
  // list does not rebuild for every row picked.
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
    // a placeholder opens on an empty box: the words it was showing are a
    // prompt, not a name to edit or select over
    const pending = entry.pending === true;
    let value = entry.name;
    if (pending) {
      value = "";
    }
    const namebox = this.makeNamebox(multi, value);
    this.sizeNamebox(namebox, row, label);
    label.replaceWith(namebox);
    if (multi) {
      // the box opens at the height the text it already holds needs
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
          // named at last, so this -- not the button that put it on screen --
          // is the edit that saves it
          delete entry.pending;
          this.pendingId = null;
          this.pendingRows = null;
        }
      } else if (pending) {
        // left un-named, and never written: it goes with nothing to undo
        this.dropPending();
        this.render();
        return;
      } else {
        this.render(); // nothing changed, no write needed
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
        // Enter opens the next line of an item; tapping away still commits
        return;
      }
      key_event.preventDefault();
      finish(true);
    });
  }

  // Sets the item box to the height its text needs, so a new line pushes the
  // row down by one. Height is cleared first, or a box that has grown never
  // measures smaller again; border-box means the border has to be added back
  // on top of scrollHeight.
  growNamebox(namebox) {
    namebox.style.height = 'auto';
    const border = namebox.offsetHeight - namebox.clientHeight;
    namebox.style.height = `${namebox.scrollHeight + border}px`;
  }

  // The box takes the space the name and the filler already occupy, so the
  // row keeps its size -- rows are max-content, so a box that sized itself
  // would drag the window wider. The filler steps aside for the duration.
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
    const sent = await this.saveOut();
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

  //// encrypted lists ////

  // The three checks the password paths start with. They are separate because
  // the order differs: the lock asks whether a password exists before whether
  // this window may write one, and a change asks the other way round.
  cryptoOk() {
    if (cryptoReady()) {
      return true;
    }
    // WebCrypto is only handed out on a secure origin
    warn("encryption needs an https connection.");
    return false;
  }

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
      warn("server connection not available.");
    }
    return state;
  }

  // The lock in the corner. Unlocked, it locks; locked, it asks for the
  // password, or for a new one when the record file says none has been set.
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

  // Shuts the encrypted lists. Anything outstanding goes to the server first:
  // the key is about to go, and with it the only way to encrypt what is in
  // memory. A write that fails leaves everything open rather than lose it.
  async lockLists() {
    if (this.store.dirty === true && !await this.saveOut()) {
      warn("changes not saved, still unlocked.");
      return false;
    }
    await this.store.lockUp();
    // the list being viewed may be one that has just gone
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

  // the three boxes, in the order they are read down
  pwdBoxes() {
    return PWD_BOX_IDS.map(id => byId(id));
  }

  // The same three boxes every time; PWD_FORMS holds what they say and how
  // many are shown. The first box is always the password the user already
  // knows, so a change reads down them in order: old, new, new again.
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

  // A prompt answered wrongly: the reason goes up, the boxes named are
  // emptied, and the first of them takes the focus back.
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

  // emptied on the way out: one less place a password is left sitting
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
    // the derivation is deliberately slow, so the ring turns while it runs
    let out = null;
    if (setup) {
      out = await this.runBusy(() => this.store.crypt.setup(password));
    } else {
      out = await this.runBusy(() => this.store.crypt.unlock(password));
    }
    if (!this.isOpen()) {
      return; // the window went while the key was being worked out
    }
    if (out.ok !== true) {
      this.showPasswordError(out.error, PWD_BOX_ID, PWD_BOX2_ID);
      return;
    }
    this.closePassword();
    // whatever was already encrypted opens now
    const found = await this.runBusy(() => this.store.unsealAll());
    if (found.failed > 0) {
      warn(`${found.failed} encrypted list(s) would not open.`);
    }
    this.render();
  }

  //// changing the password ////
  //
  // Held down rather than tapped: the change re-encrypts every list under the
  // new key and writes them out, so it is not something to reach by accident.

  // The press has begun on the lock. It counts whether the lists are open or
  // shut: the hold spends the press either way.
  holdStart(event) {
    this.holdCancel();
    // a hold that opened the prompt leaves its click to the overlay, so the
    // flag that swallows that click is cleared by a fresh press
    this.held = false;
    if (!event.target.closest(`#${LOCK_BTN_ID}`)) {
      return;
    }
    this.holdIcon(true);
    this.holdTimer = setTimeout(() => {
      this.holdTimer = null;
      this.held = true; // the click that follows is spent on this
      this.holdIcon(false);
      this.changePress();
    }, LOCK_HOLD_MS);
  }

  holdCheck(event) {
    if (this.holdTimer === null) {
      return;
    }
    if (!event.target.closest(`#${LOCK_BTN_ID}`)) {
      this.holdCancel();
    }
  }

  holdCancel() {
    if (this.holdTimer !== null) {
      clearTimeout(this.holdTimer);
      this.holdTimer = null;
    }
    this.holdIcon(false);
  }

  // the icon warms up over the hold, so it is visibly counting
  holdIcon(on) {
    const icon = byId(LOCK_ICON_ID);
    if (icon) {
      icon.classList.toggle(LOCK_HOLD_CLASS, on === true);
    }
  }

  // the lock has been held for its full time
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

  async passwordChange(oldPassword, newPassword, repeated) {
    // change() unlocks with the old password to do its work, so this is the
    // last point at which whether the lists were open is knowable
    const wasOpen = this.store.unlocked();
    if (newPassword !== repeated) {
      this.showPasswordError("new passwords do not match", PWD_BOX3_ID);
      return;
    }
    const out = await this.runBusy(() => this.store.crypt.change(
      oldPassword, newPassword, (nextKey) => this.rekeyLists(nextKey)));
    if (!this.isOpen()) {
      return; // the window went while the keys were being worked out
    }
    if (out.ok !== true) {
      this.showPasswordError(out.error, PWD_BOX_ID, PWD_BOX2_ID, PWD_BOX3_ID);
      return;
    }
    this.closePassword();
    // the change leaves the new key in hand: lists already open stay open
    // under it, and with nothing open the key goes straight back out
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

  // Handed the new key by change(): every encrypted list is opened with the
  // old key, sealed under the new one, and the whole file written. Nothing is
  // swapped in until all of them are out, and a write that does not land puts
  // the old blobs back, so the file and the password record cannot disagree.
  async rekeyLists(nextKey) {
    // an open list carries plaintext its blob may not have caught up with, so
    // the blobs are brought up to date first, under the old key. A background
    // seal is let finish before that, so the two do not write the same entry.
    await this.store.settleSeal();
    if (await this.store.sealOpen() !== true) {
      return false; // a list that would not seal, so nothing is changed
    }
    const swapped = [];
    for (const entry of this.store.sealed()) {
      const context = cryptContext(FILE_NAME, entry.id);
      const text = await this.store.crypt.decrypt(entry.blob, context);
      if (text === null) {
        return false; // one that will not open, so nothing is changed
      }
      const blob = await encryptText(nextKey, text, context);
      if (blob === null) {
        return false;
      }
      swapped.push({ entry: entry, next: blob, held: entry.blob });
    }
    if (swapped.length === 0) {
      return true; // a password with nothing encrypted under it yet
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

  handleClick(event) {
    if (event.target.closest(`#${DOT_ID}`)) {
      this.saveNow();
      return;
    }
    // The placeholder goes as soon as anything else is pressed. Which row was
    // pressed is read before that: the re-draw replaces the element the press
    // arrived on.
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
    // a press anywhere on the placeholder opens it for its name
    if (id === this.pendingId) {
      if (!this.rowsLocked()) {
        this.beginRename(id);
      }
      return;
    }
    if (this.mode === MODE_SELECT) {
      // tapping anywhere on the row, the round button included, selects it
      this.toggleSelected(id, row);
      return;
    }
    // tapping the name (or the few pixels of padding around it) renames
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

  handleButton(id) {
    switch (id) {
      // the unsaved changes dialog owns the window while it is up
      case RETRY_BTN_ID:
        this.retrySave();
        return;
      case DISCARD_BTN_ID:
        this.discardChanges();
        return;
      // so does the password prompt
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
      // unlocking only reads, so it is offered on a read only window too
      case LOCK_BTN_ID:
        if (this.held === true) {
          // the hold answered this press; the click it leaves would ask twice
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
  //
  // DragSort moves the rows on screen, stepping a row only over neighbours
  // whose grips are on; commitOrder() reads the order back off the screen
  // when the row is let go. Which grips are on is movableRule().

  // Whether a press may start a drag at all: not while a name box is open,
  // while rows are being picked out, or while a search is holding rows back.
  dragAllowed() {
    return !this.editing && this.mode !== MODE_SELECT && !this.filtering();
  }

  // The test for which rows in the current view can be dragged, as a
  // function, so a pass over the rows looks at the whole list only once.
  //   - a placeholder never: it holds no place in an order yet
  //   - lists view: every list that has a row
  //   - items view: the checked items, or every item while none is checked
  movableRule() {
    if (this.view !== VIEW_ITEMS) {
      return entry => entry.pending !== true && !this.store.isHidden(entry);
    }
    const noneChecked = this.activeList().every(item => item.checked !== true);
    return entry => entry.pending !== true && (noneChecked || entry.checked === true);
  }

  // Movable rows can only have traded places among the slots movable rows
  // already held, so each of those slots takes the movable row now drawn in
  // its turn. Everything else keeps the slot it holds.
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
    // the screen and the list no longer agree on what moves: leave both
    if (drawn.length !== slots.length) {
      return;
    }
    drawn.forEach((entry, at) => { entries[slots[at]] = entry; });
    this.commit();
  }
}
