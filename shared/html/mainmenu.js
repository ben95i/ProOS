/**
 * MainMenu
 * The list button in the top left corner: the menu behind it, everything the
 * menu can do, and the two windows it opens.
 *
 * This was theme.js, which owned the palette button's dropdown. That
 * dropdown is gone -- its two rows have moved into the main menu, which now
 * reads Color, Stars, About, then whatever else the page has: the URLs out
 * of the mainmenu file on the server, and the System and Home rows on a Pi.
 * The theme colour, the About window and both menus are one thing to press
 * now, so they are one file.
 *
 * The stars toggle is still drawn by main.js's drawMenu(): it is a
 * browser-side setting row ('chk'), and its read and write live in
 * WWW_SETTINGS. Everything else here is built and placed by this class.
 *
 * The palette the colour picker offers is the thememenu file, one colour per
 * line as hex_value|display_name. ColorPrompt reads it -- it already reads a
 * file of exactly that shape for its own default swatches -- so the file is
 * named to it rather than loaded here.
 */
import { hexToRgb, loadMenu, fileURL, splitMenuData } from './utils.js';
import ColorPrompt from './colorprompt.js';

export default class MainMenu {

  static STORAGE_KEY = 'main-color';
  static DEFAULT_COLOR = '#1f2051'; // dark blue

  // what a {{token}} in the templates below becomes while the rows it stands
  // for are being put in place
  static SLOT_ATTR = 'mainmenu-slot';

  static MENU_ID = 'mainmenu';
  // the file behind the server's menu: its URL rows, and nothing else
  static MENU_FILE = 'mainmenu';
  static PI_MENU_ID = 'pi-mainmenu';

  // Both menus, written out in the order they are drawn.
  //
  // A row this class does not build is plain markup, sitting where it goes.
  // A row it does is a token: {{color}} is the colour box below, {{stars}}
  // the toggle drawMenu() draws, {{links}} the URLs out of the mainmenu
  // file. Move a line and the menu moves with it; take a token out and those
  // rows are left off altogether.
  //
  // The icon goes last on every row: the rows are laid out with their two
  // ends pushed apart, so an icon written after the label lands against the
  // menu's right edge, where all of them line up.
  //
  // The handlers for the ids below are registered in main.js, by id, off a
  // listener on the document -- so these can be rewritten as freely as any
  // other markup without a listener having to be rebound.
  static MENU_HTML = `
    <a id="btn-about-main">About<i class="fad fa-info"></i></a>
    {{color}}
    {{stars}}
    {{links}}
  `;

  static PI_MENU_HTML = `
    <a id="btn-about-pi">About<i class="fad fa-info"></i></a>
    {{color}}
    {{stars}}
    <a id="btn-log-pi">System<i class="fad fa-wrench"></i></a>
    <a id="btn-home-pi">Home<i class="fad fa-home"></i></a>
  `;

  // The stars animation toggle. Written in the same host|type|name shape as
  // a file row so drawMenu() handles it with no special case of its own; on
  // a 'chk' row the third field is the icon.
  static STARS_ROW = `stars|chk|fad fa-stars\n`;

  // Goes on the page itself while the theme colour is a light one, so a rule
  // per place can turn its text black. One class for all of them, so
  // everything that turns over turns over together.
  static LIGHT_CLASS = 'theme-light';

  // the colour row, and the picker it opens
  static ROW_ID = 'menu-theme-color';
  static ROW_LABEL = 'Color';
  // the paint drop, in the same duotone set the rest of the bar uses
  static ROW_ICON = 'fad fa-tint';
  static ANCHOR_ID = 'color-popup';
  static PALETTE_FILE = 'thememenu';
  // what the picker's heading calls this colour -- "Select Theme Color"
  static PROMPT_NAME = 'Theme';
  // ---- where black text takes over from white ----
  //
  // BT.601 weights, for the brightness of a colour as the eye reads it
  // rather than as the numbers average out. Green carries most of what is
  // seen as brightness and blue very little, which is why a saturated blue
  // comes out so much darker than a green of the same numbers.
  static LUMA_RED = 299;
  static LUMA_GREEN = 587;
  static LUMA_BLUE = 114;
  static LUMA_TOTAL = 1000;

  // The one line every element crosses together: above it the text on the
  // theme colour goes black, below it stays white.
  //
  // The scale those weights produce runs from 0 to 255. Some of it, to make
  // a number here easier to choose:
  //
  //     black        0        pure red    76        pure green  150
  //     pure blue   29        mid grey   128        cyan        179
  //     navy #1f2051 37       crimson    106        yellow      226
  //     forest      44        orange     148        white       255
  //
  // 128 is the middle of that scale. Swept over the whole colour cube it
  // agrees with what WCAG contrast says about the better of black and white
  // roughly 85% of the time; the best any single value on this formula
  // manages is 108, at about 94%, but that pulls black text onto colours
  // that still read dark. Raise this to hold white for longer, lower it to
  // turn over sooner.
  //
  // Worth knowing when moving it: the buttons are the one surface that is
  // not this colour at full strength. Their background is --main-trans-35,
  // the theme colour at 65% over the black page, so they read about a third
  // darker than the value being measured and turn over a little before they
  // look as though they should. Around 166 holds white on them until the
  // colour is pale enough that the buttons are pale too.
  static LIGHT_LUMA = 135;

  static ABOUT_WIN_ID = 'about__prompt';

  constructor() {
    // While the picker is open the page is repainted on every move, so the
    // colour can be seen as it is chosen; this holds what it was last
    // repainted in, and storage is not written until the prompt closes.
    this.pending = null;
    // bumped per menu request, so a file landing after the menu has been
    // closed or opened again cannot draw itself over what is on screen
    this.menuToken = 0;
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

  // Paints the page without keeping the colour. This is the half of the old
  // setTheme() the picker wants: it runs on every move of the map and the
  // hue slider, and a write to storage on each of those is neither wanted
  // nor cheap.
  apply(color) {
    const root = document.getElementsByTagName('html')[0];
    if (!root) {
      return;
    }
    root.style.setProperty('--main-color', color);
    // The page is marked light or not alongside the colour itself, so the
    // two can never disagree -- and since the picker calls this on every
    // move, the buttons turn over as the colour is being dragged rather than
    // once it has been settled on.
    root.classList.toggle(MainMenu.LIGHT_CLASS, MainMenu.isLightColor(color));
  }

  save(color) {
    localStorage.setItem(MainMenu.STORAGE_KEY, color);
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
    const luma = (rgb.red * MainMenu.LUMA_RED
      + rgb.green * MainMenu.LUMA_GREEN
      + rgb.blue * MainMenu.LUMA_BLUE) / MainMenu.LUMA_TOTAL;
    if (!Number.isFinite(luma)) {
      return false; // unreadable value -- leave the text as it was
    }
    return luma > MainMenu.LIGHT_LUMA;
  }

  //// the menu ////

  // The server's list button. Same shape as the file menus in main.js -- a
  // second press closes it, and opening it puts every other menu away first.
  async showMenu() {
    const menuElement = document.getElementById(MainMenu.MENU_ID);
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
    const data = await loadMenu(fileURL('read', MainMenu.MENU_FILE));
    // Opened again, or closed outright, while the file was in the air. The
    // display check matters as much as the token: drawing now would call
    // drawMenu(), which clears whichever menu has since taken the screen.
    if (token !== this.menuToken || menuElement.style.display !== 'block') {
      return;
    }
    // a file that could not be read still leaves a menu worth having -- the
    // rows built here do not come from it
    this.drawRows(menuElement, MainMenu.MENU_HTML, data);
  }

  // The Pi's list button. Its rows are already in the markup, so there is no
  // file to wait for and the menu is drawn where it is pressed.
  showPiMenu() {
    const menuElement = document.getElementById(MainMenu.PI_MENU_ID);
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
    // nothing above the token here, but a file menu opened a moment ago can
    // still be in the air and this menu is what it would draw over
    this.menuToken += 1;
    this.drawRows(menuElement, MainMenu.PI_MENU_HTML, '');
  }

  // Draws the menu: its own markup from the template above, then the rows
  // that are built rather than written -- each moved into the marker its
  // token left behind.
  drawRows(menuElement, template, fileRows) {
    // The file comes back from the API wrapped in quotes, and splitMenuData()
    // takes a quote off only the very start and end of what it is handed.
    // Putting the stars row in front leaves the file's opening quote sitting
    // in the middle of the string, where it survives into the first row and
    // ends up on the front of its URL. So the file is unquoted here, before
    // the two are joined.
    const rows = (fileRows && typeof fileRows === 'string')
      ? splitMenuData(fileRows).join('\n') : '';
    // the menu's own rows, with an empty marker standing in for each set of
    // built ones. Whatever was here last time goes with it.
    menuElement.innerHTML = template.replace(/\{\{(\w+)\}\}/g,
      (whole, slot) => `<span data-${MainMenu.SLOT_ATTR}="${slot}"></span>`);
    // Reference the globally exposed window function. It appends to the end
    // of the container, so what it draws is picked back up below and moved
    // into the markers rather than left where it landed.
    if (typeof window.drawMenu === 'function') {
      window.drawMenu(MainMenu.STARS_ROW + rows, menuElement.id);
    }
    // the stars row was the first thing drawMenu() was given, so it is the
    // first node it made; the file's URLs are whatever follows it
    const drawn = (App.dynMenuNodes || []).slice();
    this.fillSlot(menuElement, 'stars', drawn.slice(0, 1).map(row => this.iconLast(row)));
    this.fillSlot(menuElement, 'links', drawn.slice(1));
    const colorRow = this.colorRow();
    this.fillSlot(menuElement, 'color', [colorRow]);
    // removeDynMenus() takes the rows back out by node, so this one is
    // handed over with the rows drawMenu() made
    if (App.dynMenuNodes) {
      App.dynMenuNodes.push(colorRow);
    }
    // the palette is wanted by the picker rather than by the menu, so it is
    // only started here -- by the time the row is pressed it is usually in
    ColorPrompt.warmPalette(MainMenu.PALETTE_FILE);
  }

  // Sends a row's icon to the end of it. The stars row is built by
  // createListItem(), which puts the icon first and the switch after it --
  // right for a menu row anywhere else, but these two menus carry their icons
  // down the right-hand edge. Moved here rather than in main.js, since the
  // 'chk' row type is a general one and this is only how these menus lay
  // their rows out.
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

  // Puts rows where their marker is and takes the marker out. A template
  // with that token removed has nowhere to put them, so they are dropped
  // rather than left sitting at the end of the menu.
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
      // the theme's own palette file, in place of the picker's default one
      presetFile: MainMenu.PALETTE_FILE,
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

  //// the about window ////

  aboutPrompt() {
    const winId = MainMenu.ABOUT_WIN_ID;
    if (document.getElementById(winId)) return;
    const curDate = new Date();
    const curYear = curDate.getFullYear();
    const curQtr = Math.ceil((curDate.getMonth() + 1) / 3);
    const promptHTML = `
  <div id="${winId}" class="prompt_win">
    <div class="about__title">${App.sysModel}</div>
    <img src="img/automate.png" id="about__img" />
    <div class="about__text">by Ben Provenzano III</div>
    <div class="about__text">${curYear}.${curQtr} Release</div>
    <button type="button" class="button" id="about__btn">
      <i class="fad fa-times-octagon right-padding"></i>
      <span class="button__text">Close</span>
    </button>
  </div>
  `;
    document.body.insertAdjacentHTML('beforeend', promptHTML);
    const aboutprompt = document.getElementById(winId);
    aboutprompt.addEventListener('click', function handleButtonClicks(btn_evnt) {
      const clickedButton = btn_evnt.target.closest('button');
      if (!clickedButton) return;
      aboutprompt.removeEventListener('click', handleButtonClicks);
      aboutprompt.remove();
    });
  }
}
