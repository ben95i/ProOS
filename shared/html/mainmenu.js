// Automate - the list button's menu, the theme colour and the About window.
// new MainMenu({ isServer }) then .load() applies the saved colour; showMenu() opens the
// menu. The stars row is a 'chk' row drawn by drawMenu().

import { hexToRgb, gridButtonTemplate, registerListeners, onDemandModule } from './utils.js';
import { openDropdown, drawMenu } from './dropdowns.js';

// the colour picker, loaded by its first use
const loadColorPrompt = onDemandModule(() => import('./colorprompt.js'), 'the colour picker');

export default class MainMenu {

  static STORAGE_KEY = 'main-color';
  static DEFAULT_COLOR = '#1f2051';

  static MENU_ID = 'mainmenu';

  // Each model's written rows, icon last; the colour and stars rows go in after About.
  // Their handlers are registered in main.js.
  static MENU_HTML = `
    <a id="btn-about">About<i class="fad fa-info"></i></a>
  `;

  static PI_MENU_HTML = `
    <a id="btn-about">About<i class="fad fa-info"></i></a>
    <a id="btn-log-pi">System<i class="fad fa-wrench"></i></a>
    <a id="btn-home-pi">Home<i class="fad fa-home"></i></a>
  `;

  // The stars toggle as a file row; on a 'chk' row the third field is the icon.
  static STARS_ROW = `stars|chk|fad fa-stars\n`;

  // on the page while the theme colour is light, so its text can turn black
  static LIGHT_CLASS = 'theme-light';

  // the colour row, and the picker it opens
  static ROW_ID = 'menu-theme-color';
  static ROW_LABEL = 'Colors';
  static ROW_ICON = 'fad fa-tint';
  static PALETTE_FILE = 'thememenu';
  static PROMPT_NAME = 'Theme';

  // BT.601 weights, for a colour's brightness as the eye reads it
  static LUMA_RED = 299;
  static LUMA_GREEN = 587;
  static LUMA_BLUE = 114;
  static LUMA_TOTAL = 1000;

  // Above this brightness (0-255) the text on the theme colour goes black. Raise it to
  // hold white longer; buttons are --main-trans-35, so they read about a third darker.
  // black 0, blue 29, navy #1f2051 37, red 76, crimson 106, mid grey 128, orange 148, green 150
  static LIGHT_LUMA = 135;

  static ABOUT_WIN_ID = 'about__prompt';
  static ABOUT_CLOSE_ID = 'about__btn';

  // isServer picks the menu's rows
  constructor({ isServer }) {
    // the colour painted while the picker is open; saved when it closes
    this.pending = null;
    if (isServer) {
      this.menuTemplate = MainMenu.MENU_HTML;
    } else {
      this.menuTemplate = MainMenu.PI_MENU_HTML;
    }
  }

  //// the theme colour ////

  // the kept colour, or the default
  current() {
    const saved = localStorage.getItem(MainMenu.STORAGE_KEY);
    if (saved === null || saved === '') {
      return MainMenu.DEFAULT_COLOR;
    }
    return saved;
  }

  // Paints the page without saving, as the picker runs this on every move.
  apply(color) {
    const root = document.documentElement;
    root.style.setProperty('--main-color', color);
    root.classList.toggle(MainMenu.LIGHT_CLASS, MainMenu.isLightColor(color));
  }

  // keeps a colour
  save(color) {
    localStorage.setItem(MainMenu.STORAGE_KEY, color);
  }

  // page load: paints and keeps what was kept last time
  load() {
    const color = this.current();
    this.apply(color);
    this.save(color);
  }

  // Is this colour light enough that white text on it is hard to read?
  static isLightColor(color) {
    const rgb = hexToRgb(color);
    const redPart = rgb.red * MainMenu.LUMA_RED;
    const greenPart = rgb.green * MainMenu.LUMA_GREEN;
    const bluePart = rgb.blue * MainMenu.LUMA_BLUE;
    const luma = (redPart + greenPart + bluePart) / MainMenu.LUMA_TOTAL;
    if (!Number.isFinite(luma)) {
      return false; // unreadable value: leave the text as it was
    }
    return luma > MainMenu.LIGHT_LUMA;
  }

  // The list button: toggles the menu, drawn afresh each time it opens.
  showMenu() {
    if (!openDropdown(MainMenu.MENU_ID)) {
      return;
    }
    const menuElement = document.getElementById(MainMenu.MENU_ID);
    this.drawRows(menuElement);
  }

  // Draws the written rows, then drawMenu()'s stars row, and puts it and the colour row
  // after About. removeDynMenus() takes both back out.
  drawRows(menuElement) {
    menuElement.innerHTML = this.menuTemplate;
    drawMenu(MainMenu.STARS_ROW, menuElement.id);
    const starsRow = App.dynMenuNodes[0];
    const starsIcon = starsRow.querySelector(':scope > i');
    starsRow.appendChild(starsIcon);
    const colorRow = this.colorRow();
    menuElement.querySelector('#btn-about').after(colorRow, starsRow);
    App.dynMenuNodes.push(colorRow);
    this.readPalette();
  }

  // the picker's palette, so it is usually in by the time the row is pressed
  async readPalette() {
    const colorPromptModule = await loadColorPrompt();
    if (colorPromptModule) {
      colorPromptModule.loadPaletteFile(MainMenu.PALETTE_FILE);
    }
  }

  // The colour row: drawn in the theme colour, and opens the picker.
  colorRow() {
    const color = this.current();
    const row = document.createElement('a');
    row.id = MainMenu.ROW_ID;
    row.className = 'mainmenu-colorbox';
    row.style.setProperty('background-color', color);
    // dark text once the colour is light
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

  // opens the picker on the kept colour, painting the page live as it moves
  async openPrompt() {
    const colorPromptModule = await loadColorPrompt();
    if (!colorPromptModule) {
      return;
    }
    colorPromptModule.closeColorPrompt();
    const startColor = this.current();
    this.pending = startColor;
    colorPromptModule.showColorPrompt(startColor, (strippedHex) => {
      this.pending = `#${strippedHex}`;
      this.apply(this.pending);
    }, {
      name: MainMenu.PROMPT_NAME,
      presetFile: MainMenu.PALETTE_FILE,
      onClose: () => this.closePrompt()
    });
  }

  // the prompt has gone: keeps the colour the page was left on
  closePrompt() {
    if (this.pending === null) {
      return;
    }
    this.save(this.pending);
    this.pending = null;
  }

  //// the about window ////

  // Opens the About window, unless already open.
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
      icon: 'fas fa-times right-padding',
      label: 'Close'
    })}
  </div>
  `;
    document.body.insertAdjacentHTML('beforeend', promptHTML);
  }

  // takes the About window down, if open
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
