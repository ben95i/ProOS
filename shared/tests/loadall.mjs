import { ensureWifiStub, setupDom, ORIG, NEW, check, same, report, resetBody } from './env.mjs';
import { pathToFileURL } from 'node:url';

ensureWifiStub();
const testWin = setupDom();
Object.defineProperty(global, 'App', {
  get: () => testWin.App, set: (value) => { testWin.App = value; }, configurable: true
});
// main.js imports wifi.js, which is not in the snapshot (it holds a secret);
// a stub stands in for it so the module graph can be loaded here.
const files = ['device.js', 'utils.js', 'switch.js', 'weather.js', 'roomstate.js',
  'stars.js', 'colorprompt.js', 'mainmenu.js', 'bookmarks.js', 'lists.js', 'main.js'];

const loaded = {};
for (const file of files) {
  for (const [side, root] of [['before', ORIG], ['after', NEW]]) {
    try {
      const mod = await import(pathToFileURL(root + file).href);
      loaded[`${side}:${file}`] = mod;
      check(`${side} ${file} loads`, true);
    } catch (error) {
      check(`${side} ${file} loads`, false, error.message);
    }
  }
}

//// exported pure functions, before vs after ////

const bmA = loaded['before:bookmarks.js'];
const bmB = loaded['after:bookmarks.js'];
if (bmA && bmB) {
  for (const url of ['', 'example.com', 'http://x.y', 'HTTPS://X.Y', 'https://a',
      'ftp://z', 'HtTp://q', 'a b']) {
    same(`addHTTPtoURL(${url})`, bmA.addHTTPtoURL(url), bmB.addHTTPtoURL(url));
  }
  same('addHTTPtoURL(null)', bmA.addHTTPtoURL(null), bmB.addHTTPtoURL(null));
}

const lsA = loaded['before:lists.js'];
const lsB = loaded['after:lists.js'];
if (lsA && lsB) {
  const names = ['', ' ', 'Shopping', 'Caf\u00e9 na\u00efve', 'a|b', 'a~b',
    'multi\nline', 'x'.repeat(80), 'x'.repeat(1200), '  spaced   out  ',
    '\u4f60\u597d list', 'tab\tsep', '!ENC', 'a\r\nb'];
  for (const name of names) {
    same(`cleanName(${JSON.stringify(name)})`, lsA.cleanName(name), lsB.cleanName(name));
    same(`cleanItemName(${JSON.stringify(name)})`,
      lsA.cleanItemName(name), lsB.cleanItemName(name));
  }
  for (const value of [null, undefined, 12]) {
    same(`cleanName(${String(value)})`, lsA.cleanName(value), lsB.cleanName(value));
    same(`cleanItemName(${String(value)})`,
      lsA.cleanItemName(value), lsB.cleanItemName(value));
  }
  // ListStore parse / buildBody round trip over a range of files
  const files = [
    '',
    '0|Shopping|0|Milk|0\n0|Shopping|1|Eggs|1\n',
    '1|Empty|||0\n',
    'LOCK|1|token|123|0\n0|Kept|0|Row|0\n',
    '2|!ENC|0|E1.AAAA.BBBB|0\n',
    '2|!ENC|0|notablob|0\n',
    '3|Multi|0|line one~line two|0\n',
    'junk row\n|||\n4|Ok|0|Item|1\n',
    '5|Name|abc|Item|0\n',
    '"0|Quoted|0|Item|0"'
  ];
  for (const text of files) {
    const storeA = new lsA.ListStore(() => {});
    const storeB = new lsB.ListStore(() => {});
    same(`rows(${JSON.stringify(text)})`, storeA.rows(text), storeB.rows(text));
    storeA.data = storeA.parse(text);
    storeB.data = storeB.parse(text);
    same(`parse(${JSON.stringify(text)})`, storeA.data, storeB.data);
    same(`buildBody(${JSON.stringify(text)})`, storeA.buildBody(), storeB.buildBody());
    same(`sealText`, storeA.data.map(e => e.enc ? '' : storeA.sealText(e)),
      storeB.data.map(e => e.enc ? '' : storeB.sealText(e)));
  }
}

const mmA = loaded['before:mainmenu.js'];
const mmB = loaded['after:mainmenu.js'];
if (mmA && mmB) {
  for (let i = 0; i < 0x1000000; i += 7919) {
    const hex = '#' + i.toString(16).padStart(6, '0');
    const a = mmA.default.isLightColor(hex);
    const b = mmB.default.isLightColor(hex);
    if (a !== b) { check(`isLightColor(${hex})`, false, `${a} vs ${b}`); }
  }
  check('isLightColor sweep', true);
  same('mainmenu constants',
    [mmA.default.DEFAULT_COLOR, mmA.default.LIGHT_LUMA, mmA.default.MENU_HTML,
     mmA.default.PI_MENU_HTML, mmA.default.STARS_ROW, mmA.default.PALETTE_FILE],
    [mmB.default.DEFAULT_COLOR, mmB.default.LIGHT_LUMA, mmB.default.MENU_HTML,
     mmB.default.PI_MENU_HTML, mmB.default.STARS_ROW, mmB.default.PALETTE_FILE]);
}

const cpA = loaded['before:colorprompt.js'];
const cpB = loaded['after:colorprompt.js'];
if (cpA && cpB) {
  for (const name of ['', 'LED', 'Theme', '  ', null, undefined, 5]) {
    same(`titleFor(${String(name)})`,
      cpA.default.titleFor(name), cpB.default.titleFor(name));
  }
  const presetCases = [
    null, [], ['#FF0000', '00ff00'], ['bad', '#GGG', '#123456'],
    [{ color: '#abcdef', name: 'X' }, { color: 'ffffff' }, { name: 'no colour' }],
    [{ color: 5 }], ['#12345', '#1234567']
  ];
  for (const presets of presetCases) {
    same(`normalizePresets(${JSON.stringify(presets)})`,
      cpA.default.normalizePresets(presets), cpB.default.normalizePresets(presets));
  }
  // the picker's markup, byte for byte
  resetBody('<div id="anchor-a"></div><div id="anchor-b"></div>');
  const a = new cpA.default('anchor-a', () => {}, { name: 'LED' });
  const b = new cpB.default('anchor-b', () => {}, { name: 'LED' });
  a.show();
  b.show();
  same('ColorPrompt markup',
    document.getElementById('anchor-a').innerHTML,
    document.getElementById('anchor-b').innerHTML);
  // and the swatch grid built from a palette
  a.setPresets(['#FF0000', { color: '#00FF00', name: 'Green' }]);
  b.setPresets(['#FF0000', { color: '#00FF00', name: 'Green' }]);
  a.buildPresetGrid();
  b.buildPresetGrid();
  same('preset grid', a.presetGridElement.outerHTML, b.presetGridElement.outerHTML);
  a.updateStateFromHex('#00FF00', true, false);
  b.updateStateFromHex('#00FF00', true, false);
  same('picker state after hex',
    [a.currentHue, a.currentSaturation, a.currentValue, a.hexInputElement.value,
     a.hexInputElement.size, a.hexLabelElement.innerText,
     a.colorPointerElement.style.left, a.colorPointerElement.style.top],
    [b.currentHue, b.currentSaturation, b.currentValue, b.hexInputElement.value,
     b.hexInputElement.size, b.hexLabelElement.innerText,
     b.colorPointerElement.style.left, b.colorPointerElement.style.top]);
  a.destroy();
  b.destroy();
}

//// generated markup: toggle switch, status light, lists window ////

const swA = loaded['before:switch.js'];
const swB = loaded['after:switch.js'];
if (swA && swB) {
  for (const config of [{ id: 'x', checked: true }, { checked: false },
      { id: 'y', width: '54px', height: '26px' }]) {
    resetBody('<div id="pa"></div><div id="pb"></div>');
    const one = new swA.default(document.getElementById('pa'), config);
    const two = new swB.default(document.getElementById('pb'), config);
    // the generated id is random when no id is given, so it is levelled here
    const markupA = one.wrapper.outerHTML.replaceAll(one.toggleId, 'ID');
    const markupB = two.wrapper.outerHTML.replaceAll(two.toggleId, 'ID');
    same(`ToggleSwitch markup ${JSON.stringify(config)}`, markupA, markupB);
    // the payload a toggle sends
    const seenA = [];
    const seenB = [];
    one.onToggleCallback = (p) => seenA.push(p);
    two.onToggleCallback = (p) => seenB.push(p);
    one.activate();
    two.activate();
    one.checkbox.checked = !one.checkbox.checked;
    two.checkbox.checked = !two.checkbox.checked;
    one.checkbox.dispatchEvent(new window.Event('change'));
    two.checkbox.dispatchEvent(new window.Event('change'));
    same(`ToggleSwitch payload ${JSON.stringify(config)}`, seenA, seenB);
    one.destroy();
    two.destroy();
  }
}

const rsA = loaded['before:roomstate.js'];
const rsB = loaded['after:roomstate.js'];
if (rsA && rsB) {
  resetBody('<div id="anchor-a"></div><div id="anchor-b"></div>');
  global.App = { currentRoom: 'lr' };
  window.App = global.App;
  const one = new rsA.default('anchor-a');
  const two = new rsB.default('anchor-b');
  same('RoomStateManager markup',
    document.getElementById('anchor-a').innerHTML,
    document.getElementById('anchor-b').innerHTML);
  // the rows the status dropdown draws
  const devices = [
    { name: 'Lamp', status: true }, { name: 'PC', status: false },
    { name: 'Amp', status: null }
  ];
  one.roomState = true; one.devices = devices;
  two.roomState = true; two.devices = devices;
  same('status rows', one.statusMenuData(), two.statusMenuData());
  one.roomState = null;
  two.roomState = null;
  same('status rows error', one.statusMenuData(), two.statusMenuData());
  one.destroy();
  two.destroy();
}

process.exit(report('frontend modules') > 0 ? 1 : 0);
