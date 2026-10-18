// Automate Website - main menu
//
// The list button in the top left corner: the menu behind it, the theme
// colour, and the About window. new MainMenu({ isServer }) then .load() to
// apply the saved colour; showMenu() opens the menu with the rows of the
// page's model (MENU_HTML on the server, PI_MENU_HTML on a Pi).
//
// The stars toggle is drawn by dropdowns.js's drawMenu() as a browser-side
// setting row ('chk'); everything else here is built and placed by this
// class. The LED hosts' pages, once rows of this menu, are linked from the
// Pixels window (pixels.js). The picker's palette is the thememenu file, one colour per line as
// hex_value|display_name, read by ColorPrompt itself.

import { hexToRgb, gridButtonTemplate, registerListeners } from './utils.js';
import { openDropdown, drawMenu } from './dropdowns.js';
import ColorPrompt from './colorprompt.js';

export default class MainMenu {

  static STORAGE_KEY = 'main-color';
  static DEFAULT_COLOR = '#1f2051'; // dark blue

  // what a {{token}} in the templates below becomes while the rows it stands
  // for are being put in place
  static SLOT_ATTR = 'mainmenu-slot';

  // the dropdown main.js draws under the list button, on every model
  static MENU_ID = 'mainmenu';

  // The server's and the Pi's rows, written out in the order they are drawn. A row this class
  // does not build is plain markup sitting where it goes; a row it does is a
  // token -- {{color}} the colour box, {{stars}} the toggle drawMenu() draws.
  // Move a line and the menu moves with it; take a token out and that row is
  // left off altogether.
  //
  // The icon goes last on every row: the rows are laid out with their two
  // ends pushed apart, so an icon written after the label lands against the
  // menu's right edge. The handlers for these ids are registered in main.js.
  static MENU_HTML = `
    <a id="btn-about">About<i class="fad fa-info"></i></a>
    {{color}}
    {{stars}}
  `;

  static PI_MENU_HTML = `
    <a id="btn-about">About<i class="fad fa-info"></i></a>
    {{color}}
    {{stars}}
    <a id="btn-log-pi">System<i class="fad fa-wrench"></i></a>
    <a id="btn-home-pi">Home<i class="fad fa-home"></i></a>
  `;

  // The stars animation toggle, in the same host|type|name shape as a file
  // row so drawMenu() handles it with no special case. On a 'chk' row the
  // third field is the icon.
  static STARS_ROW = `stars|chk|fad fa-stars\n`;

  // goes on the page while the theme colour is a light one, so a rule per
  // place can turn its text black
  static LIGHT_CLASS = 'theme-light';

  // the colour row, and the picker it opens
  static ROW_ID = 'menu-theme-color';
  static ROW_LABEL = 'Colors';
  static ROW_ICON = 'fad fa-tint';
  static ANCHOR_ID = 'color-popup';
  static PALETTE_FILE = 'thememenu';
  static PROMPT_NAME = 'Theme'; // heading reads "Select Theme Color"

  // ---- where black text takes over from white ----
  //
  // BT.601 weights, for the brightness of a colour as the eye reads it rather
  // than as the numbers average out. Green carries most of what is seen as
  // brightness and blue very little, which is why a saturated blue comes out
  // so much darker than a green of the same numbers.
  static LUMA_RED = 299;
  static LUMA_GREEN = 587;
  static LUMA_BLUE = 114;
  static LUMA_TOTAL = 1000;

  // The one line every element crosses together: above it the text on the
  // theme colour goes black, below it stays white. The scale runs 0-255:
  //
  //     black        0        pure red    76        pure green  150
  //     pure blue   29        mid grey   128        cyan        179
  //     navy #1f2051 37       crimson    106        yellow      226
  //     forest      44        orange     148        white       255
  //
  // Raise this to hold white for longer, lower it to turn over sooner. Worth
  // knowing when moving it: the buttons are --main-trans-35, the theme colour
  // at 65% over black, so they read about a third darker than the value being
  // measured and turn over a little before they look as though they should.
  static LIGHT_LUMA = 135;

  static ABOUT_WIN_ID = 'about__prompt';
  static ABOUT_CLOSE_ID = 'about__btn';

  // isServer picks the menu's rows: true for the server's, false for a Pi's.
  //   new MainMenu({ isServer: false })  ->  About, Colors, stars, System, Home
  constructor({ isServer }) {
    // While the picker is open the page is repainted on every move, so the
    // colour can be seen as it is chosen; this holds what it was last
    // repainted in, and storage is not written until the prompt closes.
    this.pending = null;
    if (isServer) {
      this.menuTemplate = MainMenu.MENU_HTML;
    } else {
      this.menuTemplate = MainMenu.PI_MENU_HTML;
    }
  }

  //// the theme colour ////

  // the colour that was kept, or the default when nothing has been kept yet
  current() {
    const saved = localStorage.getItem(MainMenu.STORAGE_KEY);
    if (saved === null || saved === undefined || saved === '') {
      return MainMenu.DEFAULT_COLOR;
    }
    return saved;
  }

  // Paints the page without keeping the colour: the picker runs this on every
  // move of the map and the hue slider, and a write to storage on each of
  // those is neither wanted nor cheap.
  apply(color) {
    const root = document.getElementsByTagName('html')[0];
    if (!root) {
      return;
    }
    root.style.setProperty('--main-color', color);
    // marked light or not alongside the colour itself, so the two can never
    // disagree -- and the buttons turn over as the colour is dragged
    root.classList.toggle(MainMenu.LIGHT_CLASS, MainMenu.isLightColor(color));
  }

  save(color) {
    localStorage.setItem(MainMenu.STORAGE_KEY, color);
  }

  // paint and keep
  set(color) {
    this.apply(color);
    this.save(color);
  }

  // page load: draw in whatever was kept last time
  load() {
    this.set(this.current());
  }

  // Is this colour light enough that white text on it is hard to read? The
  // weighted average is the BT.601 luma the eye actually reads a colour at --
  // a flat mean calls a saturated blue as light as a green of the same
  // numbers, which it is nothing like.
  static isLightColor(color) {
    const rgb = hexToRgb(color);
    const redPart = rgb.red * MainMenu.LUMA_RED;
    const greenPart = rgb.green * MainMenu.LUMA_GREEN;
    const bluePart = rgb.blue * MainMenu.LUMA_BLUE;
    const luma = (redPart + greenPart + bluePart) / MainMenu.LUMA_TOTAL;
    if (!Number.isFinite(luma)) {
      return false; // unreadable value -- leave the text as it was
    }
    return luma > MainMenu.LIGHT_LUMA;
  }

  //// the menu ////

  // The list button. Same shape as the other menus in dropdowns.js -- a second
  // press closes it, and opening it puts every other menu away first. Its
  // rows are all in the template, so it is drawn afresh each time it opens.
  showMenu() {
    if (!openDropdown(MainMenu.MENU_ID)) {
      return;
    }
    const menuElement = document.getElementById(MainMenu.MENU_ID);
    this.drawRows(menuElement);
  }

  // Draws the menu: its own markup from the template, then the rows that are
  // built rather than written -- each moved into the marker its token left.
  drawRows(menuElement) {
    // the menu's own rows, with an empty marker standing in for each set of
    // built ones. Whatever was here last time goes with it.
    menuElement.innerHTML = this.menuTemplate.replace(/\{\{(\w+)\}\}/g,
      (whole, slot) => `<span data-${MainMenu.SLOT_ATTR}="${slot}"></span>`);
    // drawMenu() appends to the end of the container, so what it draws is
    // picked back up below and moved into the markers
    drawMenu(MainMenu.STARS_ROW, menuElement.id);
    // the stars row is the one node drawMenu() made (none if it drew nothing)
    const starsRows = App.dynMenuNodes.slice(0, 1);
    this.fillSlot(menuElement, 'stars', starsRows.map(row => this.iconLast(row)));
    const colorRow = this.colorRow();
    this.fillSlot(menuElement, 'color', [colorRow]);
    // removeDynMenus() takes the rows back out by node, so this one is handed
    // over with the rows drawMenu() made
    App.dynMenuNodes.push(colorRow);
    // the palette is wanted by the picker rather than by the menu, so it is
    // only started here -- by the time the row is pressed it is usually in
    ColorPrompt.warmPalette(MainMenu.PALETTE_FILE);
  }

  // Sends a row's icon to the end of it. createListItem() puts the icon first
  // and the switch after it, which is right for a menu row anywhere else;
  // this menu carries its icons down the right hand edge.
  iconLast(row) {
    if (!row) {
      return row;
    }
    const icon = row.querySelector(':scope > i');
    if (icon) {
      row.appendChild(icon);
    }
    return row;
  }

  // Puts rows where their marker is and takes the marker out. A template with
  // that token removed has nowhere to put them, so they are dropped rather
  // than left sitting at the end of the menu.
  fillSlot(menuElement, name, rows) {
    const slot = menuElement.querySelector(`[data-${MainMenu.SLOT_ATTR}="${name}"]`);
    if (!slot) {
      rows.forEach(row => row.remove());
      return;
    }
    rows.forEach(row => slot.parentNode.insertBefore(row, slot));
    slot.remove();
  }

  // The colour box: the row's background is the colour the theme is set to,
  // and it opens the picker on the theme palette.
  colorRow() {
    const color = this.current();
    const row = document.createElement('a');
    row.id = MainMenu.ROW_ID;
    row.className = 'mainmenu-colorbox';
    row.style.setProperty('background-color', color);
    // white on a light box is barely there, so the label and the drop turn
    // dark instead once the colour is light enough to need it
    if (MainMenu.isLightColor(color)) {
      row.classList.add('mainmenu-colorbox--invert');
    }
    row.appendChild(document.createTextNode(MainMenu.ROW_LABEL));
    // last, so the drop sits on the right with every other row's icon
    const icon = document.createElement('i');
    icon.className = MainMenu.ROW_ICON;
    row.appendChild(icon);
    row.addEventListener('click', () => this.openPrompt());
    return row;
  }

  //// the colour picker ////

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
    const prompt = new ColorPrompt(MainMenu.ANCHOR_ID, (strippedHex) => {
      // live, so the page is seen in the colour while it is being picked
      this.pending = `#${strippedHex}`;
      this.apply(this.pending);
    }, {
      name: MainMenu.PROMPT_NAME,
      presetFile: MainMenu.PALETTE_FILE,
      onClose: () => this.closePrompt()
    });
    prompt.show();
    // seed the picker with the colour already set, without sending it back
    prompt.updateStateFromHex(startColor, true, false);
    App.colorPrompt = prompt;
  }

  // The prompt has gone -- closed, replaced, or taken with the page. The page
  // is already in the colour it was left on; this is where it is kept.
  closePrompt() {
    if (this.pending === null) {
      return;
    }
    this.save(this.pending);
    this.pending = null;
  }

  //// the about window ////

  // Opens the About window, unless it is already open. Its Close button is
  // registered below the class.
  aboutPrompt() {
    const winId = MainMenu.ABOUT_WIN_ID;
    if (document.getElementById(winId)) return;
    const curDate = new Date();
    const curYear = curDate.getFullYear();
    const monthsPerQuarter = 3;
    const curQtr = Math.ceil((curDate.getMonth() + 1) / monthsPerQuarter);
    const promptHTML = `
  <div id="${winId}" class="prompt_win">
    <div class="about__title">${App.sysModel}</div>
    <img src="img/automate.png" id="about__img" />
    <div class="about__text">by Ben Provenzano III</div>
    <div class="about__text">${curYear}.${curQtr} Release</div>
    ${gridButtonTemplate({
      buttonId: MainMenu.ABOUT_CLOSE_ID,
      icon: 'fad fa-times-octagon right-padding',
      label: 'Close'
    })}
  </div>
  `;
    document.body.insertAdjacentHTML('beforeend', promptHTML);
  }

  // Takes the About window down, if it is open.
  static closeAboutPrompt() {
    const aboutWindow = document.getElementById(MainMenu.ABOUT_WIN_ID);
    if (aboutWindow) {
      aboutWindow.remove();
    }
  }
}
registerListeners({
  [MainMenu.ABOUT_CLOSE_ID]: () => MainMenu.closeAboutPrompt()
});
