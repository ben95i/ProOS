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
// The edit lock lives in its own file, lists-lock.save, as a single row:
//   OWNER_TOKEN|TIMESTAMP
// A blank token, or a timestamp older than the expiry, means the lock is free.
// Whoever opens the window first claims it and edits; anyone opening it while
// the lock is held gets a read only window. The lock is released on close and
// expires on its own if a browser goes away without closing, because the
// owner re-stamps the timestamp while the window is open.
//
// Keeping it out of the list file is what makes that re-stamp cheap: it is a
// one row write to a tiny file, so it can run often without pushing the whole
// list back over the wire, and it never touches the list data at all. Older
// list files carry a leading LOCK| row from when the two shared a file; it
// parses as "not a list row" and is dropped the first time the list is saved.
//
// Nothing is written on a timer. The list is saved when the close button is
// pressed, when the page is hidden (switching tab or app), and when the page
// is being unloaded. Everything in between is held in memory.
//
// The dot in the top right corner reports where those edits stand: green once
// the server has taken them, yellow while there are changes it has not been
// given yet, and red after a write has failed. A failed write changes nothing
// on the server, the edits stay in memory, and closing the window offers to
// retry or discard them.
//
// Same file API as the bookmarks menu: GET /api/read?file=<name> and
// POST /api/write?file=<name> (text/plain body), for both files.

import { splitMenuData, cleanString, isNumeric, removeWindow, showPopup,
  dragGripElement, DragSort, FileLock, readFile, writeFile, sendFileBeacon,
  html } from './utils.js';

const FILE_NAME = 'lists.save';
const LOCK_FILE = 'lists-lock.save';
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
const DOT_UNSAVED_CLASS = 'list__dot--unsaved';
const DOT_SAVE_CLASS = 'list__dot--save';
const SPINNER_ID = 'list__spinner';
const SPINNER_ON_CLASS = 'list__spinner--on';
const DOT_ERROR_CLASS = 'list__dot--error';
// file layout
const COL_DELIMITER = '|'; // separates the columns of a row
// dot states, in the order they take precedence
const DOT_HIDDEN = 0;  // nothing has been tried yet
const DOT_SAVED = 1;   // green -- the server holds what is on screen
const DOT_UNSAVED = 2; // yellow -- edits the server has not been given
const DOT_ERROR = 3;   // red -- a write failed
// Stored names are printable ASCII, with the column separator "|" swapped
// out. "/" and "\" used to go with it and are now kept -- nothing downstream
// treats a name as a path.
//
// An item may hold more than one line. The file is one row per item, so a
// newline cannot be stored as itself: it goes in as "~", which is why "~" is
// taken out of the text on the way in and never survives to the file as a
// literal. List names stay on one line.
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

//// shared helpers ////

// names are trimmed to printable ASCII and kept to one line of sane length
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
  if (name.length >= MAX_NAME_LENGTH) {
    return `${name.substring(0, MAX_NAME_LENGTH)}...`;
  }
  return name;
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
  if (name.length >= MAX_ITEM_LENGTH) {
    return `${name.substring(0, MAX_ITEM_LENGTH)}...`;
  }
  return name;
}

// item text <-> the one row it is stored on: the newlines it carries are the
// only thing that cannot go in the file as itself
function encodeLines(value) {
  return String(value).split('\n').join(NEWLINE_MARK);
}

function decodeLines(value) {
  return String(value).split(NEWLINE_MARK).join('\n');
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
    if (typeof onChange === 'function') {
      this.onChange = onChange;
    } else {
      this.onChange = function () {};
    }
    this.data = [];         // [{ id, name, items: [{ id, name, checked }] }]
    this.readOnly = false;  // another user holds the lock, or nothing loaded
    this.online = null;     // last server exchange: true / false / not tried
    this.dirty = false;     // edits in memory that the server has not been given
    this.saving = null;     // in-flight save, so writes cannot overlap
    this.sentBody = null;   // the list text the server is known to hold
    this.loaded = false;    // a parsed list is standing in this.data
    this.lastGoodBody = ""; // last body known to be safe to write back
    this.releasing = null;  // release in flight, so two cannot race
    // one editor at a time, in a file of its own (utils.js)
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

  // Hands the file to the browser to send on its own. The implementation
  // moved to utils.js when the bookmarks menu grew an unload path of its own
  // and needed the identical thing; this stays as the store's way in.
  writeBeacon(file, text) {
    return sendFileBeacon(file, text);
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
  // list that was actually loaded is standing in it. A session that never
  // loaded -- a read that failed, or one torn down and re-opened while a
  // release was still settling -- holds an empty array, and writing that out
  // would erase the file; the last body known to be good goes back instead,
  // which is what the server already has, so the write changes nothing.
  // A loaded list that the user has genuinely emptied still writes empty.
  safeBody() {
    if (this.loaded === true) {
      return this.buildBody();
    }
    return this.lastGoodBody;
  }

  //// session ////

  // Loads the list and takes the edit lock when it is free. onData is called
  // as soon as the list is parsed: claiming the lock is a second round trip,
  // and on a slow link there is no reason to hold the rows off-screen for it
  // -- it changes only whether they can be edited, not what they say.
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
    // Two files now, so two reads -- but they are asked for together and come
    // back together, which costs the same wait as the one read used to.
    const [text, lockText] = await Promise.all([this.read(), this.lock.read()]);
    if (text === null) {
      // nothing loaded, so there is nothing safe to write back
      this.data = [];
      this.readOnly = true;
      return;
    }
    this.data = this.parse(text);
    this.loaded = true;
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
  // list is re-read rather than kept: the winner is the one allowed to write,
  // so what is on screen may already be behind.
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
    }
    showPopup(`[${WIN_TITLE}] another user is editing, opened read only.`);
    this.onChange();
  }

  canWrite() {
    return this.lock.owned === true && this.readOnly !== true;
  }

  // An edit was made. Nothing is written here: the list is saved when the
  // window is closed, when the page is hidden, and when it is unloaded. This
  // only records that the server is now behind, which turns the dot yellow.
  markChanged() {
    this.dirty = true;
  }

  // Writes the list out, if there is anything the server has not been given.
  // Only one write is ever on the wire; a second caller waits for the first
  // and then writes whatever is still outstanding, so an edit made during a
  // slow write is not left behind.
  save() {
    const previous = this.saving;
    const running = this.runSave(previous);
    this.saving = running;
    // the promise .finally() returns is a new one, and it rejects too; with
    // nothing attached that showed up as an unhandled rejection even though
    // every caller of save() handles the failure itself
    running.finally(() => {
      if (this.saving === running) {
        this.saving = null;
      }
    }).catch(() => {});
    return running;
  }

  async runSave(previous) {
    if (previous !== null) {
      try {
        await previous;
      } catch (error) {
        // a failed write is already reported through the status dot
      }
    }
    if (!this.canWrite()) {
      return true; // read only, or the lock has already gone back
    }
    if (this.dirty !== true) {
      return true; // the server is already holding this list
    }
    const body = this.safeBody();
    if (body === this.sentBody) {
      // the edits since the last write cancelled each other out, so the file
      // on the server is already this text
      this.dirty = false;
      return true;
    }
    const sent = await this.write(body);
    if (sent === true) {
      this.sentBody = body;
      this.lastGoodBody = body; // the server took it, so it is safe to redo
      this.dirty = false;
    }
    return sent;
  }

  // The last-chance save for a page that is going away. It cannot be awaited
  // and it cannot be checked, so it is only worth sending when there really
  // is something outstanding.
  saveBeacon() {
    if (!this.canWrite()) {
      return false;
    }
    if (this.dirty !== true) {
      return false;
    }
    const body = this.safeBody();
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
  // gets a read only list until the stamp expires -- the next visit brings a
  // new token with it, so the old row reads as somebody else editing.
  releaseBeacon() {
    if (this.lock.owned !== true) {
      return false;
    }
    const handed = this.writeBeacon(LOCK_FILE, this.lock.line(false));
    if (handed === true) {
      this.lock.abandon();
      // The page is normally gone a moment later, but a page put into the
      // back/forward cache can come back instead. Saying so here keeps the
      // window honest in that gap rather than looking editable while it can
      // no longer write; reclaim() puts it back.
      this.readOnly = true;
    }
    return handed;
  }

  // Takes the lock again after the page was put away and then brought back
  // (the back/forward cache). The lock was handed back on the way out, so
  // this window owns nothing until it asks again.
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
    if (this.releasing) {
      return this.releasing;
    }
    const running = this.runRelease();
    this.releasing = running;
    running.finally(() => {
      if (this.releasing === running) {
        this.releasing = null;
      }
    }).catch(() => {});
    return running;
  }

  async runRelease() {
    if (this.online === false) {
      // nothing to write to, so the lock is left to expire on its own
      this.lock.abandon();
      return false;
    }
    // The lock lives in its own file now, so handing it back no longer
    // carries the list with it: anything unsaved has to go out on its own,
    // and it has to go first, while the lock is still held.
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
    this.openId = null;   // list being viewed
    this.editing = false; // inline name box is open
    this.busy = false;    // an exchange with the server is being waited on
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
    // Page level listeners, attached for the life of the instance. They cost
    // nothing while the window is shut: with no lock held there is nothing
    // to write, and every one of them starts by checking exactly that.
    //
    // Held on `this` rather than passed as throwaway arrows, so dispose()
    // can take them off again. They used to be unremovable, which kept every
    // Lists instance -- its store, its lock token and its heartbeat --
    // reachable from document and window for the life of the tab: a
    // re-render (GoToHomePage) left the old one listening beside the new,
    // and both answered pagehide by writing the same file.
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

  // The page is going into the background -- switching tab, switching app,
  // or on the way to being closed. This is the last moment the page is
  // reliably still alive, so it is the one that matters: a normal write is
  // used, because it can be checked and the dot updated from the answer.
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
  // Safari it often does not fire at all -- the lock expiring on its own is
  // what covers that.
  handleUnload() {
    // the list first, while the lock is still held
    this.store.saveBeacon();
    // then the lock itself. Leaving it held is what left a re-opened window
    // read only: the row still named a window that had gone, and the next
    // visit arrives with a new token, so its own old row looks like somebody
    // else editing until it expires.
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

  // Close button. Anything the write interval was still holding goes out
  // now; only a write the server refuses stops the window closing.
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
    this.dragSort.cancel();
    this.store.release();
    removeWindow(WIN_ID);
    this.editing = false;
    this.view = VIEW_LISTS;
    this.mode = MODE_OPEN;
    this.openId = null;
    this.selected.clear();
  }

  // End of life for the instance, as opposed to the window. The close button
  // uses destroy() instead and keeps the page listeners: a shut window still
  // has to hand the lock back if the page is hidden while a release is
  // settling. This is what the page calls when it is letting the instance go
  // for good.
  dispose() {
    document.removeEventListener('visibilitychange', this.pageListeners.hidden);
    window.removeEventListener('pagehide', this.pageListeners.unload);
    window.removeEventListener('unload', this.pageListeners.unload);
    window.removeEventListener('pageshow', this.pageListeners.restore);
    this.destroy();
  }

  // Runs an exchange with the server with the ring turning by the status
  // dot. Every path that awaits the store goes through here, and the ring
  // stops whether the exchange worked or threw. A caller that has already
  // stopped it itself (showRows) is not re-drawn over.
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
    const markup = html`
<div id="${WIN_ID}" class="prompt_win list__win nohide">
  <span class="list__dot" id="${DOT_ID}"></span>
  <span class="list__spinner" id="${SPINNER_ID}"></span>
  <div class="list__header">
    <button type="button" class="list__navbtn" id="${BACK_BTN_ID}">
      <i class="fad fa-chevron-left"></i>
    </button>
    <div class="list__title" id="${TITLE_ID}">${WIN_TITLE}</div>
  </div>
  <div class="list__list scroll-bar-style" id="${LIST_ID}"></div>
  <div class="list__actions">
    <button type="button" class="button button__sml list__btn" id="${CLOSE_BTN_ID}">
      <span class="button__text"><i class="fad fa-times-octagon"></i></span>
    </button>
    <button type="button" class="button button__sml list__btn" id="${ADD_BTN_ID}">
      <span class="button__text"><i class="fad fa-folder-plus"></i></span>
    </button>
    <button type="button" class="button button__sml list__btn" id="${DEL_BTN_ID}">
      <span class="button__text"><i class="fad fa-trash-alt" id="${DEL_ICON_ID}"></i></span>
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
    return list.querySelector(`.list__row[data-id="${id}"]`);
  }

  render() {
    const list = this.listElem();
    if (!list) {
      return;
    }
    // A re-draw can arrive mid-drag -- lockLost() reaches here through the
    // store's onChange -- and it replaces the rows the drag is still holding
    // a reference to, so the row being carried would be left behind on a
    // detached node. Dropping it first puts it back where it started, which
    // is the right answer when the list is about to be rebuilt anyway. A drop
    // that has already happened cancels to nothing, so the ordinary path
    // through commitOrder() is untouched.
    this.dragSort.cancel();
    const items = this.view === VIEW_ITEMS;
    const entry = this.openList();
    // title / back button
    let title = WIN_TITLE;
    if (items && entry) {
      title = entry.name;
    }
    document.getElementById(TITLE_ID).innerText = title;
    let backVisibility = "hidden";
    if (items) {
      backVisibility = "visible";
    }
    document.getElementById(BACK_BTN_ID).style.visibility = backVisibility;
    // rows
    list.innerHTML = "";
    if (this.busy === true && this.store.loaded !== true) {
      // Waiting on the first read: there are no rows to show yet, and the
      // ring turning by the status dot says so. Once the list has loaded a
      // busy state draws as normal -- a save leaves the rows where they are.
      this.renderActions();
      this.updateDot();
      return;
    }
    const rows = this.visibleRows();
    if (rows.length === 0) {
      const empty = document.createElement('div');
      empty.className = "list__empty";
      if (items) {
        empty.innerText = "No items yet";
      } else {
        empty.innerText = "No lists yet";
      }
      list.appendChild(empty);
    } else {
      rows.forEach(row => list.appendChild(this.createRow(row)));
      // total sitting at the bottom of the rows: the items in the open list,
      // or every item across all the lists in the main menu
      let count = 0;
      let noun = 'total item';
      if (items) {
        count = rows.length;
        noun = 'item';
      } else {
        count = this.store.data.reduce((sum, entry) => sum + entry.items.length, 0);
      }
      const total = document.createElement('div');
      total.className = "list__total";
      total.innerText = this.totalText(count, noun);
      list.appendChild(total);
    }
    this.clampScroll(list);
    this.renderActions();
    this.updateDot();
  }

  createRow(entry) {
    const items = this.view === VIEW_ITEMS;
    const select = this.mode === MODE_SELECT;
    const row = document.createElement('div');
    // the lists view is the shallower of the two, so its rows are given more
    // height; the items view keeps the tighter row so more of a list fits
    row.className = "list__row";
    if (!items) {
      row.className += " list__row--tall";
    }
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
    // an item that runs to more than one line wraps instead of being cut off
    // on one; a single line row keeps the ellipsis it always had
    if (String(entry.name).indexOf('\n') !== -1) {
      label.className += " list__label--multi";
    }
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
    let className = "list__round";
    if (forDelete === true) {
      className += " list__round--del";
    }
    if (on === true) {
      className += " list__round--on";
    }
    return this.createCell('span', className, '<i class="fad fa-check"></i>');
  }

  totalText(count, noun) {
    if (count === 1) {
      return `${count} ${noun}`;
    }
    return `${count} ${noun}s`;
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
    [ADD_BTN_ID, DEL_BTN_ID].forEach(id => {
      let buttonDisplay = "";
      if (waiting) {
        buttonDisplay = "none";
      }
      document.getElementById(id).style.display = buttonDisplay;
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

  // Where the edits stand: green once the server has taken them, yellow while
  // there are changes it has not been given yet, red after a failed write.
  dotState() {
    if (this.store.online === null) {
      return DOT_HIDDEN; // nothing has been sent yet, so there is nothing to say
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

  // the dot pressed: write the list out now rather than waiting for the
  // close button, the page being hidden, or the page going away
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
    if (overlay) {
      let overlayDisplay = "none";
      if (show === true) {
        overlayDisplay = "flex";
      }
      overlay.style.display = overlayDisplay;
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
    this.resetScroll();
  }

  showLists() {
    this.openId = null;
    this.view = VIEW_LISTS;
    this.setMode(MODE_OPEN);
    this.resetScroll();
  }

  // A view change swaps the rows for a different, usually shorter, set. The
  // list is a scroll container and its scrollTop outlives the rows being
  // replaced, so coming back from a long list to a shorter one leaves it
  // scrolled past the end of the new content -- an empty panel until the
  // browser gets round to clamping it. Every view starts at the top instead.
  //
  // On iOS that needs more than writing scrollTop. The window is a fixed,
  // transformed box, so the panel inside it scrolls on its own thread, and a
  // fling carries on there after the finger has gone: a scrollTop written
  // into the middle of one is overridden the next time the fling updates, and
  // the panel is left parked past the end of the new rows with nothing to
  // paint. Taking the overflow away for a moment ends the fling and drops the
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
    this.setMode(MODE_OPEN, false);
    const entries = this.activeList();
    let entry = null;
    if (this.view === VIEW_ITEMS) {
      entry = { id: nextId(entries), name: "New Item", checked: false };
    } else {
      entry = { id: nextId(entries), name: "New List", items: [] };
    }
    entries.push(entry);
    // the new row is a change in its own right: with nothing saving on a
    // timer any more, hiding the page before the name box is answered would
    // otherwise leave it behind
    this.commit();
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
    let label = null;
    if (row) {
      label = row.querySelector('.list__label');
    }
    if (!label || !entry || this.editing || this.rowsLocked()) {
      return;
    }
    this.editing = true;
    // An item can hold more than one line, so it is edited in a textarea that
    // grows downward as lines are added. A list name is still one line, and
    // still a plain text box.
    const multi = this.view === VIEW_ITEMS;
    let namebox = null;
    if (multi) {
      namebox = document.createElement('textarea');
      namebox.rows = 1;
      namebox.maxLength = MAX_ITEM_LENGTH;
    } else {
      namebox = document.createElement('input');
      namebox.type = "text";
    }
    namebox.className = "list__namebox";
    if (multi) {
      namebox.className += " list__namebox--multi";
    }
    namebox.setAttribute("autocorrect", "off");
    namebox.setAttribute("autocapitalize", "none");
    namebox.value = entry.name;
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
