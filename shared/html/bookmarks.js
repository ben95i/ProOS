// Automate - bookmarks menu: the file, drop-down, search, add / edit window and drag
// re-order. bookmarks.save holds URL|NAME|ICON per line, in menu order; ICON is a URL-safe
// base64 favicon, and older rows have two columns. Editing is single-user on a FileLock,
// taken only while an edit window is open or a row is moved.

import { html, cleanString, fileRows, boundedText, base64URLSafeEncode,
  base64URLSafeToStandard, sendCmd, dragGripElement, DragSort, FileLock,
  readFile, writeFile, sendFileBeacon, LOCK_TAG, searchRowElement, searchTerm,
  searchMatches, makeElement, singleFlight, reportFileFailure } from './utils.js';
import { showPopup, reportException } from './error.js';
import { removeDropdowns } from './dropdowns.js';

const FILE_NAME = 'bookmarks.save';
const LOCK_FILE = 'bookmarks-lock.save';
const MENU_ID = 'bookmarks';
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
// the lookup button reports on itself
const LOOKUP_ICON_CLASS = 'fa-search';
const LOOKUP_BUSY_CLASS = 'editFav__button--busy';
const LOOKUP_FAIL_ICON_CLASS = 'fa-exclamation-triangle';
const LOOKUP_FAIL_CLASS = 'editFav__button--failed';
const LOOKUP_ERROR_MS = 3000; // how long the fail icon stays
// why a lookup gave up, for the popup
const LOOKUP_NO_URL = 'no URL to look up';
const LOOKUP_NO_CLIPBOARD = 'no URL entered, and the clipboard could not be read';
const LOOKUP_NO_ANSWER = 'no answer from the server';
const LOOKUP_NO_TITLE = 'the site returned no title';
const PREVIEW_ID = 'editFav__iconpreview';
// the add / edit window's buttons and their icons
const EDIT_BTN_IDS = {
  up: 'editFav__btn-up',
  lookup: 'editFav__btn-lookup',
  down: 'editFav__btn-down',
  save: 'editFav__btn-save',
  cancel: 'editFav__btn-cancel',
  remove: 'editFav__btn-delete'
};
const EDIT_BTN_ICONS = {
  up: 'fa-arrow-up',
  lookup: 'fa-search',
  down: 'fa-arrow-down',
  save: 'fa-save',
  cancel: 'fa-times-octagon',
  remove: 'fa-trash-alt'
};
const PREVIEW_EMPTY_CLASS = 'editFav__iconpreview--empty';
const SELECTED_CLASS = 'dd-selected';
const MAX_NAME_LENGTH = 30;
const LOCKED_MSG = "read/only, locked by another user";
const INFO_ICON_HTML = '<i class="fad fa-info-circle"></i>';
const ICON_CHARS = /^[A-Za-z0-9_@-]*$/;
const MAX_ICON_LENGTH = 8192;
// menu states
const BKM_INACTIVE = 0;
const BKM_OPEN_MODE = 1;
const BKM_EDIT_MODE = 2;

// adds the https:// prefix when the URL has no scheme
function addHTTPtoURL(linkin) {
  if (linkin === "" || linkin === null) {
    return "";
  }
  const lower = linkin.toLowerCase();
  if (lower.startsWith('http://', 0) || lower.startsWith('https://', 0)) {
    return linkin;
  }
  return "https://" + linkin;
}

// A saved icon goes straight into an <img src>, so anything but URL-safe base64 is dropped.
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

// the stored bytes name their format: PNG, or JPEG for icons saved before PNG
function iconMimeType(icon) {
  if (icon.startsWith('iVBORw0KGgo')) {
    return 'image/png';
  }
  return 'image/jpeg';
}

// the row's icon markup, or the info glyph with no icon
function iconMarkup(icon) {
  if (icon === "") {
    return INFO_ICON_HTML;
  }
  return html`<img class="${FAV_ICON_CLASS}" alt="" width="24" height="24"
    src="data:${iconMimeType(icon)};base64,${base64URLSafeToStandard(icon)}">`;
}

export default class Bookmarks {

  // set up by toggle()
  constructor() {
    this.state = BKM_INACTIVE;
    this.pendingAdd = null;  // placeholder row waiting on the add window
    this.promptIcon = "";    // icon held by the open add / edit window
    this.readOnly = false;   // another user held the lock when last looked
    this.loadedBody = "";    // the file as it was when the menu was drawn
    this.menuDrawn = false;  // rows are drawn and safe to read back out
    this.releasing = null;   // release in flight, so two cannot race
    this.claiming = null;    // claim in flight, so two cannot race
    this.loading = false;    // the file is on its way in
    this.lookupBtn = null;   // site lookup button of the open window
    this.lookupTimer = null; // fail icon waiting to turn back
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

  // is the add / edit window driving the menu
  isEditMode() {
    return this.state === BKM_EDIT_MODE;
  }

  // the menu's container
  menuElem() {
    return document.getElementById(MENU_ID);
  }

  // the bookmark rows
  items() {
    const menu = this.menuElem();
    if (!menu) {
      return [];
    }
    return [...menu.getElementsByClassName(ITEM_CLASS)];
  }

  // star button: draws the menu, or hides it when open
  toggle() {
    if (this.state !== BKM_INACTIVE) {
      removeDropdowns();
      return;
    }
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    removeDropdowns();
    menu.style.display = 'block';
    this.state = BKM_OPEN_MODE;
    this.renderButtons();
    this.load();
  }

  // called by removeDropdowns()
  close() {
    this.discardPendingAdd();
    // the lock goes back before the rows it would write are torn down
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
    // loadedBody stays: a release still settling writes it instead of the torn-down rows
    this.state = BKM_INACTIVE;
  }

  // the add button, drawn next to the star while the menu is open
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

  // Reads the bookmarks and the lock together.
  async readBoth() {
    const [text, lockText] = await Promise.all([
      readFile(FILE_NAME),
      this.lock.read()
    ]);
    return { text: text, lockText: lockText, busy: this.lock.busy(lockText) };
  }

  // first draw: reads the file behind a loading row
  async load() {
    this.showLoading();
    const { text, busy } = await this.readBoth();
    this.loading = false;
    if (this.state === BKM_INACTIVE) {
      return; // closed while loading
    }
    if (text === null) {
      this.removeLoading();
      reportFileFailure('loading the bookmarks');
      return;
    }
    this.readOnly = busy;
    this.redraw(text);
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

  // removes the loading row
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
    this.redraw(text);
    return true;
  }

  // Bookmark rows only; the old lock row and anything short is left out.
  // 'https://x|Name|icon' -> ['https://x', 'Name', 'icon']
  bookmarkRows(data) {
    return fileRows(data)
      .map(line => line.split("|"))
      .filter(cols => cols.length >= 2 && cols[0] !== LOCK_TAG);
  }

  // draws the menu from file text and puts the search box back on top
  redraw(text) {
    this.drawMenu(text);
    this.showSearch();
  }

  // a row's icon column; older rows have none
  rowIcon(cols) {
    if (cols.length > 2) {
      return cols[2];
    }
    return "";
  }

  // draws the rows: 0=URL, 1=Name, 2=Icon
  drawMenu(data) {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    this.clearMenu();
    this.bookmarkRows(data).forEach(cols => {
      menu.appendChild(this.createItem(cols[0], cols[1].trim(), this.rowIcon(cols)));
    });
    // the copy this was drawn from, so an edit can tell if the file moved on
    this.loadedBody = this.buildBody();
    this.menuDrawn = true;
    Object.keys(this.listeners).forEach(name => {
      menu.addEventListener(name, this.listeners[name]);
    });
  }

  // takes the rows and their listeners down
  clearMenu() {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    // nothing may be read back out of the rows until drawMenu() puts a set up again
    this.menuDrawn = false;
    Object.keys(this.listeners).forEach(name => {
      menu.removeEventListener(name, this.listeners[name]);
    });
    this.items().forEach(item => item.remove());
  }

  // one bookmark row
  createItem(url, name, icon) {
    const item = makeElement('a', ITEM_CLASS);
    // drag handle, hidden while another user is editing
    item.appendChild(dragGripElement(this.readOnly === true));
    const label = makeElement('span', LABEL_CLASS);
    label.innerText = name;
    item.appendChild(label);
    // the favicon, also the row's edit button; its own holder leaves the spinner alone
    const editIcon = makeElement('span', EDIT_ICON_CLASS);
    editIcon.appendChild(makeElement('span', GLYPH_CLASS));
    editIcon.appendChild(makeElement('span', SPINNER_CLASS));
    item.appendChild(editIcon);
    item.url = url;
    item.icon = "";
    this.setItemIcon(item, icon);
    item.addEventListener("click", (event) => this.clickItem(item, event));
    return item;
  }

  // is the menu open on this window? The container outlives close().
  menuIsOpen() {
    return this.state !== BKM_INACTIVE && this.menuElem() !== null;
  }

  // The body to write. Rows are read back only while a drawn set stands; after a teardown
  // the drawn-from copy goes back, so the write is a no-op rather than a wipe.
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
      _file += `${item.url}|${this.itemName(item)}|${item.icon}\n`;
    });
    return _file;
  }

  // file text in the shape buildBody() writes
  bodyFromText(text) {
    let body = "";
    this.bookmarkRows(text).forEach(cols => {
      body += `${cols[0]}|${cols[1].trim()}|${this.rowIcon(cols)}\n`;
    });
    return body;
  }

  //// item text and icon ////

  // the name's element
  labelElem(item) {
    return item.querySelector(`.${LABEL_CLASS}`);
  }

  // a row's name
  itemName(item) {
    return this.labelElem(item).innerText;
  }

  // sets a row's name
  setItemName(item, name) {
    this.labelElem(item).innerText = name;
  }

  // stores the icon on the row and redraws its edit button
  setItemIcon(item, icon) {
    item.icon = validIcon(icon);
    item.querySelector(`.${GLYPH_CLASS}`).innerHTML = iconMarkup(item.icon);
  }

  // rings the icon of a row whose window is on its way
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

  // stops any row's ring
  clearRowLoading() {
    const menu = this.menuElem();
    if (menu) {
      menu.querySelectorAll(`.${ROW_LOADING_CLASS}`)
        .forEach(holder => holder.classList.remove(ROW_LOADING_CLASS));
    }
  }

  // the add button, which rings itself while the lock is taken
  addBtnElem() {
    return document.getElementById(ADD_BTN_ID);
  }

  // rings the add button
  setAddLoading(on) {
    const btn = this.addBtnElem();
    if (btn) { // close() takes the whole button away
      btn.classList.toggle(ADD_LOADING_CLASS, on === true);
    }
  }

  // the ring doubles as a guard: a second tap would start a second claim
  addIsLoading() {
    const btn = this.addBtnElem();
    return btn !== null && btn.classList.contains(ADD_LOADING_CLASS);
  }

  //// search ////

  // draws the search box as the menu's first row
  showSearch() {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    // a redraw calls this again, and never leaves a second box
    this.removeSearch();
    this.removeLoading();
    // "nohide" on every part, so clearing a search does not shut the menu
    const searchMenuItem = searchRowElement({
      tag: 'a',
      id: SEARCH_ID,
      partClass: 'nohide',
      boxId: SEARCH_BOX_ID,
      onSearch: (value) => this.filterItems(value)
    });
    menu.insertBefore(searchMenuItem, menu.firstChild);
    const searchBox = document.getElementById(SEARCH_BOX_ID);
    if (searchBox) {
      searchBox.focus();
    }
  }

  // hides the rows that do not match
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

  // removes the search box
  removeSearch() {
    const search = document.getElementById(SEARCH_ID);
    if (search) search.remove();
  }

  //// edit lock ////

  // Takes the lock before anything that could write the file: re-reads it, and if it moved
  // on since this menu was drawn, redraws and backs off. One claim at a time, as the add
  // button, a row icon and a drop all reach here without being awaited.
  beginLockedEdit() {
    return singleFlight(this, 'claiming', () => this.runBeginLockedEdit());
  }

  // the claim itself
  async runBeginLockedEdit() {
    if (this.lock.owned === true) {
      return true; // already ours, an edit window is open
    }
    const { text, lockText, busy } = await this.readBoth();
    if (!this.menuIsOpen()) {
      return false; // closed during the read; claim nothing
    }
    if (text === null) {
      reportFileFailure('opening the bookmarks for editing');
      return false;
    }
    if (busy) {
      this.lockedElsewhere(text);
      return false;
    }
    if (this.bodyFromText(text) !== this.loadedBody) {
      // the file changed since this menu was drawn
      this.readOnly = false;
      this.redraw(text);
      showPopup("[bookmarks] reloaded, changed by another user.");
      return false;
    }
    // a one row write to the lock file; the bookmarks are untouched until the edit ends
    if (!await this.lock.claim(lockText)) {
      this.lockedElsewhere(text);
      return false;
    }
    // Two windows can claim at once, so the claim is confirmed behind the window: the
    // lock's settle() holds later writes, and a window that loses is shut by lockLost().
    this.lock.confirm();
    this.readOnly = false;
    return true;
  }

  // Writes the menu out and hands the lock back. One release at a time, as close() and the
  // edit window closing can both call this.
  releaseLock() {
    return singleFlight(this, 'releasing', () => this.runRelease());
  }

  // the release itself
  async runRelease() {
    if (this.lock.owned !== true) {
      return true;
    }
    // read before anything awaits: close() tears the menu down in this same tick
    const body = this.safeBody();
    await this.lock.settle();
    if (this.lock.owned !== true) {
      return true; // the claim was lost, and lockLost() reported it
    }
    // the bookmarks first, while the lock is held
    const sent = await writeFile(FILE_NAME, body);
    if (sent) {
      this.loadedBody = body;
    }
    // freed even if that write failed: the rows are still in the menu
    await this.lock.release();
    return sent;
  }

  // As the page goes away: the rows and the lock row as beacons, as an ordinary write
  // would be cancelled. Only while the lock is held.
  unloadBeacon() {
    if (this.lock.owned !== true) {
      return false;
    }
    // the bookmarks first, as runRelease() writes them
    const body = this.safeBody();
    const handed = sendFileBeacon(FILE_NAME, body);
    if (handed === true) {
      this.loadedBody = body;
    }
    // then the lock row, whether or not the rows got away
    if (sendFileBeacon(LOCK_FILE, this.lock.line(false)) === true) {
      this.lock.abandon();
    }
    return handed;
  }

  // releases the lock, reporting a failed write
  async endLockedEdit() {
    if (!await this.releaseLock()) {
      reportFileFailure('saving the bookmarks');
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
      this.redraw(text);
    }
  }

  // Another window won the lock as both claimed it: redraws from what it has.
  async lockLost() {
    this.closePrompt();
    this.discardPendingAdd();
    if (this.state !== BKM_INACTIVE) {
      this.state = BKM_OPEN_MODE;
    }
    const text = await readFile(FILE_NAME);
    this.lockedElsewhere(text);
  }

  // a row was dragged: takes the lock, writes the new order, hands it back
  async dropMove() {
    if (!await this.beginLockedEdit()) {
      // could not take it: back to how the file has it
      await this.reload();
      return;
    }
    await this.endLockedEdit();
  }

  //// add / edit mode ////

  // the add button: a placeholder row and its window, once the lock is taken
  async addBookmark() {
    const menu = this.menuElem();
    if (!menu || this.loading === true || this.addIsLoading()) {
      return;
    }
    // the button turns from the tap until the window is up
    this.setAddLoading(true);
    try {
      if (!await this.beginLockedEdit()) {
        return;
      }
      if (!this.menuIsOpen()) {
        // closed while the lock was taken: hands it back and draws nothing
        await this.releaseLock();
        return;
      }
      this.discardPendingAdd();
      this.closePrompt();
      const item = this.createItem("about:blank", "New Bookmark", "");
      item.classList.add(SELECTED_CLASS);
      // at the top of the bookmarks, below the search box
      menu.insertBefore(item, this.firstItem());
      this.pendingAdd = item;
      this.state = BKM_EDIT_MODE;
      this.drawPrompt(true, null, null, item);
    } finally {
      this.setAddLoading(false);
    }
  }

  // opens the window on a row, from its icon or any row while the window is up
  async editItem(item) {
    this.setRowLoading(item, true);
    try {
      if (!await this.beginLockedEdit()) {
        return;
      }
      if (!this.menuIsOpen() || !item.isConnected) {
        // closed, or the row redrawn, while the lock was taken
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

  // closes the window, writes the menu and hands the lock back, leaving the menu up
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

  // the first bookmark row, so inserts land under the search box
  firstItem() {
    const items = this.items();
    if (items.length > 0) {
      return items[0];
    }
    return null;
  }

  // a row pressed: the favicon or an open window edits it, otherwise it opens its link
  clickItem(item, event) {
    // the grip is for dragging only
    if (this.dragSort.isDragging() || event.target.closest('.drag-grip')) {
      return;
    }
    if (event.target.closest(`.${EDIT_ICON_CLASS}`) || this.state === BKM_EDIT_MODE) {
      this.editItem(item);
      return;
    }
    const url = item.url;
    if (!(url == "" || url == "about:blank")) {
      window.open(url, "_blank");
    }
  }

  //// add / edit window ////

  // closes the window
  closePrompt() {
    this.clearLookupTimer();
    this.lookupBtn = null;
    this.items().forEach(item => item.classList.remove(SELECTED_CLASS));
    const favPrompt = document.getElementById(PROMPT_ID);
    if (favPrompt) {
      favPrompt.remove();
    }
  }

  // Writes the window's values back onto the row; false when a box was empty, with the
  // reason in its placeholder.
  savePrompt(item, nameBox, urlBox) {
    let stopsave = false;
    if (nameBox.value === "") {
      nameBox.placeholder = "Name cannot be empty";
      stopsave = true;
    }
    if (urlBox.value === "") {
      urlBox.placeholder = "URL cannot be empty";
      stopsave = true;
    }
    if (stopsave === true) {
      return false;
    }
    this.setItemName(item, boundedText(nameBox.value.replaceAll("|", "-"), MAX_NAME_LENGTH));
    const _boxurl = addHTTPtoURL(urlBox.value.replaceAll("|", "-"));
    item.url = _boxurl;
    urlBox.value = _boxurl;
    this.setItemIcon(item, this.promptIcon);
    return true;
  }

  // draws the add / edit window for a row: banner and icon preview, name and URL boxes, six buttons
  drawPrompt(add, url, name, item) {
    // saving writes this back onto the row; a lookup replaces it
    this.promptIcon = validIcon(item.icon);
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
      ${Object.keys(EDIT_BTN_IDS).map(key => `
      <button class="editFav__button button fad ${EDIT_BTN_ICONS[key]}" type="button"
              id="${EDIT_BTN_IDS[key]}"></button>`).join('')}
    `;
    const editFavText = editFavPrompt.querySelector(".editFav__toplabel");
    const nameBox = editFavPrompt.querySelector("#editFav__namebox");
    const urlBox = editFavPrompt.querySelector("#editFav__urlbox");
    this.lookupBtn = editFavPrompt.querySelector(`#${EDIT_BTN_IDS.lookup}`);
    // set as properties, where null becomes ""
    nameBox.value = name;
    urlBox.value = url;
    if (add === true) {
      nameBox.placeholder = "Enter Name";
      urlBox.placeholder = "Enter URL";
    }
    document.body.appendChild(editFavPrompt);
    // the preview beside the banner: a click drops the icon, the row keeping its old one
    // until saved
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
      switch (btn_event.target.id) {
        case EDIT_BTN_IDS.up:
          this.shiftItem(item, true);
          break;
        case EDIT_BTN_IDS.down:
          this.shiftItem(item, false);
          break;
        case EDIT_BTN_IDS.cancel:
          // closes the window, not the bookmarks menu
          this.endEditMode();
          break;
        case EDIT_BTN_IDS.lookup:
          this.lookupURL();
          break;
        case EDIT_BTN_IDS.save:
          if (!this.savePrompt(item, nameBox, urlBox)) {
            return;
          }
          editFavText.innerHTML = "Changes Saved";
          // the new row is a real bookmark now
          if (this.pendingAdd === item) {
            this.pendingAdd = null;
          }
          this.endEditMode();
          break;
        case EDIT_BTN_IDS.remove:
          if (this.pendingAdd === item) {
            this.pendingAdd = null;
          }
          item.remove();
          this.endEditMode();
          break;
      }
    });
  }

  // moves a row one place up or down the menu
  shiftItem(item, up) {
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

  // Redraws the preview from this.promptIcon; empty with no icon.
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

  // Looks the URL box's site up, or the clipboard's when empty. The button shows the
  // progress, and the name box is written only on success.
  async lookupURL() {
    const urlBoxElem = document.getElementById("editFav__urlbox");
    const nameBoxElem = document.getElementById("editFav__namebox");
    this.setLookupBusy();
    let urlin = urlBoxElem.value;
    if (urlin === "") {
      // the clipboard when the URL box is empty
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
    const data = await sendCmd('main', 'sitelookup', base64URLSafeEncode(url));
    if (data === null) {
      this.lookupFailed(LOOKUP_NO_ANSWER);
      return;
    }
    // a title of nothing but spaces is no title
    const siteName = (cleanString(data.title) || "").trim();
    if (siteName === "") {
      this.lookupFailed(LOOKUP_NO_TITLE);
      return;
    }
    nameBoxElem.value = siteName;
    this.promptIcon = validIcon(data.icon);
    this.drawIconPreview();
    this.setLookupIcon();
  }

  //// lookup button state ////

  // Swaps the magnifier for a spinner ring (a child, so the button stays still).
  setLookupBusy() {
    this.clearLookupTimer();
    if (!this.lookupBtn) {
      return;
    }
    this.lookupBtn.innerHTML = "";
    this.lookupBtn.classList.remove('fad', LOOKUP_ICON_CLASS,
      LOOKUP_FAIL_ICON_CLASS, LOOKUP_FAIL_CLASS);
    this.lookupBtn.classList.add(LOOKUP_BUSY_CLASS);
    this.lookupBtn.appendChild(makeElement('span', SPINNER_CLASS));
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

  // The button only shows it failed; the reason goes to the popup.
  lookupFailed(reason) {
    // the reason may already end in punctuation
    const detail = String(reason).trim();
    let stop = ".";
    if (/[.!?]$/.test(detail)) {
      stop = "";
    }
    reportException('looking up the bookmark\'s site', `${detail}${stop}`);
    this.setLookupIcon(LOOKUP_FAIL_ICON_CLASS);
    this.lookupTimer = setTimeout(() => {
      this.lookupTimer = null;
      this.setLookupIcon();
    }, LOOKUP_ERROR_MS);
  }

  // stops the fail icon's timer
  clearLookupTimer() {
    if (this.lookupTimer) {
      clearTimeout(this.lookupTimer);
      this.lookupTimer = null;
    }
  }
}
