// Automate Website - lists window
//
// Two classes: ListStore holds the list data, the file and the edit lock;
// Lists is the window itself (default export, used by main.js).
//
// A centred window holding a set of lists; selecting one swaps the window
// over to the items inside it. Both levels support add, rename, multi-select
// delete and drag re-ordering. Items add a round completed button: un-checked
// items float to the top, checked items sit below in their saved order, and
// only checked items can be dragged. Delete mode turns that same round button
// red rather than adding a second set of boxes. A list holding more than
// SEARCH_MIN_ITEMS items carries a search box that hides non-matching rows in
// place, without re-drawing or re-ordering anything.
//
// Saved file (lists.save), one line per item:
//   LIST_ID|LIST_NAME|ITEM_ID|ITEM_NAME|CHECK_STATE
// Row order is the order of the lists and their items, so nothing carries a
// position number. A list with no items keeps its line with the item fields
// empty. An older leading LOCK| row parses as "not a list row" and is dropped
// the first time the list is saved.
//
// The edit lock lives in lists-lock.save (FileLock in utils.js): whoever
// opens the window first claims it, anyone opening it while it is held gets a
// read only window.
//
// Nothing is written on a timer. The list is saved on the close button, when
// the page is hidden, and when it is unloaded. The dot in the top right
// corner reports where those edits stand: green once the server has taken
// them, yellow while there are changes it has not been given, red after a
// failed write -- and a failed write leaves the edits in memory, with the
// close offering to retry or discard.
//
// Encrypted lists
//
// A list can be locked. Its name and every item are held on one row as a
// single AES-256-GCM blob:
//   LIST_ID|!ENC|0|E1.<iv>.<ciphertext>|0
// so nothing about it is in the file as itself -- not the name, not the item
// count, not whether anything is ticked off. An older parser reads that row
// as a list with no items rather than as damage.
//
// The key comes from a password through the shared helpers in utils.js, and
// only a hash of the verifier is stored, in lists-pwd.save. While locked, the
// encrypted lists are not drawn, not counted and not opened; they sit in
// memory as the blobs they arrived as and are written back untouched. The key
// never leaves this tab.

import { splitMenuData, cleanString, isNumeric, removeWindow, showPopup,
  dragGripElement, DragSort, FileLock, readFile, writeFile, sendFileBeacon,
  html, FileCrypt, cryptoReady, isCryptBlob, cryptContext, encryptText,
  searchRowElement, searchTerm, searchMatches, makeElement, singleFlight,
  trackPromise, awaitQuiet } from './utils.js';

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
const ROW_LOCK_ON_CLASS = 'list__rowlock--on';
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
// Stored names are printable ASCII with the column separator "|" swapped out;
// "/" and "\" are kept, since nothing downstream treats a name as a path.
//
// An item may hold more than one line. The file is one row per item, so a
// newline cannot be stored as itself: it goes in as "~", which is why "~" is
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
// What stands in the name column of an encrypted row. It cannot be mistaken
// for a name: cleanName() takes "!" through untouched, but every real name
// has been through it too and none of them come out as this.
const ENC_MARK = '!ENC';
// password prompt modes
const PWD_SETUP = 0;  // no password yet -- pick one, twice
const PWD_UNLOCK = 1; // one is set -- enter it
const PWD_CHANGE = 2; // the old one and the new one, twice

//// shared helpers ////

// cuts a name to length, marking that it was cut
function boundedText(name, limit) {
  if (name.length >= limit) {
    return `${name.substring(0, limit)}...`;
  }
  return name;
}

// A list name: printable ASCII, one line, of sane length.
// cleanName(" Caf\u00e9 | x ") -> "Cafe x"
export function cleanName(value) {
  let raw = '';
  if (value !== null && value !== undefined) {
    raw = String(value);
  }
  const text = cleanString(raw);
  const name = (text || '')
    .normalize("NFD")             // split accents off their base letter
    .replace(COMBINING_MARKS, '') // "Cafe" keeps the "e", loses the accent
    .replace(NON_ASCII, '')
    .replace(RESERVED_CHARS, '')
    .replace(EXTRA_SPACES, ' ')   // close up any gaps left behind
    .trim();
  return boundedText(name, MAX_NAME_LENGTH);
}

// The same treatment for an item, which may run to more than one line: line
// breaks are kept (and normalised to "\n"), runs of spaces are closed up
// without touching them, and the text is allowed to be much longer.
export function cleanItemName(value) {
  let raw = '';
  if (value !== null && value !== undefined) {
    raw = String(value);
  }
  const name = raw
    .replace(/\r\n|\r/g, '\n')    // CRLF and lone CR both become one newline
    .normalize("NFD")
    .replace(COMBINING_MARKS, '')
    .replace(NON_ASCII_LINES, '')
    .replace(RESERVED_CHARS_LINES, '')
    .replace(EXTRA_BLANKS, ' ')
    .trim();
  return boundedText(name, MAX_ITEM_LENGTH);
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
    this.lock = new FileLock({
      file: LOCK_FILE,
      onExchange: (ok) => { this.online = ok; },
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

  // hands a file to the browser to send on its own (utils.js)
  writeBeacon(file, text) {
    return sendFileBeacon(file, text);
  }

  //// file text ////

  // File text -> rows of columns, blank lines dropped. Anything outside
  // printable ASCII is taken out of a row before it is split: nothing this
  // file holds is written with such a character, so a row carrying one has
  // been damaged in transit, and dropping the damage is what lets the rest of
  // the row still be read.
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

  // An encrypted list is one row and stays sealed: the name column says so,
  // and everything the list holds is in the blob on it. It is kept whether or
  // not it can be opened, so a locked window writes it back exactly as it
  // arrived -- including one that arrived damaged, since a row nobody can
  // read is worth more than a row that quietly disappears.
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
      if (entry.enc === true) {
        // One row, and the blob is all of it. sealOpen() has already
        // refreshed it for anything edited since. A blob that somehow went
        // missing takes its row with it rather than writing a half list.
        if (typeof entry.blob === 'string' && entry.blob !== "") {
          _file += `${entry.id}|${ENC_MARK}|0|${entry.blob}|0\n`;
        }
        return;
      }
      const head = `${entry.id}|${entry.name}`;
      if (entry.items.length === 0) {
        _file += head + "|||0\n";
        return;
      }
      entry.items.forEach(item => {
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

  // What to write for the list rows. this.data is only worth writing while a
  // list that was actually loaded is standing in it: a session that never
  // loaded holds an empty array, and writing that out would erase the file.
  // The last body known to be good goes back instead, which is what the
  // server already has. A loaded list the user has genuinely emptied still
  // writes empty.
  safeBody() {
    if (this.loaded === true) {
      return this.buildBody();
    }
    return this.lastGoodBody;
  }

  //// encrypted lists ////
  //
  // An encrypted list is the same entry as any other while it is open: the
  // window adds items to it, renames it and re-orders it without knowing.
  // The difference is only in how it reaches the file.

  unlocked() {
    return this.crypt.isUnlocked();
  }

  // is there anything encrypted to unlock in the first place
  hasSealed() {
    return this.data.some(entry => entry.enc === true);
  }

  // an entry the window is allowed to draw: everything, less the encrypted
  // lists that have not been opened
  isHidden(entry) {
    return entry.enc === true && entry.open !== true;
  }

  // the whole list, as the one string that goes inside the blob
  sealText(entry) {
    return JSON.stringify({
      n: entry.name,
      i: entry.items.map(item => {
        let checked = 0;
        if (item.checked === true) {
          checked = 1;
        }
        return { i: item.id, n: item.name, c: checked };
      })
    });
  }

  // and back again; anything not the shape it went in as is refused rather
  // than half loaded
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
  // sealed and counted: it belongs to another password, or it has been
  // tampered with, and either way overwriting it would be worse.
  async unsealAll() {
    let opened = 0;
    let failed = 0;
    if (!this.unlocked()) {
      return { opened: opened, failed: failed };
    }
    for (const entry of this.data) {
      if (entry.enc !== true || entry.open === true) {
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
    for (const entry of this.data) {
      if (entry.enc !== true || entry.open !== true || entry.stale !== true) {
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

  // Kicks a seal off in the background after an edit, so the blobs are
  // already fresh by the time anything wants to write them -- including the
  // beacon on the way out of the page, which cannot wait for anything.
  sealSoon() {
    return trackPromise(this, 'sealing', this.sealOpen());
  }

  async settleSeal() {
    // a seal that threw leaves the entry stale, and the next one redoes it
    await awaitQuiet(this.sealing);
  }

  // Shuts the encrypted lists again: the blobs are brought up to date, then
  // the plaintext and the key are dropped together. The caller saves first --
  // this is the point of no return for anything still in memory.
  async lockUp() {
    await this.sealOpen();
    this.data.forEach(entry => {
      if (entry.enc !== true) {
        return;
      }
      entry.name = "";
      entry.items = [];
      entry.open = false;
      entry.stale = false;
    });
    this.crypt.lock();
  }

  // Turns a list into an encrypted one, or back into a plain one. Only ever
  // on a list the user is adding, and only while the key is in hand.
  setSealed(entry, sealed) {
    if (!entry) {
      return false;
    }
    if (sealed === true) {
      if (!this.unlocked()) {
        return false;
      }
      entry.enc = true;
      entry.open = true;
      entry.stale = true;
      entry.blob = null;
    } else {
      delete entry.enc;
      delete entry.open;
      delete entry.stale;
      delete entry.blob;
    }
    return true;
  }

  //// session ////

  // Loads the list and takes the edit lock when it is free. onData() is
  // called as soon as the list is parsed: claiming the lock is a second round
  // trip, and it changes only whether the rows can be edited, not what they
  // say.
  async open(onData) {
    // a release from a previous session can still be settling, and it builds
    // its body when it resumes -- let it finish first
    await awaitQuiet(this.releasing);
    this.dirty = false;
    this.sentBody = null;
    this.loaded = false; // nothing trustworthy in this.data until the read is in
    // two files, asked for together, which costs the same wait as one
    const [text, lockText] = await Promise.all([this.read(), this.lock.read()]);
    if (text === null) {
      // nothing loaded, so there is nothing safe to write back
      this.data = [];
      this.readOnly = true;
      return;
    }
    this.data = this.parse(text);
    this.loaded = true;
    // a window re-opened while the key is still in hand shows what it showed
    // before, rather than asking for the password a second time
    if (this.unlocked()) {
      await this.unsealAll();
    }
    // what the server holds: a save that builds this same text again has
    // nothing to say and is dropped rather than put on the wire
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
    }
  }

  // A competing window won the lock in the moment we both claimed it. The
  // list is re-read rather than kept: the winner is the one allowed to write.
  async lockLost() {
    this.readOnly = true;
    this.dirty = false;
    this.sentBody = null;
    const text = await this.read();
    if (text === null) {
      this.loaded = false;
    } else {
      this.data = this.parse(text);
      this.loaded = true;
      if (this.unlocked()) {
        await this.unsealAll();
      }
    }
    showPopup(`[${WIN_TITLE}] another user is editing, opened read only.`);
    this.onChange();
  }

  canWrite() {
    return this.lock.owned === true && this.readOnly !== true;
  }

  // An edit was made. Nothing is written here: this only records that the
  // server is now behind, which turns the dot yellow.
  markChanged() {
    this.dirty = true;
    // The edit may have been to an encrypted list, and there is no cheap way
    // to know which from here -- so every open one is marked and re-sealed.
    // It keeps the blobs ready for a write that cannot wait.
    let any = false;
    this.data.forEach(entry => {
      if (entry.enc === true && entry.open === true) {
        entry.stale = true;
        any = true;
      }
    });
    if (any) {
      this.sealSoon();
    }
  }

  // The file needs writing, but nothing in memory changed: a rekey replaced
  // the blobs wholesale and they are already current. markChanged() would
  // mark every open list stale, and the seal that follows would rebuild those
  // blobs under the key being replaced.
  markBlobsChanged() {
    this.dirty = true;
  }

  // Writes the list out, if there is anything the server has not been given.
  // Only one write is ever on the wire; a second caller waits for the first
  // and then writes whatever is still outstanding, so an edit made during a
  // slow write is not left behind.
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
    // what is on the wire right now, so the beacon on the way out of the page
    // does not send the same text a second time
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

  // The last-chance save for a page that is going away. It cannot be awaited
  // and it cannot be checked, so it is only worth sending when there really
  // is something outstanding. Nothing can be encrypted here either --
  // WebCrypto is a promise and the page is not waiting -- so it sends the
  // blobs sealSoon() has already made.
  saveBeacon() {
    if (!this.canWrite()) {
      return false;
    }
    if (this.dirty !== true) {
      return false;
    }
    const body = this.safeBody();
    // A save started when the page was hidden is often still on the wire when
    // it is unloaded a moment later. Sending the same text again gains
    // nothing and puts two writes on one file at once.
    if (body === this.wireBody) {
      return false;
    }
    const handed = this.writeBeacon(FILE_NAME, body);
    if (handed === true) {
      // taken on trust: there is no answer to wait for
      this.sentBody = body;
      this.lastGoodBody = body;
      this.dirty = false;
    }
    return handed;
  }

  // Hands the lock back as the page goes away. Without this the lock file is
  // left holding the token of a window that no longer exists, and re-opening
  // gets a read only list until the stamp expires.
  releaseBeacon() {
    if (this.lock.owned !== true) {
      return false;
    }
    const handed = this.writeBeacon(LOCK_FILE, this.lock.line(false));
    if (handed === true) {
      this.lock.abandon();
      // The page is normally gone a moment later, but one put into the
      // back/forward cache can come back instead. Saying so here keeps the
      // window honest in that gap; reclaim() puts it back.
      this.readOnly = true;
    }
    return handed;
  }

  // Takes the lock again after the page was put away and brought back (the
  // back/forward cache), since it was handed back on the way out.
  async reclaim() {
    if (this.lock.owned === true) {
      return true;
    }
    if (this.loaded !== true) {
      return false;
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

  // Writes out anything unsaved and hands the lock back. Only ever one at a
  // time: destroy() fires this without waiting, so a re-opened window can
  // call it again while the first is still settling.
  release() {
    return singleFlight(this, 'releasing', () => this.runRelease());
  }

  async runRelease() {
    if (this.online === false) {
      // nothing to write to, so the lock is left to expire on its own
      this.lock.abandon();
      return false;
    }
    // Anything unsaved has to go out on its own, and it has to go first,
    // while the lock is still held.
    const sent = await this.save();
    // freeing the lock is worth trying even if that save did not land -- the
    // edits stay in memory either way, and a stuck lock helps nobody
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
    this.newId = null;     // row being added, while its name box is open
    this.query = "";       // what is in the search box, if there is one
    this.pwdMode = null;   // password prompt: setup / unlock / change, or not up
    this.pwdDone = null;   // what to run once it has been answered
    this.holdTimer = null; // the lock is being held down
    this.held = false;     // it was held long enough, so the press is spent
    this.epoch = 0;        // counts openings, so a late teardown knows its own
    this.busy = false;     // an exchange with the server is being waited on
    this.settling = false; // rows are up, the edit lock has not answered yet
    // rows picked out for deletion, by id. Kept here rather than read back
    // out of the rows, so a re-draw in the middle of selecting keeps them.
    this.selected = new Set();
    this.dragSort = new DragSort({
      rowClass: ROW_CLASS,
      onDrop: () => this.commitOrder()
    });
    // window level listeners, attached as a set
    this.listeners = {
      click: this.handleClick.bind(this),
      // no re-ordering while a name box is open, while rows are being picked
      // out for deletion, or while a search is holding rows back -- the gap a
      // row would be dropped into is not the gap it would land in
      pointerdown: (event) => {
        this.holdStart(event);
        if (!this.editing && this.mode !== MODE_SELECT && !this.filtering()) {
          this.dragSort.start(event);
        }
      },
      pointermove: (event) => {
        // a finger that slides off the lock has changed its mind
        this.holdCheck(event);
        this.dragSort.move(event);
      },
      pointerup: (event) => {
        this.holdCancel();
        this.dragSort.end(event);
      },
      pointercancel: (event) => {
        this.holdCancel();
        this.dragSort.end(event);
      }
    };
    // Page level listeners, attached for the life of the instance. They cost
    // nothing while the window is shut: with no lock held there is nothing to
    // write, and every one of them starts by checking exactly that.
    //
    // Held on `this` rather than passed as throwaway arrows, so dispose() can
    // take them off again. They used to be unremovable, which kept every
    // Lists instance -- its store, its lock token and its heartbeat --
    // reachable from document and window for the life of the tab.
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

  // The page is going into the background -- switching tab, switching app, or
  // on the way to being closed. This is the last moment the page is reliably
  // still alive, so a normal write is used: it can be checked and the dot
  // updated from the answer.
  handleHidden() {
    if (document.visibilityState !== 'hidden') {
      return;
    }
    if (this.store.dirty !== true) {
      return;
    }
    this.store.save().then(() => this.updateDot());
  }

  // The page is being torn down. An awaited write would be cancelled part
  // way, so this hands the list to the browser to finish on its own. It only
  // fires if the hide above did not already get the edits away, and on iOS
  // Safari it often does not fire at all -- the lock expiring covers that.
  handleUnload() {
    // the list first, while the lock is still held
    this.store.saveBeacon();
    // then the lock itself: leaving it held is what left a re-opened window
    // read only until it expired
    this.store.releaseBeacon();
  }

  // The page was put away and brought back rather than loaded fresh (the
  // back/forward cache), so the window is still on screen but the lock was
  // handed back on the way out. Ask for it again.
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
    return document.getElementById(WIN_ID) !== null;
  }

  // opens the window, or closes it again if it is already on-screen
  async activate() {
    if (this.isOpen()) {
      this.closeWindow();
      return;
    }
    this.epoch += 1;
    this.view = VIEW_LISTS;
    this.mode = MODE_OPEN;
    this.openId = null;
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
      showPopup(`[${WIN_TITLE}] server connection not available.`);
    }
    this.render();
  }

  // Close button. Anything unsaved goes out now; only a write the server
  // refuses stops the window closing.
  async closeWindow() {
    if (this.store.dirty === true) {
      const sent = await this.runBusy(() => this.store.save());
      this.updateDot();
      if (!sent) {
        this.showOverlay(true);
        return;
      }
    }
    this.destroy();
  }

  destroy() {
    this.busy = false;
    this.settling = false;
    this.holdCancel();
    this.held = false;
    this.dragSort.cancel();
    // The release writes out anything still unsaved, and that write needs the
    // key -- so the lock comes after it rather than beside it.
    const releasing = this.store.release();
    // A window closed and opened again in the moment that release takes would
    // otherwise have the key taken back off it by the chain below, after it
    // had been entered again. The count says which window this belongs to.
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
    this.newId = null;
    this.view = VIEW_LISTS;
    this.mode = MODE_OPEN;
    this.openId = null;
    this.query = "";
    this.selected.clear();
  }

  // End of life for the instance, as opposed to the window. The close button
  // uses destroy() and keeps the page listeners: a shut window still has to
  // hand the lock back if the page is hidden while a release is settling.
  dispose() {
    document.removeEventListener('visibilitychange', this.pageListeners.hidden);
    window.removeEventListener('pagehide', this.pageListeners.unload);
    window.removeEventListener('unload', this.pageListeners.unload);
    window.removeEventListener('pageshow', this.pageListeners.restore);
    this.destroy();
  }

  // Runs an exchange with the server with the ring turning by the status dot.
  // Every path that awaits the store goes through here, and the ring stops
  // whether the exchange worked or threw. A caller that has already stopped
  // it itself (showRows) is not re-drawn over.
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

  // The ring and the buttons are the only things a busy state changes, so
  // only those are re-drawn. Going through the full render() instead pulled
  // the rows out and put them straight back, which read as the list blinking
  // every time it was saved.
  renderBusy() {
    if (!this.isOpen()) {
      return;
    }
    this.renderActions();
    this.updateDot();
  }

  // the file is in and parsed: the rows replace the label straight away, and
  // the buttons stay off until the lock says whether they can be used
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
    // The back button starts hidden. The window always opens on the lists
    // view, and the first render() is a round trip away -- without this the
    // back button is on screen beside the lock for the length of the read.
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
    <button type="button" class="button button__sml list__btn" id="${CLOSE_BTN_ID}">
      <span class="button__text"><i class="fad fa-times-octagon"></i></span>
    </button>
    <button type="button" class="button button__sml list__btn" id="${DEL_BTN_ID}">
      <span class="button__text"><i class="fad fa-trash-alt" id="${DEL_ICON_ID}"></i></span>
    </button>
    <button type="button" class="button button__sml list__btn" id="${ADD_BTN_ID}">
      <span class="button__text"><i class="fad fa-folder-plus"></i></span>
    </button>
    <div class="list__status" id="${STATUS_ID}">
      <i class="fad fa-lock"></i>Read/Only
    </div>
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
        <button type="button" class="button list__dlg-btn" id="${PWD_CANCEL_BTN_ID}">
          <span class="button__text">Cancel</span>
        </button>
        <button type="button" class="button list__dlg-btn" id="${PWD_OK_BTN_ID}">
          <span class="button__text">Unlock</span>
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
    // Enter answers the password prompt and Escape drops it, the same way the
    // name box on a row behaves. The boxes live for the life of the window,
    // so these are attached once here rather than every time it opens.
    PWD_BOX_IDS.forEach(id => {
      document.getElementById(id).addEventListener('keydown', (key_event) => {
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

  // un-checked items are floated to the top; both groups keep the order they
  // are saved in, so checking an item drops it straight back where it was
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

  // Only checked items can be dragged; everything in the list view can --
  // except a locked list, which has no row on screen to drag. Saying so here
  // is what keeps commitOrder() working while one is hidden: it pairs the
  // rows it can see with the slots they came from, and a list with no row has
  // to be left out of both halves or the two no longer line up.
  isMovable(entry) {
    if (this.view !== VIEW_ITEMS) {
      return !this.store.isHidden(entry);
    }
    return entry.checked === true;
  }

  // Records an edit and re-draws. Nothing goes to the server here -- the dot
  // turns yellow and the list is written on close, on hide, or on unload.
  commit() {
    this.store.markChanged();
    this.render();
  }

  //// drawing ////

  listElem() {
    return document.getElementById(LIST_ID);
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
    // A re-draw can arrive mid-drag -- lockLost() reaches here through the
    // store's onChange -- and it replaces the rows the drag is still holding
    // a reference to. Dropping it first puts the row back where it started,
    // which is the right answer when the list is about to be rebuilt anyway.
    this.dragSort.cancel();
    // A re-draw rebuilds the search box along with everything else, and one
    // can land while it is being typed into. Noting the focus now is what
    // lets it be put back at the end, rather than the caret and a phone
    // keyboard going with the old box.
    const oldBox = document.getElementById(SEARCH_BOX_ID);
    const searching = oldBox !== null && document.activeElement === oldBox;
    const items = this.view === VIEW_ITEMS;
    this.renderHeader(items);
    list.innerHTML = "";
    if (this.busy === true && this.store.loaded !== true) {
      // Waiting on the first read: there are no rows to show yet, and the
      // ring turning by the status dot says so. Once the list has loaded a
      // busy state draws as normal -- a save leaves the rows where they are.
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
    document.getElementById(TITLE_ID).innerText = title;
    // The back button belongs to the items view and the lock to the lists
    // view, and neither is ever wanted at the same time as the other. Sharing
    // the slot rather than sitting beside it is what leaves the header -- and
    // the title centred in it -- exactly as it was.
    const backBtn = document.getElementById(BACK_BTN_ID);
    let backVisibility = "hidden";
    let backDisplay = "none";
    let lockDisplay = "";
    if (items) {
      backVisibility = "visible";
      backDisplay = "";
      lockDisplay = "none";
    }
    backBtn.style.visibility = backVisibility;
    backBtn.style.display = backDisplay;
    document.getElementById(LOCK_BTN_ID).style.display = lockDisplay;
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
    rows.forEach(row => list.appendChild(this.createRow(row)));
    // Stands in for the rows while a search matches none of them. It is built
    // with them and shown by applyFilter(), so a keystroke that empties the
    // list has nothing left to build.
    if (this.searchable()) {
      const none = makeElement('div', "list__empty");
      none.id = NO_MATCH_ID;
      none.innerText = "No matches";
      none.style.display = "none";
      list.appendChild(none);
    }
    // the items in the open list, or every item across all the lists
    let count = 0;
    let noun = 'total item';
    if (items) {
      count = rows.length;
      noun = 'item';
    } else {
      count = this.store.data.reduce((sum, entry) => sum + entry.items.length, 0);
    }
    const total = makeElement('div', "list__total");
    total.innerText = this.totalText(count, noun);
    list.appendChild(total);
  }

  // puts the caret back in the search box after a re-draw
  refocusSearch() {
    const box = document.getElementById(SEARCH_BOX_ID);
    if (!box) {
      return;
    }
    box.focus();
    // focus() on its own selects the whole value; the caret belongs where the
    // typing left it
    const end = box.value.length;
    box.setSelectionRange(end, end);
  }

  createRow(entry) {
    const items = this.view === VIEW_ITEMS;
    const select = this.mode === MODE_SELECT;
    // the lists view is the shallower of the two, so its rows are given more
    // height; the items view keeps the tighter row so more of a list fits
    let rowClass = ROW_CLASS;
    if (!items) {
      rowClass += " list__row--tall";
    }
    const row = makeElement('div', rowClass);
    row.dataset.id = entry.id;
    // What the search box matches against, folded once here rather than on
    // every keystroke: a search is then a pass over the rows and nothing
    // more, with no lookup back into the list behind them.
    row.dataset.find = String(entry.name).toLowerCase();
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
      row.appendChild(dragGripElement(this.rowsLocked() || !this.isMovable(entry)));
      if (items) {
        row.appendChild(this.roundElement(entry.checked === true, false));
      }
    }
    // The lock on a list row: a badge on one that is encrypted, and a button
    // on the row being added, which is the one moment the choice can be made.
    // It is not offered on an item row -- a list is encrypted whole.
    if (!items && (entry.enc === true || this.newId === entry.id)) {
      row.appendChild(this.rowLockElement(entry.enc === true, this.newId === entry.id));
    }
    // an item that runs to more than one line wraps instead of being cut off
    // on one; a single line row keeps the ellipsis it always had
    let labelClass = LABEL_CLASS;
    if (String(entry.name).indexOf('\n') !== -1) {
      labelClass += " list__label--multi";
    }
    const label = makeElement('span', labelClass);
    label.innerText = entry.name;
    row.appendChild(label);
    // the label is the rename target, so the rest of the row is a separate
    // filler that carries the open / complete tap
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

  // The round button in both of its jobs: the completed toggle on an item
  // row, and the delete marker on any row while select mode is on. The delete
  // one is the same button in red, so the two never look like the same thing.
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

  // The lock on a row. While it is a button it must not take the focus: the
  // name box on that same row is open, and letting the press blur it would
  // commit the name and rebuild the row out from under the press. Cancelling
  // pointerdown leaves the focus where it is; the click still arrives.
  rowLockElement(on, pressable) {
    const cell = makeElement('span', ROW_LOCK_CLASS, '<i class="fad fa-lock"></i>');
    if (on === true) {
      cell.classList.add(ROW_LOCK_ON_CLASS);
    }
    if (pressable === true) {
      cell.addEventListener('pointerdown', (event) => event.preventDefault());
    }
    return cell;
  }

  // the header lock: shut while the encrypted lists are, open once they are
  updateLockIcon() {
    const icon = document.getElementById(LOCK_ICON_ID);
    if (!icon) {
      return;
    }
    const open = this.store.unlocked();
    icon.classList.toggle("fa-lock-open", open);
    icon.classList.toggle("fa-lock", !open);
    icon.classList.toggle(LOCK_ICON_ON_CLASS, open);
    const button = document.getElementById(LOCK_BTN_ID);
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
    // Read only: everything that would write the file is taken off-screen.
    // While the server is being waited on they go too, since the list they
    // would change is not settled yet -- but without the read only notice,
    // which is not the reason.
    const settling = this.settling === true;
    const readOnly = !settling && this.store.readOnly === true;
    // A save does not take the buttons away: the list is loaded and editable,
    // and blanking them for the length of a write is the same blink the rows
    // used to have. Only an unloaded or unsettled list stands them down.
    const loading = this.busy === true && this.store.loaded !== true;
    const waiting = readOnly || settling || loading;
    let statusDisplay = "none";
    if (readOnly) {
      statusDisplay = "flex";
    }
    document.getElementById(STATUS_ID).style.display = statusDisplay;
    let buttonDisplay = "";
    if (waiting) {
      buttonDisplay = "none";
    }
    [ADD_BTN_ID, DEL_BTN_ID].forEach(id => {
      document.getElementById(id).style.display = buttonDisplay;
    });
    if (waiting) {
      return;
    }
    // the delete button becomes an X once something is selected, and the icon
    // carries the same red as the rings for as long as the mode is on
    const select = this.mode === MODE_SELECT;
    const confirm = this.selected.size > 0;
    const icon = document.getElementById(DEL_ICON_ID);
    icon.classList.toggle("fa-times", confirm);
    icon.classList.toggle("fa-trash-alt", !confirm);
    icon.classList.toggle(DEL_ICON_ON_CLASS, select);
  }

  // Where the edits stand: green once the server has taken them, yellow while
  // there are changes it has not been given, red after a failed write.
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
    // the ring turns whenever the server is being waited on, whatever the dot
    // underneath it happens to be saying
    const spinner = document.getElementById(SPINNER_ID);
    if (spinner) {
      spinner.classList.toggle(SPINNER_ON_CLASS, this.busy === true);
    }
    const dot = document.getElementById(DOT_ID);
    if (!dot) {
      return;
    }
    const state = this.dotState();
    if (state === DOT_HIDDEN) {
      dot.style.display = "none";
      return;
    }
    dot.style.display = "block";
    dot.classList.toggle(DOT_UNSAVED_CLASS, state === DOT_UNSAVED);
    dot.classList.toggle(DOT_ERROR_CLASS, state === DOT_ERROR);
    // pressing it writes the list out, so it only takes presses -- and only
    // shows the hand -- while there is something waiting to be written
    const canSave = this.store.dirty === true && this.store.canWrite();
    dot.classList.toggle(DOT_SAVE_CLASS, canSave);
    if (canSave) {
      dot.title = "Save changes";
    } else {
      dot.removeAttribute('title');
    }
  }

  // the dot pressed: write the list out now rather than waiting for the close
  // button, the page being hidden, or the page going away
  async saveNow() {
    if (this.store.dirty !== true) {
      return;
    }
    if (!this.store.canWrite()) {
      return;
    }
    await this.runBusy(() => this.store.save());
    this.updateDot();
  }

  showOverlay(show) {
    const overlay = document.getElementById(OVERLAY_ID);
    if (!overlay) {
      return;
    }
    let overlayDisplay = "none";
    if (show === true) {
      overlayDisplay = "flex";
    }
    overlay.style.display = overlayDisplay;
  }

  //// searching ////

  // Only the items view, and only once a list is long enough that finding
  // something in it by eye is work. A shorter list is on screen already.
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

  // The top row of a long list -- the same box the bookmarks menu carries.
  // Nothing is re-drawn as it is typed: applyFilter() puts the rows away in
  // place, so the box keeps the focus and the caret through every keystroke.
  searchRow() {
    return searchRowElement({
      rowClass: "list__search",
      boxId: SEARCH_BOX_ID,
      maxLength: MAX_NAME_LENGTH,
      // a re-draw arrives with the search still on, so the box comes back
      // with what was typed into it rather than empty
      value: this.query,
      onSearch: (value) => {
        this.query = value;
        this.applyFilter();
      }
    });
  }

  // Takes the rows that do not match out of view. They are hidden rather than
  // removed, so they keep their slots and everything else -- the ids on the
  // rows, the selection, the order the list is saved in -- is untouched by a
  // search. Called on every keystroke, so it does no more than walk the rows.
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
    // the grips go with them: what is on screen is no longer the whole list,
    // so there is no order in it to save
    list.classList.toggle(LIST_FIND_CLASS, query !== "");
    const none = document.getElementById(NO_MATCH_ID);
    if (none) {
      let noneDisplay = "none";
      if (query !== "" && shown === 0) {
        noneDisplay = "block";
      }
      none.style.display = noneDisplay;
    }
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

  // a search belongs to the list it was typed in, so both ways out of one
  // start with an empty box
  showItems(id) {
    this.query = "";
    this.openId = id;
    this.view = VIEW_ITEMS;
    this.setMode(MODE_OPEN);
    this.resetScroll();
  }

  showLists() {
    this.query = "";
    this.openId = null;
    this.view = VIEW_LISTS;
    this.setMode(MODE_OPEN);
    this.resetScroll();
  }

  // A view change swaps the rows for a different, usually shorter, set. The
  // list is a scroll container and its scrollTop outlives the rows being
  // replaced, so coming back from a long list to a shorter one leaves it
  // scrolled past the end of the new content. Every view starts at the top.
  //
  // On iOS that needs more than writing scrollTop. The window is a fixed,
  // transformed box, so the panel inside it scrolls on its own thread and a
  // fling carries on there after the finger has gone: a scrollTop written
  // into the middle of one is overridden the next time the fling updates.
  // Taking the overflow away for a moment ends the fling and drops the
  // scroller back to the main thread, so the reset holds.
  resetScroll() {
    const list = this.listElem();
    if (!list) {
      return;
    }
    const overflow = list.style.overflowY;
    list.style.overflowY = "hidden";
    list.scrollTop = 0;
    // reading a layout value applies the change now rather than at the end of
    // the frame, which is what actually stops the fling
    void list.offsetHeight;
    list.style.overflowY = overflow;
    list.scrollTop = 0;
  }

  // Rows can leave without the view changing -- a delete, or items floating
  // up as they are checked off -- and the same stale scrollTop shows an empty
  // panel. Anything past the new end is pulled back to it.
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

  addEntry() {
    // A new row carries a placeholder name that almost never matches what is
    // in the search box, and it is about to have its name box opened on it,
    // so the search is dropped first rather than the row arriving out of view.
    this.query = "";
    this.setMode(MODE_OPEN, false);
    const entries = this.activeList();
    let entry = { id: nextId(entries), name: "New List", items: [] };
    if (this.view === VIEW_ITEMS) {
      entry = { id: nextId(entries), name: "New Item", checked: false };
    }
    entries.push(entry);
    // Remembered for as long as its name box is open: it is the row the lock
    // is offered on, and the row that is dropped again if it is left unnamed.
    this.newId = entry.id;
    // the new row is a change in its own right: with nothing saving on a
    // timer, hiding the page before the name box is answered would otherwise
    // leave it behind
    this.commit();
    // open the name box on the new row
    const row = this.rowById(entry.id);
    if (row && typeof row.scrollIntoView === 'function') {
      row.scrollIntoView({ block: 'nearest' });
    }
    this.beginRename(entry.id, true);
  }

  // First press turns on the selection check boxes, the second either deletes
  // what is selected or leaves select mode again; either way the list ends up
  // back in its normal state.
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

  // Marks a row for deletion, or takes the mark off again. The button is
  // turned over in place rather than through a re-draw, so a long list does
  // not rebuild every time one row is picked.
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

  // the box a name is edited in: a plain input for a list, a textarea for an
  // item, which may run to more than one line
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

  beginRename(id, isNew = false) {
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
    const namebox = this.makeNamebox(multi, entry.name);
    this.sizeNamebox(namebox, row, label);
    label.replaceWith(namebox);
    if (multi) {
      // the box opens at the height the text it already holds needs
      this.growNamebox(namebox);
    }
    namebox.focus();
    namebox.select();
    let closed = false;
    const finish = (save) => {
      if (closed) {
        return;
      }
      closed = true;
      this.editing = false;
      if (this.newId === id) {
        this.newId = null; // named, or dropped -- either way it is not new now
      }
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
        // Enter opens the next line of the item rather than finishing it; the
        // box and the row grow to hold it, and tapping away still commits
        return;
      }
      key_event.preventDefault();
      finish(true);
    });
  }

  // Sets the item box to the height its text actually needs, so a new line
  // pushes the box -- and the row around it -- down by one line. Height is
  // cleared first, or a box that has grown never measures smaller again.
  // Everything on the page is border-box, so the border has to be added back
  // on top of scrollHeight, which covers the text and the padding only.
  growNamebox(namebox) {
    namebox.style.height = 'auto';
    const border = namebox.offsetHeight - namebox.clientHeight;
    namebox.style.height = `${namebox.scrollHeight + border}px`;
  }

  // The box takes over the space the name and the filler beside it already
  // occupy, so the row keeps the width and height it had before the rename
  // started (rows are max-content, so a box that sized itself would drag the
  // whole window wider). The filler steps aside for the duration.
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
    const sent = await this.runBusy(() => this.store.save());
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

  //// encrypted lists ////

  // The lock in the corner. Unlocked, it locks; locked, it asks for the
  // password, or for a new one when the record file says none has been set.
  // onDone is what the row lock hands in: what to do once the way is open.
  async lockPress(onDone) {
    if (!cryptoReady()) {
      // WebCrypto is only handed out on a secure origin, and there is no
      // second-best version of this worth falling back to
      showPopup(`[${WIN_TITLE}] encryption needs an https connection.`);
      return;
    }
    if (this.store.unlocked()) {
      await this.lockLists();
      return;
    }
    const state = await this.runBusy(() => this.store.crypt.status());
    if (state === null) {
      showPopup(`[${WIN_TITLE}] server connection not available.`);
      return;
    }
    if (state.ready === true) {
      this.showPassword(PWD_UNLOCK, onDone);
      return;
    }
    // no password yet, and setting one writes a file
    if (!this.store.canWrite()) {
      showPopup(`[${WIN_TITLE}] another user is editing, opened read only.`);
      return;
    }
    this.showPassword(PWD_SETUP, onDone);
  }

  // Shuts the encrypted lists again. Anything outstanding goes to the server
  // first: the key is about to go, and with it the only way to encrypt what
  // is still in memory. A write that fails leaves everything open, because
  // locking on top of it would throw the edits away.
  async lockLists() {
    if (this.store.dirty === true) {
      const sent = await this.runBusy(() => this.store.save());
      this.updateDot();
      if (!sent) {
        showPopup(`[${WIN_TITLE}] changes not saved, still unlocked.`);
        return false;
      }
    }
    await this.store.lockUp();
    // the list being viewed may be one that has just gone
    const entry = this.openList();
    if (this.view === VIEW_ITEMS && (!entry || this.store.isHidden(entry))) {
      this.showLists();
      return true;
    }
    this.setMode(this.mode, false);
    this.render();
    return true;
  }

  //// password prompt ////

  showPassword(mode, onDone) {
    const overlay = document.getElementById(PWD_OVERLAY_ID);
    if (!overlay) {
      return;
    }
    this.pwdMode = mode;
    this.pwdDone = null;
    if (typeof onDone === 'function') {
      this.pwdDone = onDone;
    }
    const setup = mode === PWD_SETUP;
    const change = mode === PWD_CHANGE;
    const first = document.getElementById(PWD_BOX_ID);
    const second = document.getElementById(PWD_BOX2_ID);
    const third = document.getElementById(PWD_BOX3_ID);
    let heading = "Enter lists password";
    let action = "Unlock";
    if (setup) {
      heading = "Set a lists password";
      action = "Set";
    } else if (change) {
      heading = "Change lists password";
      action = "Change";
    }
    document.getElementById(PWD_MSG_ID).innerText = heading;
    document.getElementById(PWD_OK_BTN_ID)
      .querySelector('.button__text').innerText = action;
    first.value = "";
    second.value = "";
    third.value = "";
    // The first box is whichever password the user already knows, so a change
    // reads down the boxes in the order it happens: the old one, then the new
    // one twice.
    let firstHint = "Password";
    let secondHint = "Repeat password";
    let firstFill = "current-password";
    if (setup) {
      firstHint = "New password";
      firstFill = "new-password";
    } else if (change) {
      firstHint = "Current password";
      secondHint = "New password";
    }
    first.placeholder = firstHint;
    second.placeholder = secondHint;
    first.setAttribute("autocomplete", firstFill);
    second.setAttribute("autocomplete", "new-password");
    third.setAttribute("autocomplete", "new-password");
    let secondDisplay = "none";
    if (setup || change) {
      secondDisplay = "";
    }
    let thirdDisplay = "none";
    if (change) {
      thirdDisplay = "";
    }
    second.style.display = secondDisplay;
    third.style.display = thirdDisplay;
    this.showPasswordError("");
    overlay.style.display = "flex";
    first.focus();
  }

  showPasswordError(text) {
    const line = document.getElementById(PWD_ERR_ID);
    if (line) {
      line.innerText = text || "";
    }
  }

  // The boxes are emptied on the way out: a password left sitting in an input
  // is one more place it can be read from than it needs to be.
  closePassword() {
    const overlay = document.getElementById(PWD_OVERLAY_ID);
    if (overlay) {
      overlay.style.display = "none";
    }
    PWD_BOX_IDS.forEach(id => {
      const box = document.getElementById(id);
      if (box) {
        box.value = "";
      }
    });
    this.pwdMode = null;
    this.pwdDone = null;
    this.showPasswordError("");
  }

  async passwordSubmit() {
    if (this.pwdMode === null) {
      return;
    }
    const first = document.getElementById(PWD_BOX_ID);
    const second = document.getElementById(PWD_BOX2_ID);
    const third = document.getElementById(PWD_BOX3_ID);
    const password = first.value;
    const setup = this.pwdMode === PWD_SETUP;
    if (this.pwdMode === PWD_CHANGE) {
      await this.passwordChange(password, second.value, third.value);
      return;
    }
    if (setup && password !== second.value) {
      this.showPasswordError("passwords do not match");
      second.value = "";
      second.focus();
      return;
    }
    // The derivation is deliberately slow -- that is the whole point of it --
    // so the ring by the status dot turns while it runs.
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
      this.showPasswordError(out.error);
      first.value = "";
      second.value = "";
      first.focus();
      return;
    }
    const done = this.pwdDone;
    this.closePassword();
    // whatever was already encrypted opens now
    const found = await this.runBusy(() => this.store.unsealAll());
    if (found.failed > 0) {
      showPopup(`[${WIN_TITLE}] ${found.failed} encrypted list(s) would not open.`);
    }
    this.render();
    if (done) {
      done();
    }
  }

  //// the lock on a row ////

  // Pressed on the row being added, this is what makes the list an encrypted
  // one. The key has to be in hand first, so an unlock -- or a first password
  // -- comes before the choice takes effect.
  toggleRowCrypt(id) {
    if (this.view === VIEW_ITEMS || this.rowsLocked()) {
      return;
    }
    const entry = this.store.data.find(other => other.id === id);
    if (!entry || this.newId !== id) {
      return; // the badge on a saved row says what it is, and is not a switch
    }
    if (entry.enc === true) {
      this.setRowSealed(entry, id, false);
      return;
    }
    if (this.store.unlocked()) {
      // the name box is still open on this row, so nothing is re-drawn: the
      // badge is turned over where it stands and the name commits as usual
      this.setRowSealed(entry, id, true);
      return;
    }
    // The prompt takes the focus, which closes the name box and commits what
    // has been typed so far. The row itself stays, so it is picked up again
    // on the way back -- as the new row it still is, if that is what it was.
    const wasNew = this.newId === id;
    this.lockPress(() => {
      const later = this.store.data.find(other => other.id === id);
      if (!later) {
        return; // the box was left empty and the row went with it
      }
      this.store.setSealed(later, true);
      if (wasNew) {
        this.newId = id;
      }
      this.commit();
      this.reopenName(id, wasNew);
    });
  }

  // turns the badge over in place, without re-drawing the row under the box
  setRowSealed(entry, id, sealed) {
    this.store.setSealed(entry, sealed);
    this.markRowLock(id, sealed);
    this.store.markChanged();
    this.updateDot();
  }

  markRowLock(id, on) {
    const row = this.rowById(id);
    if (!row) {
      return;
    }
    const cell = row.querySelector(`.${ROW_LOCK_CLASS}`);
    if (cell) {
      cell.classList.toggle(ROW_LOCK_ON_CLASS, on === true);
    }
  }

  reopenName(id, isNew) {
    const row = this.rowById(id);
    if (!row) {
      return;
    }
    if (typeof row.scrollIntoView === 'function') {
      row.scrollIntoView({ block: 'nearest' });
    }
    this.beginRename(id, isNew === true);
  }

  //// changing the password ////
  //
  // Held down rather than tapped. The change re-encrypts every encrypted list
  // under the new key and writes them out, which is why it is a hold and not
  // a button of its own -- it is not something to reach by accident.

  // The press has begun on the lock. It counts whether the lists are open or
  // shut: the hold spends the press either way.
  holdStart(event) {
    this.holdCancel();
    // A hold that opened the password prompt leaves its click to the overlay
    // rather than to the button, so the flag meant to swallow that click can
    // still be set. A fresh press clears it, or the next tap on the lock
    // would be the one swallowed.
    this.held = false;
    if (!event.target.closest(`#${LOCK_BTN_ID}`)) {
      return;
    }
    const icon = document.getElementById(LOCK_ICON_ID);
    if (icon) {
      // the icon warms up over the hold, so it is visibly doing something
      // rather than looking like a press that missed
      icon.classList.add(LOCK_HOLD_CLASS);
    }
    this.holdTimer = setTimeout(() => {
      this.holdTimer = null;
      this.held = true; // the click that follows is spent on this
      this.clearHoldStyle();
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
    this.clearHoldStyle();
  }

  clearHoldStyle() {
    const icon = document.getElementById(LOCK_ICON_ID);
    if (icon) {
      icon.classList.remove(LOCK_HOLD_CLASS);
    }
  }

  // the lock has been held for its full time
  async changePress() {
    if (!cryptoReady()) {
      showPopup(`[${WIN_TITLE}] encryption needs an https connection.`);
      return;
    }
    // the re-encrypted lists have to go back to the server, so this needs the
    // edit lock as much as any other write does
    if (!this.store.canWrite()) {
      showPopup(`[${WIN_TITLE}] another user is editing, opened read only.`);
      return;
    }
    const state = await this.runBusy(() => this.store.crypt.status());
    if (state === null) {
      showPopup(`[${WIN_TITLE}] server connection not available.`);
      return;
    }
    if (state.ready !== true) {
      showPopup(`[${WIN_TITLE}] no list password has been set.`);
      return;
    }
    this.showPassword(PWD_CHANGE);
  }

  async passwordChange(oldPassword, newPassword, repeated) {
    const second = document.getElementById(PWD_BOX2_ID);
    const third = document.getElementById(PWD_BOX3_ID);
    // change() unlocks with the old password to do its work, so this is the
    // last point at which whether the user had the lists open is knowable
    const wasOpen = this.store.unlocked();
    if (newPassword !== repeated) {
      this.showPasswordError("new passwords do not match");
      third.value = "";
      third.focus();
      return;
    }
    const out = await this.runBusy(() => this.store.crypt.change(
      oldPassword, newPassword, (nextKey) => this.rekeyLists(nextKey)));
    if (!this.isOpen()) {
      return; // the window went while the keys were being worked out
    }
    if (out.ok !== true) {
      this.showPasswordError(out.error);
      const first = document.getElementById(PWD_BOX_ID);
      first.value = "";
      second.value = "";
      third.value = "";
      first.focus();
      return;
    }
    this.closePassword();
    // The change leaves the new key in hand. Lists that were already open
    // stay open under it -- shutting them as a side effect of a password
    // change is not what was asked for. With nothing open the key has nothing
    // to hold, so it goes straight back out again.
    if (!wasOpen) {
      await this.store.lockUp();
    }
    this.updateDot();
    this.render();
    if (out.error) {
      showPopup(`[${WIN_TITLE}] ${out.error}.`);
    } else {
      showPopup(`[${WIN_TITLE}] password changed.`);
    }
  }

  // Handed the new key by change(): every locked list is opened with the old
  // key -- which is still the one the store is holding at this point -- and
  // sealed again under the new one, and the whole file goes back to the
  // server. Nothing is swapped in until all of them have come out, and a
  // write that does not land puts the old blobs back, so the file and the
  // password record never disagree about which key opens what.
  async rekeyLists(nextKey) {
    // An open list carries plaintext its blob may not have caught up with, so
    // the blobs are brought up to date first -- under the old key, which is
    // what change() is still holding. A background seal is let finish before
    // that, so the two are not writing the same entry at once.
    await this.store.settleSeal();
    if (await this.store.sealOpen() !== true) {
      return false; // a list that would not seal, so nothing is changed
    }
    const swapped = [];
    for (const entry of this.store.data) {
      if (entry.enc !== true) {
        continue;
      }
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
    const button = event.target.closest('button');
    if (button) {
      this.handleButton(button.id);
      return;
    }
    const row = event.target.closest(`.${ROW_CLASS}`);
    if (!row) {
      return;
    }
    // Ahead of the editing guard on purpose: the lock is pressed while the
    // name box on that row is open, which is the only time it is a button.
    if (event.target.closest(`.${ROW_LOCK_CLASS}`)) {
      this.toggleRowCrypt(Number(row.dataset.id));
      return;
    }
    if (this.editing || event.target.closest('.drag-grip')) {
      return;
    }
    const id = Number(row.dataset.id);
    if (this.mode === MODE_SELECT) {
      // tapping anywhere on the row, the round button included, selects it
      this.toggleSelected(id, row);
      return;
    }
    // tapping the name (or the few pixels of padding around it) renames
    if (!this.rowsLocked() && event.target.closest(`.${LABEL_CLASS}`)) {
      this.beginRename(id, false);
      return;
    }
    if (this.view !== VIEW_ITEMS) {
      this.showItems(id);
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
        this.showLists();
        return;
      // Unlocking only reads, so it is offered on a read only window too --
      // the setup path checks for itself before it writes anything.
      case LOCK_BTN_ID:
        if (this.held === true) {
          // the press was answered by the hold, and the click it leaves
          // behind would otherwise ask for the password a second time
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

  // Reads the new order back out of the window and saves it. The rows that
  // cannot be dragged keep the slots they already hold, so an un-checked item
  // stays where it is until it is checked again.
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
    const moved = this.rowElements()
      .map(row => entries.find(entry => entry.id === Number(row.dataset.id)))
      .filter(entry => entry && this.isMovable(entry));
    if (moved.length !== slots.length) {
      return;
    }
    moved.forEach((entry, idx) => { entries[slots[idx]] = entry; });
    this.commit();
  }
}
