// Automate Website - Bookmarks Menu
//
// Everything the bookmarks menu does lives in this class: loading and saving
// the file, drawing the drop-down, the search box, add / edit mode, the
// add-edit window and drag re-ordering.
//
// Saved file (bookmarks.save), one line per bookmark:
//   URL|NAME|ICON
// ICON is the site's favicon as a URL-safe base64 PNG (24x24, 32 colours),
// built by the sitelookup API. It is empty when no icon could be fetched,
// and rows saved before this column existed simply have two columns.
// The row order in the file is the menu order, so dragging a row and saving
// is all the re-ordering that is needed.
//
// Rows carry a grip handle on the left, the same one the lists window uses
// (DragSort / drag-grip in utils.js).
//
// Editing is single-user, on the same lock the lists window uses (FileLock in
// utils.js): the first row of the file is LOCK|STATE|OWNER_TOKEN|TIMESTAMP|0.
// The menu is readable by everybody at all times -- the lock is only taken
// for the moment an edit window is open or a row is being moved, and handed
// straight back afterwards. While another user holds it the grips are hidden
// and the edit icons report the menu as read only.

import { html, showPopup, cleanString, splitMenuData, base64URLSafeEncode,
  base64URLSafeToStandard, sendCmd, dragGripElement, DragSort, FileLock,
  readFile, writeFile, LOCK_TAG } from './utils.js';

const FILE_NAME = 'bookmarks.save';
const MENU_ID = 'bookmarks';         // drop-down container, drawn by main.js
const BUTTONS_ID = 'bookmark-buttons';
const ADD_BTN_ID = 'btn-add-bookmark';
const STAR_BTN_ID = 'btn-show-bookmarks';
const SEARCH_ID = 'bookmark__search';
const LOADING_ID = 'bookmark__loading';
const SEARCH_BOX_ID = 'bookmark__search_txt';
const PROMPT_ID = 'editFav__prompt';
const ITEM_CLASS = 'bookmarked__item';
const EDIT_ICON_CLASS = 'bookmarked__edit';
const FAV_ICON_CLASS = 'bookmarked__favicon';
const GLYPH_CLASS = 'bookmarked__glyph';
const SPINNER_CLASS = 'bookmark-spinner';
const ROW_LOADING_CLASS = 'bookmarked__edit--loading';
const ADD_LOADING_CLASS = 'bookmark__add--loading';
// the lookup button reports on itself instead of writing into the name box
const LOOKUP_ICON_CLASS = 'fa-search';
const LOOKUP_BUSY_CLASS = 'editFav__button--busy';
const LOOKUP_SPIN_CLASS = 'editFav__button-spin';
const LOOKUP_SPIN_ICON = 'fad fa-sync';
const LOOKUP_FAIL_ICON_CLASS = 'fa-exclamation-triangle';
const LOOKUP_FAIL_CLASS = 'editFav__button--failed';
// The backend can answer with its own failure text instead of a title. The
// match is the whole answer, so a real page title that happens to mention
// the phrase is still a title.
const LOOKUP_FAIL_REPLY = /^site\s*lookup\s*error!?$/i;
const LOOKUP_ERROR_MS = 3000; // how long the fail icon sits before the magnifier
// why a lookup gave up, reported in a popup while the button just shows the
// fail icon -- the name box is never touched on any of these
const LOOKUP_NO_URL = 'no URL to look up';
const LOOKUP_NO_CLIPBOARD = 'no URL entered, and the clipboard could not be read';
const LOOKUP_NO_ANSWER = 'no answer from the server';
const LOOKUP_NO_TITLE = 'the site returned no title';
const PREVIEW_ID = 'editFav__iconpreview';
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

// add HTTPs prefix if not defined
export function addHTTPtoURL(linkin) {
  let linkout;
  if (linkin === "" || linkin === null) {
    linkout = "";
  } else {
    if (linkin.toLowerCase().startsWith('http://', 0) ||
        linkin.toLowerCase().startsWith('https://', 0)) {
      linkout = linkin;
    } else {
      linkout = "https://" + linkin;
    }
  }
  return linkout;
}

// a saved icon is only ever used after it passes this: it goes straight
// into an <img src>, so anything that is not plain URL-safe base64 is dropped
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
  return icon.startsWith('iVBORw0KGgo') ? 'image/png' : 'image/jpeg';
}

// URL-safe base64 back to the standard alphabet a data URI expects
function iconMarkup(icon) {
  if (icon === "") {
    return INFO_ICON_HTML;
  }
  return html`<img class="${FAV_ICON_CLASS}" alt="" width="24" height="24"
    src="data:${iconMimeType(icon)};base64,${base64URLSafeToStandard(icon)}">`;
}

export default class Bookmarks {

  constructor(options = {}) {
    // main.js hands in its drop-down teardown so the cancel button and an
    // opened link close the menus exactly as they did before
    this.removeDropdowns = typeof options.removeDropdowns === 'function'
      ? options.removeDropdowns : function () {};
    this.state = BKM_INACTIVE;
    this.pendingAdd = null;  // placeholder row waiting on the add window
    this.promptIcon = "";    // icon held by the open add / edit window
    this.readOnly = false;   // another user held the lock when we last looked
    this.loadedBody = "";    // the file as it was when the menu was drawn
    this.menuDrawn = false;  // rows are drawn and safe to read back out
    this.releasing = null;   // release in flight, so two cannot race
    this.confirming = null;  // claim waiting to be confirmed, if any
    this.loading = false;    // the file is on its way in
    this.lookupBtn = null;   // site lookup button of the open window
    this.lookupTimer = null; // error text waiting to turn back into the icon
    // one editor at a time, shared with the lists window (utils.js)
    this.lock = new FileLock({
      read: () => readFile(FILE_NAME),
      write: (text) => writeFile(FILE_NAME, text),
      body: () => this.safeBody(),
      onLost: (text) => this.lockLost(text)
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
    return menu ? [...menu.getElementsByClassName(ITEM_CLASS)] : [];
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
    // loadedBody is deliberately kept: it is the copy the server holds, and
    // a release still settling after this needs it to write back instead of
    // the torn-down rows. load() replaces it the next time the menu is drawn.
    this.state = BKM_INACTIVE;
  }

  // add button, rendered next to the star on demand (editing is started from
  // the pencil on each row)
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

  async load() {
    this.showLoading();
    const { text, busy } = await this.lock.check();
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
    // replaces the loading row
    this.showSearch();
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
    const { text, busy } = await this.lock.check();
    if (this.state === BKM_INACTIVE || text === null) {
      return false;
    }
    this.readOnly = busy;
    this.drawMenu(text);
    this.showSearch();
    return true;
  }

  // bookmark rows only: the lock row and anything short is left out
  bookmarkRows(data) {
    return splitMenuData(data)
      .map(line => cleanString(line))
      .filter(line => line)
      .map(line => line.split("|"))
      .filter(cols => cols.length >= 2 && cols[0] !== LOCK_TAG);
  }

  // 0=URL, 1=Name, 2=Icon
  drawMenu(data) {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    this.clearMenu();
    this.bookmarkRows(data).forEach(cols => {
      // rows written before the icon column exists just have two columns
      const icon = cols.length > 2 ? cols[2] : "";
      menu.appendChild(this.createItem(cols[0], cols[1].trim(), icon));
    });
    // remember the copy this was drawn from, so an edit can tell whether the
    // file moved on underneath it
    this.loadedBody = this.buildBody();
    this.menuDrawn = true; // the rows on-screen now match the file
    menu.addEventListener('pointerdown', this.listeners.pointerdown);
    menu.addEventListener('pointermove', this.listeners.pointermove);
    menu.addEventListener('pointerup', this.listeners.pointerup);
    menu.addEventListener('pointercancel', this.listeners.pointercancel);
  }

  clearMenu() {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    // the rows are about to go: nothing may be read back out of them until
    // drawMenu() has put a known-good set up again
    this.menuDrawn = false;
    menu.removeEventListener('pointerdown', this.listeners.pointerdown);
    menu.removeEventListener('pointermove', this.listeners.pointermove);
    menu.removeEventListener('pointerup', this.listeners.pointerup);
    menu.removeEventListener('pointercancel', this.listeners.pointercancel);
    this.items().forEach(item => item.remove());
  }

  createItem(url, name, icon) {
    const item = document.createElement('a');
    item.classList.add(ITEM_CLASS);
    // drag handle, same grip the lists window uses -- hidden outright while
    // another user is editing, so nothing can be moved
    item.appendChild(dragGripElement(this.readOnly === true));
    const label = document.createElement('span');
    label.classList.add('bookmarked__label');
    label.innerText = name;
    item.appendChild(label);
    // the site's favicon, which is also the edit button for this row --
    // the info icon stands in until a lookup fills one in
    const editIcon = document.createElement('span');
    editIcon.classList.add(EDIT_ICON_CLASS);
    // the icon itself lives in its own holder, so redrawing it leaves the
    // spinner that rings it alone
    const glyph = document.createElement('span');
    glyph.classList.add(GLYPH_CLASS);
    editIcon.appendChild(glyph);
    // spinner ring, turning around the icon while the edit window is opening
    const spinner = document.createElement('span');
    spinner.classList.add(SPINNER_CLASS);
    editIcon.appendChild(spinner);
    item.appendChild(editIcon);
    // store URL on the element, as the menu always has
    Object.defineProperty(item, "url", {
      enumerable: false,
      writable: true,
      value: url
    });
    // the icon travels with the row the same way
    Object.defineProperty(item, "icon", {
      enumerable: false,
      writable: true,
      value: ""
    });
    this.setItemIcon(item, icon);
    item.addEventListener("click", (event) => this.clickItem(item, event));
    return item;
  }

  // The menu is only really open while this window is driving it. The
  // container itself is drawn by main.js and outlives close(), so testing for
  // the element alone says nothing about whether there are rows in it.
  menuIsOpen() {
    return this.state !== BKM_INACTIVE && this.menuElem() !== null;
  }

  // What to write for the bookmark rows. The DOM is only worth reading back
  // while a drawn set of rows is standing in it; once the menu has been torn
  // down there are no rows left, and writing that out would erase the file.
  // The copy the menu was drawn from goes back instead, which is what the
  // server already holds -- so the write is a no-op rather than a wipe.
  // A menu that is still up and genuinely empty (the last bookmark was just
  // deleted) keeps menuDrawn, so an intentional empty file still writes.
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
      const url = item.url === null || item.url === undefined ? "" : String(item.url);
      // build output file
      _file += `${url}|${this.itemName(item)}|${item.icon}\n`;
    });
    return _file;
  }

  //// item text ////

  labelElem(item) {
    return item.querySelector('.bookmarked__label');
  }

  itemName(item) {
    const label = this.labelElem(item);
    return label ? label.innerText : item.innerText;
  }

  setItemName(item, name) {
    const label = this.labelElem(item);
    if (label) {
      label.innerText = name;
    }
  }

  //// item icon ////

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
    if (on === true && item) {
      const holder = item.querySelector(`.${EDIT_ICON_CLASS}`);
      if (holder) {
        holder.classList.add(ROW_LOADING_CLASS);
      }
    }
  }

  clearRowLoading() {
    const menu = this.menuElem();
    if (menu) {
      menu.querySelectorAll(`.${ROW_LOADING_CLASS}`)
        .forEach(holder => holder.classList.remove(ROW_LOADING_CLASS));
    }
  }

  // The add button reports on itself the same way a row does. A row hangs its
  // spinner on the favicon that was tapped; the add button has no row yet, so
  // it rings the button itself while the edit lock is being taken.
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
    // draw menu container with search icon + text input box
    const searchMenuItem = document.createElement("a");
    searchMenuItem.id = SEARCH_ID;
    searchMenuItem.classList.add("nohide");
    searchMenuItem.innerHTML = html`
      <input type="text" id="${SEARCH_BOX_ID}" class="nohide" placeholder="Search here..." autocorrect="off" autocapitalize="none">
    `;
    // always the first row in the menu
    menu.insertBefore(searchMenuItem, menu.firstChild);
    const searchBox = document.getElementById(SEARCH_BOX_ID);
    // focus keyboard on search text box
    searchBox.focus();
    // filter list as the user types
    searchMenuItem.addEventListener("input", () => {
      this.filterItems(searchBox.value);
    });
  }

  filterItems(termIn) {
    const searchTerm = termIn.toLowerCase();
    this.items().forEach(item => {
      const itemText = this.itemName(item).toLowerCase();
      item.style.display = itemText.includes(searchTerm) ? "" : "none";
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
  async beginLockedEdit() {
    if (this.lock.owned === true) {
      return true; // already ours, an edit window is open
    }
    const { text, busy } = await this.lock.check();
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
    if (!await this.lock.claim(text)) {
      this.lockedElsewhere(text);
      return false;
    }
    // Two windows can claim in the same moment, so the claim still has to be
    // confirmed -- but that waits on a settling delay, and holding the edit
    // window back for it is a visible stall. It runs behind the window
    // instead: the only thing written before it lands is the claim itself
    // (the lock row over an unchanged body), and settleClaim() below holds
    // every later write until the answer is in. A window that loses is shut
    // again by lockLost().
    this.confirming = this.lock.confirm();
    this.confirming.finally(() => { this.confirming = null; });
    this.readOnly = false;
    return true;
  }

  // waits for a claim that is still being confirmed, so nothing is written
  // while it is still unknown whether this window holds the lock
  async settleClaim() {
    if (this.confirming) {
      await this.confirming;
    }
  }

  // Writes the menu out and hands the lock back in the same request. The rows
  // are only trusted while a drawn menu is still on screen; if it has been
  // torn down the copy the menu was drawn from goes back instead, so a
  // teardown can never write an empty file over somebody's bookmarks.
  //
  // Only ever one release at a time. close() and the edit window closing can
  // both call this in the same moment, and two of them in flight would each
  // capture their own body -- the one that resumed last winning, which after
  // a teardown is the one holding no rows at all.
  releaseLock() {
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
    if (this.lock.owned !== true) {
      return true;
    }
    // Read the rows out before anything is awaited: close() tears the menu
    // down and clears the rows in the same tick this is called from, so
    // waiting first would leave nothing to write.
    const body = this.safeBody();
    await this.settleClaim();
    if (this.lock.owned !== true) {
      return true; // the claim was lost, and lockLost() has reported it
    }
    this.lock.abandon(); // stops the heartbeat before the closing write
    const sent = await writeFile(FILE_NAME, this.lock.line(false) + body);
    if (sent) {
      this.loadedBody = body;
    }
    return sent;
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

  // a competing window won the lock in the moment we both claimed it
  lockLost(text) {
    this.closePrompt();
    this.discardPendingAdd();
    if (this.state !== BKM_INACTIVE) {
      this.state = BKM_OPEN_MODE;
    }
    this.lockedElsewhere(text);
  }

  // the file body as the server has it, in the same shape buildBody() writes
  bodyFromText(text) {
    let body = "";
    this.bookmarkRows(text).forEach(cols => {
      const icon = cols.length > 2 ? cols[2] : "";
      body += `${cols[0]}|${cols[1].trim()}|${icon}\n`;
    });
    return body;
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
      // create placeholder menu item
      const item = this.createItem("about:blank", "New Bookmark", "");
      // highlight selected item
      item.classList.add(SELECTED_CLASS);
      // insert item at the top of the bookmarks, below the search box
      menu.insertBefore(item, this.firstItem());
      this.pendingAdd = item;
      this.state = BKM_EDIT_MODE;
      // add new window
      this.drawPrompt(true, null, null, item);
    } finally {
      // the window is up, or the attempt is over
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
        // the menu was closed, or the row redrawn, while the lock was being
        // taken
        await this.releaseLock();
        return;
      }
      this.discardPendingAdd();
      this.closePrompt();
      this.state = BKM_EDIT_MODE;
      item.classList.add(SELECTED_CLASS); // highlight selected item
      this.drawPrompt(false, item.url, this.itemName(item), item);
    } finally {
      // the window is up, or the attempt is over
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
    return items.length > 0 ? items[0] : null;
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
      // open URL in new tab
      window.open(url, "_blank");
    }
  }

  //// add / edit window ////

  // close add / edit window
  closePrompt() {
    this.clearLookupTimer();
    this.lookupBtn = null;
    // A full-document querySelectorAll + inline style write per node used to
    // run here to re-enable clicking outside the window. Nothing sets
    // pointerEvents any more, so it was clearing something that is never set:
    // the orphaned half of a mechanism that has since gone.
    // un-highlight selected item
    this.items().forEach(item => item.classList.remove(SELECTED_CLASS));
    const favPrompt = document.getElementById(PROMPT_ID);
    if (favPrompt) {
      favPrompt.remove();
    }
  }

  drawPrompt(add, url, name, item) {
    // the window edits one icon at a time; a lookup replaces it, saving
    // writes it back onto the row
    this.promptIcon = item ? validIcon(item.icon) : "";
    // create window: banner text with the icon preview beside it, name box,
    // URL box, six action buttons
    const editFavPrompt = document.createElement("div");
    editFavPrompt.className = "editFav__win";
    editFavPrompt.id = PROMPT_ID;
    editFavPrompt.classList.add("prompt_win");
    const bannerText = add === true ? "Add Bookmark" : "Edit Bookmark";
    editFavPrompt.innerHTML = html`
      <div class="editFav__toptext">
        <span class="editFav__toplabel">${bannerText}</span>
        <span class="editFav__iconpreview" id="${PREVIEW_ID}"></span>
      </div>
      <input type="text" id="editFav__namebox" class="editFav__textbox"
             autocorrect="off" autocapitalize="none">
      <input type="text" id="editFav__urlbox" class="editFav__textbox"
             autocorrect="off" autocapitalize="none">
      <button class="editFav__button button fad fa-arrow-up" type="button"></button>
      <button class="editFav__button button fad fa-search" type="button"></button>
      <button class="editFav__button button fad fa-arrow-down" type="button"></button>
      <button class="editFav__button button fad fa-save" type="button"></button>
      <button class="editFav__button button fad fa-times-octagon" type="button"></button>
      <button class="editFav__button button fad fa-trash-alt" type="button"></button>
    `;
    // element references (buttons in DOM order: row1 up/lookup, row2 down/save,
    // row3 cancel/delete)
    const editFavText = editFavPrompt.querySelector(".editFav__toplabel");
    const nameBox = editFavPrompt.querySelector("#editFav__namebox");
    const urlBox = editFavPrompt.querySelector("#editFav__urlbox");
    // reported on directly by lookupURL()
    this.lookupBtn = null;
    const [editFavUpBtn, editFavLookupBtn, editFavDownBtn,
           editFavSaveBtn, editFavCancelBtn, editFavDeleteBtn] =
      editFavPrompt.querySelectorAll("button");
    this.lookupBtn = editFavLookupBtn;
    // set values via properties (null coerces to "" exactly as before)
    nameBox.value = name;
    urlBox.value = url;
    if (add === true) { // add new mode placeholders
      nameBox.placeholder = "Enter Name";
      urlBox.placeholder = "Enter URL";
    }
    // display window on page
    document.body.appendChild(editFavPrompt);
    // favicon preview, sitting to the right of the banner text. Clicking it
    // drops the icon; the row keeps its old one until the window is saved.
    editFavPrompt.querySelector(`#${PREVIEW_ID}`)
      .addEventListener("click", () => {
        if (this.promptIcon !== "") {
          this.promptIcon = "";
          this.drawIconPreview();
        }
      });
    this.drawIconPreview();
    // handle button actions
    editFavPrompt.addEventListener("click", (btn_event) => {
      if (btn_event.target.tagName !== "BUTTON") { return; }
      // move up button action
      if (btn_event.target === editFavUpBtn) {
        this.shiftItemUp(item);
      }
      // move down button action
      if (btn_event.target === editFavDownBtn) {
        this.shiftItemDown(item);
      }
      // cancel button action -- closes the window, not the bookmarks menu
      if (btn_event.target === editFavCancelBtn) {
        this.endEditMode();
      }
      // lookup URL action
      if (btn_event.target === editFavLookupBtn) {
        this.lookupURL();
      }
      // save button action
      if (btn_event.target === editFavSaveBtn) {
        // do not allow empty URL or name
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
          return;
        }
        // update changed values
        if (item) {
          // save name to menu object
          const _boxname = nameBox.value.replaceAll("|", "-");
          // name length limiter
          if (_boxname.length >= MAX_NAME_LENGTH) {
            this.setItemName(item, `${_boxname.substring(0, MAX_NAME_LENGTH)}...`);
          } else {
            this.setItemName(item, _boxname);
          }
          // save URL to menu object
          const _boxurl = addHTTPtoURL(urlBox.value.replaceAll("|", "-"));
          item.url = _boxurl;
          urlBox.value = _boxurl;
          // save the looked-up icon to the menu object
          this.setItemIcon(item, this.promptIcon);
          editFavText.innerHTML = "Changes Saved";
        }
        // the new row is a real bookmark now
        if (this.pendingAdd === item) {
          this.pendingAdd = null;
        }
        // closing writes the menu out and hands the lock back in one request
        this.endEditMode();
      }
      // delete button action
      if (btn_event.target === editFavDeleteBtn) {
        if (this.pendingAdd === item) {
          this.pendingAdd = null;
        }
        item.remove();
        // closing writes the menu out and hands the lock back in one request
        this.endEditMode();
      }
    });
  }

  //// move buttons ////

  shiftItemUp(item) {
    if (item && item.previousElementSibling) {
      item.parentNode.insertBefore(item, item.previousElementSibling);
    }
  }

  shiftItemDown(item) {
    if (item && item.nextElementSibling) {
      item.parentNode.insertBefore(item.nextElementSibling, item);
    }
  }

  //// URL lookup ////

  // redraws the preview beside the banner text from this.promptIcon. With
  // no icon to show the slot is left empty rather than standing in with the
  // info icon; the text boxes below are not affected either way.
  drawIconPreview() {
    const preview = document.getElementById(PREVIEW_ID);
    if (preview) {
      const empty = this.promptIcon === "";
      preview.classList.toggle(PREVIEW_EMPTY_CLASS, empty);
      preview.innerHTML = empty ? "" : iconMarkup(this.promptIcon);
    }
  }

  // The button reports on itself: its icon gives way to a word while the
  // lookup runs, and the name box is only ever written on success.
  async lookupURL() {
    const urlBoxElem = document.getElementById("editFav__urlbox");
    const nameBoxElem = document.getElementById("editFav__namebox");
    if (!urlBoxElem || !nameBoxElem) {
      return;
    }
    this.setLookupBusy();
    // read URL box
    let urlin = urlBoxElem.value;
    if (urlin === null || urlin == "") {
      // read from clipboard if URL empty
      try {
        urlin = await navigator.clipboard.readText();
      } catch (error) {
        // the browser refused the clipboard, or there is nothing on it
        this.lookupFailed(LOOKUP_NO_CLIPBOARD);
        return;
      }
    }
    // add HTTPs to URL
    const url = addHTTPtoURL(urlin);
    if (url === "") {
      this.lookupFailed(LOOKUP_NO_URL);
      return;
    }
    urlBoxElem.value = url;
    try {
      // search for the URLs title and favicon
      const data = await sendCmd('main', 'sitelookup', base64URLSafeEncode(url));
      if (data === null || data === undefined) {
        // sendCmd() swallows the reason, so the request itself failed
        this.lookupFailed(LOOKUP_NO_ANSWER);
        return;
      }
      // the API answers { title, icon }; a plain string is still accepted
      // so an older backend keeps working
      const title = (typeof data === 'object') ? data.title : data;
      const icon = (typeof data === 'object') ? data.icon : "";
      // remove invalid characters; a title of nothing but spaces is no title
      const siteName = (cleanString(
        title === null || title === undefined ? "" : String(title)) || "").trim();
      if (siteName === "") {
        // nothing usable came back -- the boxes are left exactly as they were
        this.lookupFailed(LOOKUP_NO_TITLE);
        return;
      }
      if (LOOKUP_FAIL_REPLY.test(siteName)) {
        // the answer is the backend reporting a failure, not a title: its own
        // wording is the reason, and nothing is written
        this.lookupFailed(siteName);
        return;
      }
      // the only path that writes to the name box
      nameBoxElem.value = siteName;
      // show the fetched icon, saving writes it to the row
      this.promptIcon = validIcon(icon);
      this.drawIconPreview();
      this.setLookupIcon();
    } catch (error) {
      // anything the lookup itself threw -- nothing is written
      this.lookupFailed(String(error && error.message ? error.message : error));
    }
  }

  //// lookup button state ////

  // Swaps the magnifier for a turning refresh icon while the lookup runs.
  // The icon is a child element, not the button's own glyph, so the rotation
  // turns the icon rather than the whole button.
  setLookupBusy() {
    this.clearLookupTimer();
    if (!this.lookupBtn) {
      return;
    }
    this.lookupBtn.innerHTML = "";
    this.lookupBtn.classList.remove('fad', LOOKUP_ICON_CLASS,
      LOOKUP_FAIL_ICON_CLASS, LOOKUP_FAIL_CLASS);
    this.lookupBtn.classList.add(LOOKUP_BUSY_CLASS);
    const spinner = document.createElement('i');
    spinner.className = `${LOOKUP_SPIN_ICON} ${LOOKUP_SPIN_CLASS}`;
    this.lookupBtn.appendChild(spinner);
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
    const stop = /[.!?]$/.test(detail) ? "" : ".";
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
