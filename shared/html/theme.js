/**
 * Theme
 * The theme colour: the value the page is drawn in, where it is kept, and
 * the menu and colour picker that change it.
 *
 * Taken out of main.js and utils.js. The menu is opened straight off the
 * palette button now rather than through showDynFileMenu(), and the colour
 * row is built here rather than as a 'thm' case in createListItem() -- the
 * only thing still borrowed from main.js is drawMenu(), which draws the
 * stars animation toggle above the colour row. That row is a browser-side
 * setting ('chk'), and its read and write live in main.js's WWW_SETTINGS.
 *
 * The old thememenu file held one row per colour, `hex|thm|name`, and each
 * row set the theme on its own. The file is now a palette, one colour per
 * line as `hex_value|display_name`, and it is loaded into the picker rather
 * than drawn as rows: the menu shows a single colour box, in whatever the
 * theme is set to, that opens the picker on that palette.
 */
import { parsePalette, loadMenu, fileURL, hexToRgb } from './utils.js';
import ColorPrompt from './colorprompt.js';

export default class Theme {

  static STORAGE_KEY = 'main-color';
  static DEFAULT_COLOR = '#1f2051'; // dark blue
  // the dropdown, the file behind it, and the anchor renderPage() injects
  // for every colour prompt on the site
  static MENU_ID = 'thememenu';
  static MENU_FILE = 'thememenu';
  static ANCHOR_ID = 'color-popup';
  // The stars animation toggle, above the colour row. Written in the same
  // host|type|name shape as a file row so drawMenu() handles it with no
  // special case of its own; on a 'chk' row the third field is the icon.
  static STARS_ROW = `stars|chk|fad fa-stars\n`;
  static ROW_ID = 'menu-theme-color';
  static ROW_LABEL = 'Color';
  // what the picker's heading calls this colour -- "Select Theme Color"
  static PROMPT_NAME = 'Theme';
  // the paint drop, in the same duotone set the rest of the bar uses
  static ROW_ICON = 'fad fa-tint';
  // A box lighter than this carries dark text instead of white. Set just
  // under a pure green (150), which is the brightest colour that still lands
  // low on this scale and is already hard to read white on.
  static LIGHT_LUMA = 140;

  constructor() {
    // the palette last read out of the thememenu file, as { color, name }
    this.palette = [];
    // While the picker is open the page is repainted on every move, so the
    // colour can be seen as it is chosen; this holds what it was last
    // repainted in, and storage is not written until the prompt closes.
    this.pending = null;
    // bumped per menu request, so a file landing after the menu has been
    // closed or opened again cannot draw itself over what is on screen
    this.menuToken = 0;
  }

  //// the colour itself ////

  // the colour that was kept, or the default when nothing has been kept yet
  current() {
    const saved = localStorage.getItem(Theme.STORAGE_KEY);
    if (saved === null || saved === undefined || saved === '') {
      return Theme.DEFAULT_COLOR;
    }
    return saved;
  }

  // Paints the page without keeping the colour. This is the half of the old
  // setTheme() the picker wants: it runs on every move of the map and the
  // hue slider, and a write to storage on each of those is neither wanted
  // nor cheap.
  apply(color) {
    const root = document.getElementsByTagName('html')[0];
    if (root) {
      root.style.setProperty('--main-color', color);
    }
  }

  save(color) {
    localStorage.setItem(Theme.STORAGE_KEY, color);
  }

  // paint and keep -- what the old setTheme() did
  set(color) {
    this.apply(color);
    this.save(color);
  }

  // page load: draw in whatever was kept last time
  load() {
    this.set(this.current());
  }

  // Is this colour light enough that white text on it is hard to read? The
  // weighted average is the BT.601 luma the eye actually reads a colour at
  // -- a flat mean calls a saturated blue as light as a green of the same
  // numbers, which it is nothing like.
  static isLightColor(color) {
    const rgb = hexToRgb(color);
    const luma = (rgb.red * 299 + rgb.green * 587 + rgb.blue * 114) / 1000;
    if (!Number.isFinite(luma)) {
      return false; // unreadable value -- leave the text as it was
    }
    return luma > Theme.LIGHT_LUMA;
  }

  //// the menu ////

  // The thememenu file: one colour per line, `hex_value|display_name`. The
  // reading of it lives in utils.js, since the colour prompt's own default
  // palette is a second file in the same format.
  setPalette(fileText) {
    this.palette = parsePalette(fileText);
    return this.palette;
  }

  // The palette button. Same shape as the file menus in main.js -- a second
  // press closes it, and opening it puts every other menu away first.
  async showMenu() {
    const menuElement = document.getElementById(Theme.MENU_ID);
    if (!menuElement) {
      return;
    }
    if (menuElement.style.display === 'block') {
      menuElement.style.display = 'none';
      return;
    }
    // Reference the globally exposed window function
    if (typeof window.removeDropdowns === 'function') window.removeDropdowns();
    menuElement.style.display = 'block';
    const token = ++this.menuToken;
    const data = await loadMenu(fileURL('read', Theme.MENU_FILE));
    // Opened again, or closed outright, while the file was in the air. The
    // display check matters as much as the token: drawing now would call
    // drawMenu(), which clears whichever menu has since taken the screen.
    if (token !== this.menuToken || menuElement.style.display !== 'block') {
      return;
    }
    // a file that could not be read leaves the palette empty, which the
    // picker answers with its own presets -- the menu still draws
    this.setPalette(data);
    this.drawMenu(menuElement);
  }

  drawMenu(menuElement) {
    // Reference the globally exposed window function. It clears the rows of
    // whatever menu was last drawn, so the colour row is appended after it
    // rather than before.
    if (typeof window.drawMenu === 'function') {
      window.drawMenu(Theme.STARS_ROW, Theme.MENU_ID);
    }
    const row = this.colorRow();
    menuElement.appendChild(row);
    // removeDynMenus() takes the rows back out by node, so this one is
    // handed over with the rows drawMenu() made
    if (App.dynMenuNodes) {
      App.dynMenuNodes.push(row);
    }
  }

  // The colour box: the row's background is the colour the theme is set to,
  // and it opens the picker on the palette just read.
  colorRow() {
    const color = this.current();
    const row = document.createElement('a');
    row.id = Theme.ROW_ID;
    row.className = 'theme-colorbox';
    row.style.setProperty('background-color', color);
    // white on a light box is barely there, so the label and the drop turn
    // dark instead once the colour is light enough to need it
    if (Theme.isLightColor(color)) {
      row.classList.add('theme-colorbox--invert');
    }
    const icon = document.createElement('i');
    icon.className = Theme.ROW_ICON;
    row.appendChild(icon);
    row.appendChild(document.createTextNode(Theme.ROW_LABEL));
    row.addEventListener('click', () => this.openPrompt());
    return row;
  }

  //// the picker ////

  openPrompt() {
    // the previous prompt is destroyed rather than dropped: show() wipes the
    // anchor's markup, and an instance left behind mid-drag still holds
    // pointermove / pointerup listeners on window
    if (App.colorPrompt) {
      App.colorPrompt.destroy();
      App.colorPrompt = null;
    }
    const startColor = this.current();
    this.pending = startColor;
    const prompt = new ColorPrompt(Theme.ANCHOR_ID, (strippedHex) => {
      // live, so the page is seen in the colour while it is being picked
      this.pending = `#${strippedHex}`;
      this.apply(this.pending);
    }, {
      name: Theme.PROMPT_NAME,
      // the palette out of the file, in place of the picker's own presets
      presets: this.palette,
      onClose: () => this.closePrompt()
    });
    prompt.show();
    // seed the picker with the colour already set, without sending it back
    prompt.updateStateFromHex(startColor, true, false);
    App.colorPrompt = prompt;
  }

  // The prompt has gone -- closed, replaced, or taken with the page. The
  // page is already in the colour it was left on; this is where that colour
  // is finally kept.
  closePrompt() {
    if (this.pending === null) {
      return;
    }
    this.save(this.pending);
    this.pending = null;
  }
}
