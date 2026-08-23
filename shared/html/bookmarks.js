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

import { html, showPopup, loadMenu, savePOST, cleanString,
  splitMenuData, base64URLSafeEncode, base64URLSafeToStandard, sendCmd,
  dragGripElement, DragSort } from './utils.js';

const FILE_NAME = 'bookmarks.save';
const MENU_ID = 'bookmarks';         // drop-down container, drawn by main.js
const BUTTONS_ID = 'bookmark-buttons';
const STAR_BTN_ID = 'btn-show-bookmarks';
const SEARCH_ID = 'bookmark__search';
const SEARCH_BOX_ID = 'bookmark__search_txt';
const PROMPT_ID = 'editFav__prompt';
const ITEM_CLASS = 'bookmarked__item';
const EDIT_ICON_CLASS = 'bookmarked__edit';
const FAV_ICON_CLASS = 'bookmarked__favicon';
const PREVIEW_ID = 'editFav__iconpreview';
const PREVIEW_EMPTY_CLASS = 'editFav__iconpreview--empty';
const SELECTED_CLASS = 'dd-selected';
const FILE_TYPE = 'bkmrk';           // column 1 of every row
const MAX_NAME_LENGTH = 42;
// stand-in shown on any row that has no favicon saved
const INFO_ICON_HTML = '<i class="fad fa-info-circle"></i>';
// the alphabet base64URLSafeEncode() produces, nothing else is let through
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
    this.pendingAdd = null; // placeholder row waiting on the add window
    this.promptIcon = "";   // icon held by the open add / edit window
    this.dragSort = new DragSort({
      rowClass: ITEM_CLASS,
      onDrop: () => this.save()
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
    if (this.menuElem()) {
      this.clearMenu();
    }
    const buttons = document.getElementById(BUTTONS_ID);
    if (buttons) {
      buttons.remove();
    }
    this.dragSort.cancel();
    this.removeSearch();
    this.closePrompt();
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
          <a id="btn-add-bookmark">
            <i class="fad fa-folder-plus"></i>
          </a>
        </div>`);
    }
  }

  //// file ////

  async load() {
    const url = location.protocol + "//" + location.hostname
      + "/api/read?file=" + FILE_NAME;
    const data = await loadMenu(url);
    if (this.state === BKM_INACTIVE) {
      return; // menu closed while loading
    }
    if (!data || typeof data !== 'string') {
      showPopup("[bookmarks] no data to parse.");
      return;
    }
    this.drawMenu(data);
    this.showSearch();
  }

  // 0=URL, 1=Name, 2=Icon
  drawMenu(data) {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    this.clearMenu();
    splitMenuData(data).forEach(line => {
      if (line === "") {
        return;
      }
      const cols = line.split("|");
      if (cols.length < 2) {
        return;
      }
      // rows written before the icon column exists just have two columns
      const icon = cols.length > 2 ? cols[2] : "";
      menu.appendChild(this.createItem(cols[0], cols[1].trim(), icon));
    });
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
    menu.removeEventListener('pointerdown', this.listeners.pointerdown);
    menu.removeEventListener('pointermove', this.listeners.pointermove);
    menu.removeEventListener('pointerup', this.listeners.pointerup);
    menu.removeEventListener('pointercancel', this.listeners.pointercancel);
    this.items().forEach(item => item.remove());
  }

  createItem(url, name, icon) {
    const item = document.createElement('a');
    item.classList.add(ITEM_CLASS);
    // drag handle, same grip the lists window uses
    item.appendChild(dragGripElement());
    const label = document.createElement('span');
    label.classList.add('bookmarked__label');
    label.innerText = name;
    item.appendChild(label);
    // the site's favicon, which is also the edit button for this row --
    // the info icon stands in until a lookup fills one in
    const editIcon = document.createElement('span');
    editIcon.classList.add(EDIT_ICON_CLASS);
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

  // reads the menu back out in its current order and transmits it
  save() {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
    let _file = "";
    this.items().forEach(item => {
      const url = Object.values(item.url).join("");
      // build output file
      _file += `${url}|${this.itemName(item)}|${item.icon}\n`;
    });
    // transmit file
    savePOST(FILE_NAME, [_file]);
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
    const holder = item.querySelector(`.${EDIT_ICON_CLASS}`);
    if (holder) {
      holder.innerHTML = iconMarkup(item.icon);
    }
  }

  //// search ////

  showSearch() {
    const menu = this.menuElem();
    if (!menu) {
      return;
    }
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

  //// add / edit mode ////

  addBookmark() {
    const menu = this.menuElem();
    if (!menu) {
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
  }

  // opens the add / edit window on a row; called by the row pencil and by
  // clicking any other row while the window is already up
  editItem(item) {
    this.discardPendingAdd();
    this.closePrompt();
    this.state = BKM_EDIT_MODE;
    item.classList.add(SELECTED_CLASS); // highlight selected item
    this.drawPrompt(false, item.url, this.itemName(item), item);
  }

  // closes the window but leaves the bookmarks menu on-screen
  endEditMode() {
    this.discardPendingAdd();
    this.closePrompt();
    if (this.state !== BKM_INACTIVE) {
      this.state = BKM_OPEN_MODE;
    }
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
    // re-enable click outside of window
    document.querySelectorAll(`*:not(#${PROMPT_ID})`).forEach(elem => {
      elem.style.pointerEvents = "";
    });
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
    const [editFavUpBtn, editFavLookupBtn, editFavDownBtn,
           editFavSaveBtn, editFavCancelBtn, editFavDeleteBtn] =
      editFavPrompt.querySelectorAll("button");
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
        // save to file
        this.save();
        // close window, leaving the menu open
        this.endEditMode();
      }
      // delete button action
      if (btn_event.target === editFavDeleteBtn) {
        if (this.pendingAdd === item) {
          this.pendingAdd = null;
        }
        item.remove();
        // save to file
        this.save();
        // close window, leaving the menu open
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

  async lookupURL() {
    const urlBoxElem = document.getElementById("editFav__urlbox");
    const nameBoxElem = document.getElementById("editFav__namebox");
    if (urlBoxElem && nameBoxElem) {
      nameBoxElem.value = "Processing...";
      // read URL box
      let urlin = urlBoxElem.value;
      if (urlin === null || urlin == "") {
        // read from clipboard if URL empty
        urlin = await navigator.clipboard.readText();
      }
      // add HTTPs to URL
      let url = addHTTPtoURL(urlin);
      urlBoxElem.value = url;
      let encoded_url = base64URLSafeEncode(url);
      // search for the URLs title and favicon
      sendCmd('main', 'sitelookup', encoded_url).then((data) => {
        // the API answers { title, icon }; a plain string is still accepted
        // so an older backend keeps working
        const title = (data !== null && typeof data === 'object')
          ? data.title : data;
        const icon = (data !== null && typeof data === 'object')
          ? data.icon : "";
        if (title === null || title === undefined || title == "") {
          // URL lookup failed actions
          nameBoxElem.value = "Not Found";
          this.promptIcon = "";
          this.drawIconPreview();
          setTimeout(function() {
            if (urlBoxElem && nameBoxElem) {
              nameBoxElem.value = null;
              urlBoxElem.value = null;
            }
          }, 2000);
        } else {
          // remove invalid characters, write to name box
          nameBoxElem.value = cleanString(title);
          // show the fetched icon, saving writes it to the row
          this.promptIcon = validIcon(icon);
          this.drawIconPreview();
        }
      });
    }
  }
}
