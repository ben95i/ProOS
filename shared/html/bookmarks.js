// Automate Website - bookmarks menu
//
// Loading and saving the file, drawing the drop-down, the search box, add /
// edit mode, the add-edit window and drag re-ordering.
//
// Saved file (bookmarks.save), one line per bookmark:
//   URL|NAME|ICON
// ICON is the site's favicon as a URL-safe base64 PNG (24x24, 32 colours)
// built by the sitelookup API, empty when none could be fetched; rows saved
// before that column existed simply have two. Row order is menu order, so
// dragging a row and saving is all the re-ordering that is needed.
//
// Rows carry the same grip handle and the same search box the lists window
// uses (DragSort / searchRowElement in utils.js).
//
// Editing is single-user, on the shared FileLock in a file of its own. The
// menu is readable by everybody at all times -- the lock is taken only while
// an edit window is open or a row is being moved. While another user holds it
// the grips are hidden and the edit icons report the menu as read only.

import { html, showPopup, cleanString, splitMenuData, base64URLSafeEncode,
  base64URLSafeToStandard, sendCmd, dragGripElement, DragSort, FileLock,
  readFile, writeFile, sendFileBeacon, LOCK_TAG, searchRowElement, searchTerm,
  searchMatches, makeElement, singleFlight } from './utils.js';

const FILE_NAME = 'bookmarks.save';
const LOCK_FILE = 'bookmarks-lock.save';
const MENU_ID = 'bookmarks';         // drop-down container, drawn by main.js
const BUTTONS_ID = 'bookmark-buttons';
const ADD_BTN_ID = 'btn-add-bookmark';
const STAR_BTN_ID = 'btn-show-bookmarks';
const SEARCH_ID = 'bookmark__search';
const LOADING_ID = 'bookmark__loading';
const SEARCH_BOX_ID = 'bookmark__search_txt';
const PROMPT_ID = 'editFav__prompt';
const ITEM_CLASS = 'bookmarked__item';
const LABEL_CLASS = 'bookmarked__label';
const EDIT_ICON_CLASS = 'bookmarked__edit';
const FAV_ICON_CLASS = 'bookmarked__favicon';
const GLYPH_CLASS = 'bookmarked__glyph';
const SPINNER_CLASS = 'bookmark-spinner';
const ROW_LOADING_CLASS = 'bookmarked__edit--loading';
const ADD_LOADING_CLASS = 'bookmark__add--loading';
// the lookup button reports on itself instead of writing into the name box
const LOOKUP_ICON_CLASS = 'fa-search';
const LOOKUP_BUSY_CLASS = 'editFav__button--busy';
// the same ring the rows and the add button turn
const LOOKUP_SPIN_CLASS = SPINNER_CLASS;
const LOOKUP_FAIL_ICON_CLASS = 'fa-exclamation-triangle';
const LOOKUP_FAIL_CLASS = 'editFav__button--failed';
// The backend can answer with its own failure text instead of a title. The
// match is the whole answer, so a real page title that mentions the phrase is
// still a title.
const LOOKUP_FAIL_REPLY = /^site\s*lookup\s*error!?$/i;
const LOOKUP_ERROR_MS = 3000; // how long the fail icon sits before the magnifier
// why a lookup gave up, reported in a popup
const LOOKUP_NO_URL = 'no URL to look up';
const LOOKUP_NO_CLIPBOARD = 'no URL entered, and the clipboard could not be read';
const LOOKUP_NO_ANSWER = 'no answer from the server';
const LOOKUP_NO_TITLE = 'the site returned no title';
const PREVIEW_ID = 'editFav__iconpreview';
// the six buttons of the add / edit window
const EDIT_BTN_IDS = {
  up: 'editFav__btn-up',
  lookup: 'editFav__btn-lookup',
  down: 'editFav__btn-down',
  save: 'editFav__btn-save',
  cancel: 'editFav__btn-cancel',
  remove: 'editFav__btn-delete'
};
const PREVIEW_EMPTY_CLASS = 'editFav__iconpreview--empty';
const SELECTED_CLASS = 'dd-selected';
const MAX_NAME_LENGTH = 42;
const LOCKED_MSG = "read/only, locked by another user";
const INFO_ICON_HTML = '<i class="fad fa-info-circle"></i>';
const ICON_CHARS = /^[A-Za-z0-9_@-]*$/;
const MAX_ICON_LENGTH = 8192;
// menu states
const BKM_INACTIVE = 0;
const BKM_OPEN_MODE = 1;
const BKM_EDIT_MODE = 2;

// adds the https:// prefix when the URL carries no scheme
export function addHTTPtoURL(linkin) {
  if (linkin === "" || linkin === null) {
    return "";
  }
  const lower = linkin.toLowerCase();
  if (lower.startsWith('http://', 0) || lower.startsWith('https://', 0)) {
    return linkin;
  }
  return "https://" + linkin;
}

// A saved icon is only used after it passes this: it goes straight into an
// <img src>, so anything that is not plain URL-safe base64 is dropped.
function validIcon(icon) {
  if (!icon || typeof icon !== 'string') {
    return "";
  }
  const trimmed = icon.trim();
  if (trimmed.length > MAX_ICON_LENGTH || !ICON_CHARS.test(trimmed)) {
    return "";
  }
  return trimmed;
}

// the stored bytes name their own format: a PNG opens with the signature
// below, and icons saved before the switch to PNG are JPEG
function iconMimeType(icon) {
  if (icon.startsWith('iVBORw0KGgo')) {
    return 'image/png';
  }
  return 'image/jpeg';
}

// the row's icon markup, or the info glyph when there is no icon
function iconMarkup(icon) {
  if (icon === "") {
    return INFO_ICON_HTML;
  }
  return html`<img class="${FAV_ICON_CLASS}" alt="" width="24" height="24"
    src="data:${iconMimeType(icon)};base64,${base64URLSafeToStandard(icon)}">`;
}

export default class Bookmarks {

  // options: removeDropdowns(), the drop-down teardown in menus.js
  constructor(options = {}) {
    this.removeDropdowns = function () {};
    if (typeof options.removeDropdowns === 'function') {
      this.removeDropdowns = options.removeDropdowns;
    }
    this.state = BKM_INACTIVE;
    this.pendingAdd = null;  // placeholder row waiting on the add window
    this.promptIcon = "";    // icon held by the open add / edit window
    this.readOnly = false;   // another user held the lock when we last looked
    this.loadedBody = "";    // the file as it was when the menu was drawn
    this.menuDrawn = false;  // rows are drawn and safe to read back out
    this.releasing = null;   // release in flight, so two cannot race
    this.claiming = null;    // claim in flight, so two cannot race
    this.loading = false;    // the file is on its way in
    this.lookupBtn = null;   // site lookup button of the open window
    this.lookupTimer = null; // error icon waiting to turn back into the glass
    this.lock = new FileLock({
      file: LOCK_FILE,
      onLost: () => this.lockLost()
    });
    this.dragSort = new DragSort({
      rowClass: ITEM_CLASS,
      onDrop: () => this.dropMove()
    });
    this.listeners = {
      pointerdown: (event) => this.dragSort.start(event),
      pointermove: (event) => this.dragSort.move(event),
      pointerup: (event) => this.dragSort.end(event),
      pointercancel: (event) => this.dragSort.end(event)
    };
  }

  //// menu state ////

  isEditMode() {
    return this.state === BKM_EDIT_MODE;
  }

  menuElem() {
    return document.getElementById(MENU_ID);
  }

  items() {
    const menu = this.menuElem();
    if (!menu) {
      return [];
    }
    return [...menu.getElementsByClassName(ITEM_CLASS)];
  }

  // star button: draws the menu, or hides it when it is already open
  toggle() {
    if (this.state !== BKM_INACTIVE) {
      this.removeDropdowns();
      return;
    }
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    this.removeDropdowns();
    menu.style.display = 'block';
    this.state = BKM_OPEN_MODE;
    this.renderButtons();
    this.load();
  }

  // called by removeDropdowns()
  close() {
    this.discardPendingAdd();
    // hand the lock back before the rows this would write are torn down
    this.releaseLock();
    if (this.menuElem()) {
      this.clearMenu();
    }
    const buttons = document.getElementById(BUTTONS_ID);
    if (buttons) {
      buttons.remove();
    }
    this.dragSort.cancel();
    this.clearRowLoading();
    this.removeSearch();
    this.removeLoading();
    this.loading = false;
    this.closePrompt();
    this.readOnly = false;
    // loadedBody is deliberately kept: it is the copy the server holds, and a
    // release still settling after this needs it to write back instead of the
    // torn-down rows. load() replaces it the next time the menu is drawn.
    this.state = BKM_INACTIVE;
  }

  // add button, rendered next to the star on demand (editing is started from
  // the favicon on each row)
  renderButtons() {
    if (document.getElementById(BUTTONS_ID)) {
      return;
    }
    const starBtn = document.getElementById(STAR_BTN_ID);
    if (starBtn) {
      starBtn.insertAdjacentHTML("afterend", html`
        <div class="bookmark-buttons" id="${BUTTONS_ID}">
          <a id="${ADD_BTN_ID}">
            <i class="fad fa-folder-plus"></i>
            <span class="${SPINNER_CLASS}"></span>
          </a>
        </div>`);
    }
  }

  //// file ////

  // Reads the bookmarks and the lock together. Two files, so two requests --
  // but they go out side by side and come back as a single wait.
  async readBoth() {
    const [text, lockText] = await Promise.all([
      readFile(FILE_NAME),
      this.lock.read()
    ]);
    return { text: text, lockText: lockText, busy: this.lock.busy(lockText) };
  }

  async load() {
    this.showLoading();
    const { text, busy } = await this.readBoth();
    this.loading = false;
    if (this.state === BKM_INACTIVE) {
      return; // menu closed while loading
    }
    if (text === null || typeof text !== 'string') {
      this.removeLoading();
      showPopup("[bookmarks] no data to parse.");
      return;
    }
    this.readOnly = busy;
    this.drawMenu(text);
    this.showSearch(); // replaces the loading row
  }

  // stand-in first row, swapped for the search box once the file is drawn
  showLoading() {
    const menu = this.menuElem();
    if (!menu || document.getElementById(LOADING_ID)) {
      return;
    }
    this.loading = true;
    const loadingRow = document.createElement("a");
    loadingRow.id = LOADING_ID;
    loadingRow.classList.add("nohide");
    loadingRow.innerText = "Loading...";
    menu.insertBefore(loadingRow, menu.firstChild);
  }

  removeLoading() {
    const loadingRow = document.getElementById(LOADING_ID);
    if (loadingRow) {
      loadingRow.remove();
    }
  }

  // re-reads the file and redraws the menu from it
  async reload() {
    const { text, busy } = await this.readBoth();
    if (this.state === BKM_INACTIVE || text === null) {
      return false;
    }
    this.readOnly = busy;
    this.drawMenu(text);
    this.showSearch();
    return true;
  }

  // bookmark rows only: the old leading lock row and anything short is left
  // out. 'https://x|Name|icon' -> ['https://x', 'Name', 'icon']
  bookmarkRows(data) {
    return splitMenuData(data)
      .map(line => cleanString(line))
      .filter(line => line)
      .map(line => line.split("|"))
      .filter(cols => cols.length >= 2 && cols[0] !== LOCK_TAG);
  }

  // rows written before the icon column exists just have two columns
  rowIcon(cols) {
    if (cols.length > 2) {
      return cols[2];
    }
    return "";
  }

  // 0=URL, 1=Name, 2=Icon
  drawMenu(data) {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    this.clearMenu();
    this.bookmarkRows(data).forEach(cols => {
      menu.appendChild(this.createItem(cols[0], cols[1].trim(), this.rowIcon(cols)));
    });
    // remember the copy this was drawn from, so an edit can tell whether the
    // file moved on underneath it
    this.loadedBody = this.buildBody();
    this.menuDrawn = true; // the rows on-screen now match the file
    Object.keys(this.listeners).forEach(name => {
      menu.addEventListener(name, this.listeners[name]);
    });
  }

  clearMenu() {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    // the rows are about to go: nothing may be read back out of them until
    // drawMenu() has put a known-good set up again
    this.menuDrawn = false;
    Object.keys(this.listeners).forEach(name => {
      menu.removeEventListener(name, this.listeners[name]);
    });
    this.items().forEach(item => item.remove());
  }

  createItem(url, name, icon) {
    const item = makeElement('a', ITEM_CLASS);
    // drag handle, hidden outright while another user is editing
    item.appendChild(dragGripElement(this.readOnly === true));
    const label = makeElement('span', LABEL_CLASS);
    label.innerText = name;
    item.appendChild(label);
    // the site's favicon, which is also the edit button for this row. The
    // icon lives in its own holder so redrawing it leaves the spinner that
    // rings it alone.
    const editIcon = makeElement('span', EDIT_ICON_CLASS);
    editIcon.appendChild(makeElement('span', GLYPH_CLASS));
    editIcon.appendChild(makeElement('span', SPINNER_CLASS));
    item.appendChild(editIcon);
    // the URL and the icon travel on the element, as the menu always has
    Object.defineProperty(item, "url", {
      enumerable: false, writable: true, value: url
    });
    Object.defineProperty(item, "icon", {
      enumerable: false, writable: true, value: ""
    });
    this.setItemIcon(item, icon);
    item.addEventListener("click", (event) => this.clickItem(item, event));
    return item;
  }

  // The menu is only really open while this window is driving it. The
  // container is drawn by main.js and outlives close(), so testing for the
  // element alone says nothing about whether there are rows in it.
  menuIsOpen() {
    return this.state !== BKM_INACTIVE && this.menuElem() !== null;
  }

  // What to write for the bookmark rows. The DOM is only worth reading back
  // while a drawn set of rows is standing in it; once the menu has been torn
  // down there are no rows left, and writing that out would erase the file.
  // The copy the menu was drawn from goes back instead, which is what the
  // server already holds -- so the write is a no-op rather than a wipe. A
  // menu that is still up and genuinely empty keeps menuDrawn, so an
  // intentional empty file still writes.
  safeBody() {
    if (this.menuDrawn === true && this.menuElem()) {
      return this.buildBody();
    }
    return this.loadedBody;
  }

  // reads the menu back out in its current order
  buildBody() {
    let _file = "";
    this.items().forEach(item => {
      // item.url is a string; Object.values() used to explode it into
      // characters and rejoin them, and would throw outright on a null
      let url = "";
      if (item.url !== null && item.url !== undefined) {
        url = String(item.url);
      }
      _file += `${url}|${this.itemName(item)}|${item.icon}\n`;
    });
    return _file;
  }

  // the file body as the server has it, in the same shape buildBody() writes
  bodyFromText(text) {
    let body = "";
    this.bookmarkRows(text).forEach(cols => {
      body += `${cols[0]}|${cols[1].trim()}|${this.rowIcon(cols)}\n`;
    });
    return body;
  }

  //// item text and icon ////

  labelElem(item) {
    return item.querySelector(`.${LABEL_CLASS}`);
  }

  itemName(item) {
    const label = this.labelElem(item);
    if (label) {
      return label.innerText;
    }
    return item.innerText;
  }

  setItemName(item, name) {
    const label = this.labelElem(item);
    if (label) {
      label.innerText = name;
    }
  }

  // stores the icon on the row and redraws the edit button to match
  setItemIcon(item, icon) {
    item.icon = validIcon(icon);
    const glyph = item.querySelector(`.${GLYPH_CLASS}`);
    if (glyph) {
      glyph.innerHTML = iconMarkup(item.icon);
    }
  }

  // the spinner rings the icon of the row that was clicked, and only while
  // its window is on its way
  setRowLoading(item, on) {
    this.clearRowLoading();
    if (on !== true || !item) {
      return;
    }
    const holder = item.querySelector(`.${EDIT_ICON_CLASS}`);
    if (holder) {
      holder.classList.add(ROW_LOADING_CLASS);
    }
  }

  clearRowLoading() {
    const menu = this.menuElem();
    if (menu) {
      menu.querySelectorAll(`.${ROW_LOADING_CLASS}`)
        .forEach(holder => holder.classList.remove(ROW_LOADING_CLASS));
    }
  }

  // The add button reports on itself the same way a row does: it has no row
  // yet, so it rings the button while the edit lock is being taken.
  addBtnElem() {
    return document.getElementById(ADD_BTN_ID);
  }

  setAddLoading(on) {
    const btn = this.addBtnElem();
    if (btn) { // close() takes the whole button away, spinner and all
      btn.classList.toggle(ADD_LOADING_CLASS, on === true);
    }
  }

  // the ring doubles as the guard: a second tap while it is turning would
  // start a second claim on a lock this window is already taking
  addIsLoading() {
    const btn = this.addBtnElem();
    return btn !== null && btn.classList.contains(ADD_LOADING_CLASS);
  }

  //// search ////

  showSearch() {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    // a redraw calls this again -- never leave a second box behind
    this.removeSearch();
    this.removeLoading();
    // "nohide" goes on every part of it: the menu decides what closes it from
    // what was pressed, and pressing the icon to clear a search must not shut
    // the menu underneath.
    const searchMenuItem = searchRowElement({
      tag: 'a',
      id: SEARCH_ID,
      partClass: 'nohide',
      boxId: SEARCH_BOX_ID,
      onSearch: (value) => this.filterItems(value)
    });
    menu.insertBefore(searchMenuItem, menu.firstChild); // always the first row
    const searchBox = document.getElementById(SEARCH_BOX_ID);
    if (searchBox) {
      searchBox.focus();
    }
  }

  filterItems(termIn) {
    const term = searchTerm(termIn);
    this.items().forEach(item => {
      let rowDisplay = "none";
      if (searchMatches(this.itemName(item), term)) {
        rowDisplay = "";
      }
      item.style.display = rowDisplay;
    });
  }

  removeSearch() {
    const search = document.getElementById(SEARCH_ID);
    if (search) search.remove();
    const search_txt = document.getElementById(SEARCH_BOX_ID);
    if (search_txt) search_txt.remove();
  }

  //// edit lock ////
  //
  // Taken for one edit or one move and handed straight back, so the menu is
  // only locked while somebody is actually changing it.

  // Takes the lock for the operation about to happen. Everything that could
  // write the file goes through here first, so a stale menu can never
  // overwrite another user's work: the file is re-read, and if it changed
  // since this menu was drawn the menu is redrawn and the caller backs off.
  //
  // Only ever one claim at a time. Three paths reach here -- the add button,
  // a row icon and a drag being dropped -- and none is awaited by its caller,
  // so two taps in quick succession used to run two of these side by side:
  // both saw an unowned lock and both wrote a claim.
  beginLockedEdit() {
    return singleFlight(this, 'claiming', () => this.runBeginLockedEdit());
  }

  async runBeginLockedEdit() {
    if (this.lock.owned === true) {
      return true; // already ours, an edit window is open
    }
    const { text, lockText, busy } = await this.readBoth();
    if (!this.menuIsOpen()) {
      return false; // closed while the file was being read; claim nothing
    }
    if (text === null) {
      showPopup("[bookmarks] server connection not available.");
      return false;
    }
    if (busy) {
      this.lockedElsewhere(text);
      return false;
    }
    if (this.bodyFromText(text) !== this.loadedBody) {
      // somebody else changed the file since this menu was drawn
      this.readOnly = false;
      this.drawMenu(text);
      this.showSearch();
      showPopup("[bookmarks] reloaded, changed by another user.");
      return false;
    }
    // the claim is a one row write to the lock file; the bookmarks are not
    // touched, so nothing is written until the edit is actually finished
    if (!await this.lock.claim(lockText)) {
      this.lockedElsewhere(text);
      return false;
    }
    // Two windows can claim in the same moment, so the claim still has to be
    // confirmed -- but that waits on a settling delay, and holding the edit
    // window back for it is a visible stall. It runs behind the window
    // instead: the only thing written before it lands is the claim itself,
    // and the lock's own settle() holds every later write until the answer is
    // in. A window that loses is shut again by lockLost().
    this.lock.confirm();
    this.readOnly = false;
    return true;
  }

  // Writes the menu out and hands the lock back. Only ever one release at a
  // time: close() and the edit window closing can both call this in the same
  // moment, and two in flight would each capture their own body -- the one
  // that resumed last winning, which after a teardown holds no rows at all.
  releaseLock() {
    return singleFlight(this, 'releasing', () => this.runRelease());
  }

  async runRelease() {
    if (this.lock.owned !== true) {
      return true;
    }
    // Read the rows out before anything is awaited: close() tears the menu
    // down in the same tick this is called from, so waiting first would leave
    // nothing to write.
    const body = this.safeBody();
    await this.lock.settle();
    if (this.lock.owned !== true) {
      return true; // the claim was lost, and lockLost() has reported it
    }
    // the bookmarks go first, while the lock is still held; the lock is freed
    // after
    const sent = await writeFile(FILE_NAME, body);
    if (sent) {
      this.loadedBody = body;
    }
    // worth freeing even if that write did not land -- the rows are still in
    // the menu either way, and a stuck lock helps nobody
    await this.lock.release(true);
    return sent;
  }

  // Last chance as the page goes away. An ordinary write is exactly what the
  // browser cancels during a teardown, so the rows and the lock row are
  // handed over as beacons instead. Nothing here can be checked, so it only
  // runs when the lock is really held.
  unloadBeacon() {
    if (this.lock.owned !== true) {
      return false;
    }
    // the bookmarks first, while the lock is still held -- the same order
    // runRelease() writes them in
    const body = this.safeBody();
    const handed = sendFileBeacon(FILE_NAME, body);
    if (handed === true) {
      this.loadedBody = body;
    }
    // then the lock row itself, freed whether or not the rows got away
    if (sendFileBeacon(LOCK_FILE, this.lock.line(false)) === true) {
      this.lock.abandon();
    }
    return handed;
  }

  async endLockedEdit() {
    if (!await this.releaseLock()) {
      showPopup("[bookmarks] failed to save, server connection not available.");
      return false;
    }
    return true;
  }

  // another window holds the lock: no moving, no editing
  lockedElsewhere(text) {
    const wasOpen = this.readOnly !== true;
    this.readOnly = true;
    showPopup(`[bookmarks] ${LOCKED_MSG}.`);
    // only worth redrawing the first time, to take the grips away
    if (wasOpen && typeof text === 'string') {
      this.drawMenu(text);
      this.showSearch();
    }
  }

  // A competing window won the lock in the moment we both claimed it. The
  // bookmarks are re-read here to redraw from whatever the winner has: what
  // is on screen may already be behind.
  async lockLost() {
    this.closePrompt();
    this.discardPendingAdd();
    if (this.state !== BKM_INACTIVE) {
      this.state = BKM_OPEN_MODE;
    }
    const text = await readFile(FILE_NAME);
    this.lockedElsewhere(text);
  }

  // a row was dragged: take the lock, write the new order, hand it back
  async dropMove() {
    if (!await this.beginLockedEdit()) {
      // could not take it -- put the menu back the way the file has it
      await this.reload();
      return;
    }
    await this.endLockedEdit();
  }

  //// add / edit mode ////

  async addBookmark() {
    const menu = this.menuElem();
    if (!menu || this.loading === true || this.addIsLoading()) {
      return;
    }
    // the lock takes a read and a write before there is anything to show, so
    // the button turns from the tap until the window is up
    this.setAddLoading(true);
    try {
      if (!await this.beginLockedEdit()) {
        return; // another user is editing, or the file moved on
      }
      if (!this.menuIsOpen()) {
        // the menu was closed while the lock was being taken: hand it back
        // and draw nothing, or the window opens over a torn-down menu
        await this.releaseLock();
        return;
      }
      this.discardPendingAdd();
      this.closePrompt();
      const item = this.createItem("about:blank", "New Bookmark", "");
      item.classList.add(SELECTED_CLASS);
      // insert at the top of the bookmarks, below the search box
      menu.insertBefore(item, this.firstItem());
      this.pendingAdd = item;
      this.state = BKM_EDIT_MODE;
      this.drawPrompt(true, null, null, item);
    } finally {
      this.setAddLoading(false);
    }
  }

  // opens the add / edit window on a row; called by the row icon and by
  // clicking any other row while the window is already up
  async editItem(item) {
    this.setRowLoading(item, true);
    try {
      if (!await this.beginLockedEdit()) {
        return; // another user is editing, or the menu was just redrawn
      }
      if (!this.menuIsOpen() || !item.isConnected) {
        // the menu was closed, or the row redrawn, while the lock was taken
        await this.releaseLock();
        return;
      }
      this.discardPendingAdd();
      this.closePrompt();
      this.state = BKM_EDIT_MODE;
      item.classList.add(SELECTED_CLASS);
      this.drawPrompt(false, item.url, this.itemName(item), item);
    } finally {
      this.clearRowLoading();
    }
  }

  // closes the window, writes the menu out and hands the lock back, leaving
  // the bookmarks menu on-screen
  async endEditMode() {
    this.discardPendingAdd();
    this.closePrompt();
    if (this.state !== BKM_INACTIVE) {
      this.state = BKM_OPEN_MODE;
    }
    await this.endLockedEdit();
  }

  // an add that was never saved leaves nothing behind
  discardPendingAdd() {
    if (this.pendingAdd) {
      this.pendingAdd.remove();
      this.pendingAdd = null;
    }
  }

  // first bookmark row, so inserts land under the search box
  firstItem() {
    const items = this.items();
    if (items.length > 0) {
      return items[0];
    }
    return null;
  }

  clickItem(item, event) {
    // the grip is for dragging only
    if (this.dragSort.isDragging() ||
        (event && event.target.closest('.drag-grip'))) {
      return;
    }
    // the favicon always opens the window on its own row, and while the
    // window is up any row that is clicked becomes the one being edited
    if ((event && event.target.closest(`.${EDIT_ICON_CLASS}`)) ||
        this.state === BKM_EDIT_MODE) {
      this.editItem(item);
      return;
    }
    const url = item.url;
    if (!(url === null || url == "" || url == "about:blank")) {
      window.open(url, "_blank");
    }
  }

  //// add / edit window ////

  closePrompt() {
    this.clearLookupTimer();
    this.lookupBtn = null;
    this.items().forEach(item => item.classList.remove(SELECTED_CLASS));
    const favPrompt = document.getElementById(PROMPT_ID);
    if (favPrompt) {
      favPrompt.remove();
    }
  }

  // Trims a name to the stored length, with an ellipsis when it was cut.
  boundedName(name) {
    if (name.length >= MAX_NAME_LENGTH) {
      return `${name.substring(0, MAX_NAME_LENGTH)}...`;
    }
    return name;
  }

  // Writes the window's values back onto the row. False when a box was left
  // empty, which leaves the window open with the reason in its placeholder.
  savePrompt(item, nameBox, urlBox) {
    let stopsave = false;
    if (nameBox.value === null || nameBox.value == "") {
      nameBox.placeholder = "Name cannot be empty";
      stopsave = true;
    }
    if (urlBox.value === null || urlBox.value == "") {
      urlBox.placeholder = "URL cannot be empty";
      stopsave = true;
    }
    if (stopsave === true) {
      return false;
    }
    if (item) {
      this.setItemName(item, this.boundedName(nameBox.value.replaceAll("|", "-")));
      const _boxurl = addHTTPtoURL(urlBox.value.replaceAll("|", "-"));
      item.url = _boxurl;
      urlBox.value = _boxurl;
      this.setItemIcon(item, this.promptIcon);
    }
    return true;
  }

  drawPrompt(add, url, name, item) {
    // the window edits one icon at a time; a lookup replaces it, saving
    // writes it back onto the row
    this.promptIcon = "";
    if (item) {
      this.promptIcon = validIcon(item.icon);
    }
    // banner text with the icon preview beside it, name box, URL box, six
    // action buttons
    const editFavPrompt = document.createElement("div");
    editFavPrompt.className = "editFav__win";
    editFavPrompt.id = PROMPT_ID;
    editFavPrompt.classList.add("prompt_win");
    let bannerText = "Edit Bookmark";
    if (add === true) {
      bannerText = "Add Bookmark";
    }
    editFavPrompt.innerHTML = html`
      <div class="editFav__toptext">
        <span class="editFav__toplabel">${bannerText}</span>
        <span class="editFav__iconpreview" id="${PREVIEW_ID}"></span>
      </div>
      <input type="text" id="editFav__namebox" class="editFav__textbox"
             autocorrect="off" autocapitalize="none">
      <input type="text" id="editFav__urlbox" class="editFav__textbox"
             autocorrect="off" autocapitalize="none">
      <button class="editFav__button button fad fa-arrow-up" type="button"
              id="${EDIT_BTN_IDS.up}"></button>
      <button class="editFav__button button fad fa-search" type="button"
              id="${EDIT_BTN_IDS.lookup}"></button>
      <button class="editFav__button button fad fa-arrow-down" type="button"
              id="${EDIT_BTN_IDS.down}"></button>
      <button class="editFav__button button fad fa-save" type="button"
              id="${EDIT_BTN_IDS.save}"></button>
      <button class="editFav__button button fad fa-times-octagon" type="button"
              id="${EDIT_BTN_IDS.cancel}"></button>
      <button class="editFav__button button fad fa-trash-alt" type="button"
              id="${EDIT_BTN_IDS.remove}"></button>
    `;
    const editFavText = editFavPrompt.querySelector(".editFav__toplabel");
    const nameBox = editFavPrompt.querySelector("#editFav__namebox");
    const urlBox = editFavPrompt.querySelector("#editFav__urlbox");
    // found inside the window: it is not on the page yet
    const editFavUpBtn = editFavPrompt.querySelector(`#${EDIT_BTN_IDS.up}`);
    const editFavLookupBtn = editFavPrompt.querySelector(`#${EDIT_BTN_IDS.lookup}`);
    const editFavDownBtn = editFavPrompt.querySelector(`#${EDIT_BTN_IDS.down}`);
    const editFavSaveBtn = editFavPrompt.querySelector(`#${EDIT_BTN_IDS.save}`);
    const editFavCancelBtn = editFavPrompt.querySelector(`#${EDIT_BTN_IDS.cancel}`);
    const editFavDeleteBtn = editFavPrompt.querySelector(`#${EDIT_BTN_IDS.remove}`);
    this.lookupBtn = editFavLookupBtn;
    // set via properties (null coerces to "" exactly as before)
    nameBox.value = name;
    urlBox.value = url;
    if (add === true) {
      nameBox.placeholder = "Enter Name";
      urlBox.placeholder = "Enter URL";
    }
    document.body.appendChild(editFavPrompt);
    // favicon preview beside the banner text. Clicking it drops the icon; the
    // row keeps its old one until the window is saved.
    editFavPrompt.querySelector(`#${PREVIEW_ID}`)
      .addEventListener("click", () => {
        if (this.promptIcon !== "") {
          this.promptIcon = "";
          this.drawIconPreview();
        }
      });
    this.drawIconPreview();
    editFavPrompt.addEventListener("click", (btn_event) => {
      if (btn_event.target.tagName !== "BUTTON") { return; }
      const pressed = btn_event.target;
      if (pressed === editFavUpBtn) {
        this.shiftItem(item, true);
      }
      if (pressed === editFavDownBtn) {
        this.shiftItem(item, false);
      }
      if (pressed === editFavCancelBtn) {
        // closes the window, not the bookmarks menu
        this.endEditMode();
      }
      if (pressed === editFavLookupBtn) {
        this.lookupURL();
      }
      if (pressed === editFavSaveBtn) {
        if (!this.savePrompt(item, nameBox, urlBox)) {
          return;
        }
        if (item) {
          editFavText.innerHTML = "Changes Saved";
        }
        // the new row is a real bookmark now
        if (this.pendingAdd === item) {
          this.pendingAdd = null;
        }
        // closing writes the menu out and hands the lock back
        this.endEditMode();
      }
      if (pressed === editFavDeleteBtn) {
        if (this.pendingAdd === item) {
          this.pendingAdd = null;
        }
        item.remove();
        this.endEditMode();
      }
    });
  }

  // moves a row one place up or down the menu
  shiftItem(item, up) {
    if (!item) {
      return;
    }
    if (up === true) {
      if (item.previousElementSibling) {
        item.parentNode.insertBefore(item, item.previousElementSibling);
      }
      return;
    }
    if (item.nextElementSibling) {
      item.parentNode.insertBefore(item.nextElementSibling, item);
    }
  }

  //// URL lookup ////

  // Redraws the preview beside the banner text from this.promptIcon. With no
  // icon to show the slot is left empty rather than standing in with the info
  // icon; the text boxes below are not affected either way.
  drawIconPreview() {
    const preview = document.getElementById(PREVIEW_ID);
    if (!preview) {
      return;
    }
    const empty = this.promptIcon === "";
    preview.classList.toggle(PREVIEW_EMPTY_CLASS, empty);
    let markup = iconMarkup(this.promptIcon);
    if (empty) {
      markup = "";
    }
    preview.innerHTML = markup;
  }

  // The button reports on itself: its icon gives way to a spinner while the
  // lookup runs, and the name box is only ever written on success.
  async lookupURL() {
    const urlBoxElem = document.getElementById("editFav__urlbox");
    const nameBoxElem = document.getElementById("editFav__namebox");
    if (!urlBoxElem || !nameBoxElem) {
      return;
    }
    this.setLookupBusy();
    let urlin = urlBoxElem.value;
    if (urlin === null || urlin == "") {
      // read from the clipboard when the URL box is empty
      try {
        urlin = await navigator.clipboard.readText();
      } catch (error) {
        this.lookupFailed(LOOKUP_NO_CLIPBOARD);
        return;
      }
    }
    const url = addHTTPtoURL(urlin);
    if (url === "") {
      this.lookupFailed(LOOKUP_NO_URL);
      return;
    }
    urlBoxElem.value = url;
    try {
      const data = await sendCmd('main', 'sitelookup', base64URLSafeEncode(url));
      if (data === null || data === undefined) {
        // sendCmd() swallows the reason, so the request itself failed
        this.lookupFailed(LOOKUP_NO_ANSWER);
        return;
      }
      // the API answers { title, icon }; a plain string is still accepted so
      // an older backend keeps working
      let title = data;
      let icon = "";
      if (typeof data === 'object') {
        title = data.title;
        icon = data.icon;
      }
      // remove invalid characters; a title of nothing but spaces is no title
      let titleText = "";
      if (title !== null && title !== undefined) {
        titleText = String(title);
      }
      const siteName = (cleanString(titleText) || "").trim();
      if (siteName === "") {
        // nothing usable came back -- the boxes are left exactly as they were
        this.lookupFailed(LOOKUP_NO_TITLE);
        return;
      }
      if (LOOKUP_FAIL_REPLY.test(siteName)) {
        // the answer is the backend reporting a failure, not a title
        this.lookupFailed(siteName);
        return;
      }
      // the only path that writes to the name box
      nameBoxElem.value = siteName;
      // show the fetched icon; saving writes it to the row
      this.promptIcon = validIcon(icon);
      this.drawIconPreview();
      this.setLookupIcon();
    } catch (error) {
      let reason = String(error);
      if (error && error.message) {
        reason = String(error.message);
      }
      this.lookupFailed(reason);
    }
  }

  //// lookup button state ////

  // Swaps the magnifier for the site's own spinner ring while the lookup
  // runs. The ring is a child element, so it turns inside a button that stays
  // still, and it is centred because .button is the positioning context.
  setLookupBusy() {
    this.clearLookupTimer();
    if (!this.lookupBtn) {
      return;
    }
    this.lookupBtn.innerHTML = "";
    this.lookupBtn.classList.remove('fad', LOOKUP_ICON_CLASS,
      LOOKUP_FAIL_ICON_CLASS, LOOKUP_FAIL_CLASS);
    this.lookupBtn.classList.add(LOOKUP_BUSY_CLASS);
    this.lookupBtn.appendChild(makeElement('span', LOOKUP_SPIN_CLASS));
  }

  // puts the magnifier back, or shows the fail icon in its place
  setLookupIcon(iconClass = LOOKUP_ICON_CLASS) {
    this.clearLookupTimer();
    if (!this.lookupBtn) {
      return;
    }
    this.lookupBtn.innerHTML = "";
    this.lookupBtn.classList.remove(LOOKUP_BUSY_CLASS, LOOKUP_FAIL_CLASS,
      LOOKUP_ICON_CLASS, LOOKUP_FAIL_ICON_CLASS);
    this.lookupBtn.classList.add('fad', iconClass);
    if (iconClass === LOOKUP_FAIL_ICON_CLASS) {
      this.lookupBtn.classList.add(LOOKUP_FAIL_CLASS);
    }
  }

  // The button only reports that it failed; the reason goes to the popup,
  // where there is room for it. Nothing is written to either text box.
  lookupFailed(reason) {
    // the backend's own wording may already end in punctuation
    const detail = String(reason).trim();
    let stop = ".";
    if (/[.!?]$/.test(detail)) {
      stop = "";
    }
    showPopup(`[bookmarks] site lookup failed: ${detail}${stop}`);
    this.setLookupIcon(LOOKUP_FAIL_ICON_CLASS);
    this.lookupTimer = setTimeout(() => {
      this.lookupTimer = null;
      this.setLookupIcon();
    }, LOOKUP_ERROR_MS);
  }

  clearLookupTimer() {
    if (this.lookupTimer) {
      clearTimeout(this.lookupTimer);
      this.lookupTimer = null;
    }
  }
}
