// utils.js: before vs after
import { setupDom, resetBody, loadPair, check, same, sameCall, report,
  SAMPLE_STRINGS, makeRng, randomHex } from './env.mjs';

function attemptValue(fn) {
  try {
    return fn();
  } catch (error) {
    return null;
  }
}

setupDom();
const { before, after } = await loadPair('utils.js');

console.error('STAGE pure text helpers');
//// pure text helpers ////

const textCases = SAMPLE_STRINGS.concat([
  '"a|b|c\\nd|e|f"', 'a|b\nc|d\n', '#FF0000|Red\n#00FF00|Green\n',
  'x|Name\n\n|Blank\n', '"#123456|One"', 'nohash|Two\n'
]);

for (const text of textCases) {
  same(`splitMenuData(${JSON.stringify(text)})`,
    before.splitMenuData(text), after.splitMenuData(text));
  same(`parsePalette(${JSON.stringify(text)})`,
    before.parsePalette(text), after.parsePalette(text));
  same(`cleanString(${JSON.stringify(text)})`,
    before.cleanString(text), after.cleanString(text));
  same(`isNumeric(${JSON.stringify(text)})`,
    before.isNumeric(text), after.isNumeric(text));
  same(`searchTerm(${JSON.stringify(text)})`,
    before.searchTerm(text), after.searchTerm(text));
  same(`splitByDelimiter(${JSON.stringify(text)})`,
    before.splitByDelimiter(text), after.splitByDelimiter(text));
  same(`splitByDelimiter~(${JSON.stringify(text)})`,
    before.splitByDelimiter(text, '~'), after.splitByDelimiter(text, '~'));
  same(`checkPassword(${JSON.stringify(text)})`,
    before.checkPassword(text), after.checkPassword(text));
  same(`isCryptBlob(${JSON.stringify(text)})`,
    before.isCryptBlob(text), after.isCryptBlob(text));
}

for (const value of [null, undefined, 0, 12, {}, []]) {
  sameCall(`cleanString(${String(value)})`,
    () => before.cleanString(value), () => after.cleanString(value));
  sameCall(`isNumeric(${String(value)})`,
    () => before.isNumeric(value), () => after.isNumeric(value));
  sameCall(`searchTerm(${String(value)})`,
    () => before.searchTerm(value), () => after.searchTerm(value));
  sameCall(`parsePalette(${String(value)})`,
    () => before.parsePalette(value), () => after.parsePalette(value));
  sameCall(`checkPassword(${String(value)})`,
    () => before.checkPassword(value), () => after.checkPassword(value));
  sameCall(`isCryptBlob(${String(value)})`,
    () => before.isCryptBlob(value), () => after.isCryptBlob(value));
}

for (const text of SAMPLE_STRINGS) {
  for (const term of ['', 'a', 'ab', 'x', 'caf', 'PIPE']) {
    same(`searchMatches(${JSON.stringify(text)},${term})`,
      before.searchMatches(text, term), after.searchMatches(text, term));
  }
}
same('searchMatches(null)', before.searchMatches(null, 'a'), after.searchMatches(null, 'a'));
same('searchMatches(undef)', before.searchMatches(undefined, 'a'),
  after.searchMatches(undefined, 'a'));

console.error('STAGE base64');
//// base64 ////

for (const text of ['', 'a', 'ab', 'abc', 'Hello?World+/=', '\u00ff\u00fe', '\u4f60']) {
  sameCall(`b64enc(${JSON.stringify(text)})`,
    () => before.base64URLSafeEncode(text), () => after.base64URLSafeEncode(text));
  const encoded = attemptValue(() => before.base64URLSafeEncode(text));
  if (encoded !== null) {
    same(`b64std(${encoded})`,
      before.base64URLSafeToStandard(encoded), after.base64URLSafeToStandard(encoded));
  }
}
same('b64std("")', before.base64URLSafeToStandard(''), after.base64URLSafeToStandard(''));
same('b64std(null)', before.base64URLSafeToStandard(null), after.base64URLSafeToStandard(null));

const rng = makeRng(7);
for (let i = 0; i < 200; i++) {
  const len = Math.floor(rng() * 40);
  const bytes = new Uint8Array(len);
  for (let b = 0; b < len; b++) bytes[b] = Math.floor(rng() * 256);
  same(`bytesToB64#${i}`, before.bytesToB64(bytes), after.bytesToB64(bytes));
  const encoded = before.bytesToB64(bytes);
  same(`b64ToBytes#${i}`,
    [...before.b64ToBytes(encoded)], [...after.b64ToBytes(encoded)]);
}

console.error('STAGE colour maths');
//// colour maths ////

const rng2 = makeRng(99);
for (let i = 0; i < 600; i++) {
  const hex = randomHex(rng2);
  same(`hexToRgb(${hex})`, before.hexToRgb(hex), after.hexToRgb(hex));
  same(`hexToRgb-nohash(${hex})`,
    before.hexToRgb(hex.slice(1)), after.hexToRgb(hex.slice(1)));
  const rgb = before.hexToRgb(hex);
  same(`rgbToHex(${hex})`,
    before.rgbToHex(rgb.red, rgb.green, rgb.blue),
    after.rgbToHex(rgb.red, rgb.green, rgb.blue));
  const style = `rgb(${rgb.red}, ${rgb.green}, ${rgb.blue})`;
  same(`rgbToHexFromStyle(${style})`,
    before.rgbToHexFromStyle(style), after.rgbToHexFromStyle(style));
  same(`rgbToHsv(${hex})`,
    before.rgbToHsv(rgb.red, rgb.green, rgb.blue),
    after.rgbToHsv(rgb.red, rgb.green, rgb.blue));
  const hsv = before.rgbToHsv(rgb.red, rgb.green, rgb.blue);
  same(`hsvToRgb(${hex})`,
    before.hsvToRgb(hsv.hue, hsv.saturation, hsv.value),
    after.hsvToRgb(hsv.hue, hsv.saturation, hsv.value));
}
for (const style of ['', 'rgb(1,2)', 'transparent', 'rgb(0, 0, 0)', 'rgba(1,2,3,0.5)']) {
  same(`rgbToHexFromStyle(${style})`,
    before.rgbToHexFromStyle(style), after.rgbToHexFromStyle(style));
}
// hue sweep including the wrap points and out of range values
for (let hue = -720; hue <= 720; hue += 1) {
  for (const sat of [0, 0.33, 1]) {
    for (const val of [0, 0.5, 1]) {
      same(`hsvToRgb(${hue},${sat},${val})`,
        before.hsvToRgb(hue, sat, val), after.hsvToRgb(hue, sat, val));
    }
  }
}
for (let i = 0; i < 500; i++) {
  const hue = rng2() * 400 - 20;
  const sat = rng2();
  const val = rng2();
  same(`hsvToRgb-rand#${i}`,
    before.hsvToRgb(hue, sat, val), after.hsvToRgb(hue, sat, val));
}

console.error('STAGE mapNumber');
//// mapNumber ////

for (let i = 0; i < 300; i++) {
  const num = rng2() * 300 - 50;
  same(`mapNumber#${i}`,
    before.mapNumber(num, 0, 192, 0, 100), after.mapNumber(num, 0, 192, 0, 100));
  same(`mapNumberB#${i}`,
    before.mapNumber(num, 10, 90, -5, 5), after.mapNumber(num, 10, 90, -5, 5));
}

console.error('STAGE intervals');
//// intervals ////

same('refreshInterval', before.refreshInterval(), after.refreshInterval());
same('timeoutInterval', before.timeoutInterval(), after.timeoutInterval());
same('lock constants',
  [before.LOCK_EXPIRE_MS, before.LOCK_BEAT_MS, before.LOCK_SETTLE_MS, before.LOCK_TAG],
  [after.LOCK_EXPIRE_MS, after.LOCK_BEAT_MS, after.LOCK_SETTLE_MS, after.LOCK_TAG]);
same('crypt constants',
  [before.CRYPT_TAG, before.CRYPT_ITERATIONS, before.CRYPT_SALT_BYTES,
   before.CRYPT_IV_BYTES, before.CRYPT_KEY_BYTES, before.CRYPT_MIN_PASSWORD,
   before.CRYPT_MAX_PASSWORD, before.CRYPT_REC_TAG],
  [after.CRYPT_TAG, after.CRYPT_ITERATIONS, after.CRYPT_SALT_BYTES,
   after.CRYPT_IV_BYTES, after.CRYPT_KEY_BYTES, after.CRYPT_MIN_PASSWORD,
   after.CRYPT_MAX_PASSWORD, after.CRYPT_REC_TAG]);
same('search constants',
  [before.SEARCH_ROW_CLASS, before.SEARCH_ICON_CLASS, before.SEARCH_CLEAR_CLASS,
   before.SEARCH_BOX_CLASS, before.SEARCH_GLASS_ICON, before.SEARCH_CLEAR_ICON,
   before.SEARCH_PLACEHOLDER, before.DRAG_GRIP_CLASS, before.DRAG_GRIP_OFF_CLASS,
   before.DRAG_GRIP_HTML],
  [after.SEARCH_ROW_CLASS, after.SEARCH_ICON_CLASS, after.SEARCH_CLEAR_CLASS,
   after.SEARCH_BOX_CLASS, after.SEARCH_GLASS_ICON, after.SEARCH_CLEAR_ICON,
   after.SEARCH_PLACEHOLDER, after.DRAG_GRIP_CLASS, after.DRAG_GRIP_OFF_CLASS,
   after.DRAG_GRIP_HTML]);

console.error('STAGE crypt record');
//// crypt record ////

const recordCases = [
  '', 'V1|salt|310000|check\n', 'V1|salt|999|check\n', 'V1||310000|check\n',
  'V1|salt|310000|\n', 'V2|salt|310000|check\n', 'V1|s|abc|c\n',
  'V1|a|310000|b\nV1|c|400000|d\n', 'junk\nV1|a|310000|b\n', 'V1|a|310000\n'
];
for (const text of recordCases) {
  same(`parseCryptRecord(${JSON.stringify(text)})`,
    before.parseCryptRecord(text), after.parseCryptRecord(text));
  const slots = before.parseCryptRecord(text);
  same(`buildCryptRecord(${JSON.stringify(text)})`,
    before.buildCryptRecord(slots), after.buildCryptRecord(slots));
}
same('cryptContext', before.cryptContext('lists.save', 3),
  after.cryptContext('lists.save', 3));

console.error('STAGE html tag function');
//// html tag function ////

const name = 'Bob';
same('html tag',
  before.html`<div id="${name}">  <span> x </span>   </div>`,
  after.html`<div id="${name}">  <span> x </span>   </div>`);
same('html tag 2', before.html`  <a>${1}</a>  `, after.html`  <a>${1}</a>  `);

console.error('STAGE fileURL');
//// fileURL ////

same('fileURL read', before.fileURL('read', 'lists.save'), after.fileURL('read', 'lists.save'));
same('fileURL write', before.fileURL('write', 'a.save'), after.fileURL('write', 'a.save'));

console.error('STAGE sendCmd URL construction');
//// sendCmd URL construction ////

const seen = { before: [], after: [] };
for (const side of ['before', 'after']) {
  const mod = side === 'before' ? before : after;
  global.fetch = async (url) => {
    seen[side].push(url);
    return { ok: true, json: async () => "ok", text: async () => "ok" };
  };
  await mod.sendCmd('main', 'brxmit', 'brtvon');
  await mod.sendCmd('leds', 'textscroll', 'a b+c=d&e#f?g%h');
  await mod.sendCmd('main', null, undefined);
  await mod.sendCmd('main', 'status');
  await mod.readFile('lists.save');
  await mod.writeFile('lists.save', 'body');
}
same('sendCmd/readFile/writeFile URLs', seen.before, seen.after);

console.error('STAGE savePOST behaviour');
//// savePOST behaviour ////

for (const side of ['before', 'after']) {
  const mod = side === 'before' ? before : after;
  const sent = [];
  global.fetch = async (url, opts) => {
    sent.push([url, opts && opts.method, opts && opts.body,
      opts && opts.headers && opts.headers['Content-Type']]);
    return { ok: true, json: async () => "", text: async () => "" };
  };
  global.XMLHttpRequest = class {
    open(method, url) { this._m = method; this._u = url; }
    setRequestHeader(k, v) { this._h = [k, v]; }
    send(body) {
      sent.push([this._u, this._m, String(body), this._h[1]]);
      this.readyState = 4;
      this.status = 200;
      if (this.onreadystatechange) this.onreadystatechange();
    }
  };
  global.XMLHttpRequest.DONE = 4;
  let got = null;
  mod.savePOST('pwd', ['secret'], (file) => { got = file; });
  await new Promise(r => setTimeout(r, 10));
  seen[side] = { sent: sent, got: got };
}
same('savePOST request', seen.before, seen.after);

console.error('STAGE FileLock row handling');
//// FileLock row handling ////

const lockCases = [
  '', 'abc|123\n', '|0\n', 'abc|notanumber\n', 'abc\n', '\n\n',
  '"abc|123"', 'tok|' + Date.now() + '\n', 'old|1\n'
];
for (const text of lockCases) {
  const lockA = new before.FileLock({ file: 'x' });
  const lockB = new after.FileLock({ file: 'x' });
  lockB.token = lockA.token;
  same(`FileLock.parse(${JSON.stringify(text)})`, lockA.parse(text), lockB.parse(text));
  same(`FileLock.busy(${JSON.stringify(text)})`, lockA.busy(text), lockB.busy(text));
  same(`FileLock.line`, lockA.line(false), lockB.line(false));
}
{
  const lockA = new before.FileLock({ file: 'x' });
  const lockB = new after.FileLock({ file: 'x' });
  lockB.token = lockA.token;
  same('FileLock.busy(null)', lockA.busy(null), lockB.busy(null));
  same('FileLock.busy(own)', lockA.busy(lockA.line(true)), lockB.busy(lockB.line(true)));
  same('FileLock defaults',
    [lockA.owned, lockA.beatTimer, lockA.confirming, typeof lockA.onLost],
    [lockB.owned, lockB.beatTimer, lockB.confirming, typeof lockB.onLost]);
}

console.error('STAGE crypto round trip');
//// crypto round trip ////

for (const mod of [before, after]) {
  const key = await crypto.subtle.importKey('raw', new Uint8Array(32),
    { name: 'AES-GCM' }, false, ['encrypt', 'decrypt']);
  const blob = await mod.encryptText(key, 'hello there', mod.cryptContext('lists.save', 1));
  check('encryptText shape', mod.isCryptBlob(blob), blob);
  const out = await mod.decryptText(key, blob, mod.cryptContext('lists.save', 1));
  check('decryptText round trip', out === 'hello there', out);
  const wrong = await mod.decryptText(key, blob, mod.cryptContext('lists.save', 2));
  check('decryptText wrong context', wrong === null, String(wrong));
  const derived = await mod.deriveCryptKeys('password12', 'AAAAAAAAAAAAAAAAAAAAAA@@', 1000);
  check('deriveCryptKeys verifier', typeof derived.verifier === 'string');
  const hash = await mod.hashVerifier(derived.verifier);
  check('hashVerifier', typeof hash === 'string' && hash.length > 0);
}
// both sides derive the same key material from the same inputs
{
  const a = await before.deriveCryptKeys('password12', 'AAAAAAAAAAAAAAAAAAAAAA@@', 1000);
  const b = await after.deriveCryptKeys('password12', 'AAAAAAAAAAAAAAAAAAAAAA@@', 1000);
  same('deriveCryptKeys verifier match', a.verifier, b.verifier);
  same('hashVerifier match', await before.hashVerifier(a.verifier),
    await after.hashVerifier(b.verifier));
  const blob = await before.encryptText(a.key, 'cross', 'ctx');
  same('cross decrypt', await after.decryptText(b.key, blob, 'ctx'), 'cross');
}

console.error('STAGE DOM helpers');
//// DOM helpers ////

resetBody('<div id="a" class="grp"></div><div id="b" class="grp"></div>');
before.classDisplay('grp', 'none');
same('classDisplay before', document.getElementById('a').style.display, 'none');
after.classDisplay('grp', 'block');
same('classDisplay after', document.getElementById('a').style.display, 'block');

resetBody('<div id="a"></div>');
before.addClassToElem('add', 'a', 'x');
before.addClassToElem('add', 'a', 'x');
const afterAdd = document.getElementById('a').className;
resetBody('<div id="a"></div>');
after.addClassToElem('add', 'a', 'x');
after.addClassToElem('add', 'a', 'x');
same('addClassToElem add', afterAdd, document.getElementById('a').className);
after.addClassToElem('remove', 'a', 'x');
same('addClassToElem remove', document.getElementById('a').className, '');

resetBody('<div id="w"></div>');
before.removeWindow('w');
same('removeWindow', document.getElementById('w'), null);
before.removeWindow('nothere');
after.removeWindow('nothere');

console.error('STAGE search row markup');
//// search row markup ////

for (const options of [
  { tag: 'a', id: 'r1', partClass: 'nohide', boxId: 'b1' },
  { rowClass: 'list__search', boxId: 'b2', maxLength: 64, value: 'abc' },
  {}
]) {
  resetBody('');
  const rowA = before.searchRowElement(options);
  const rowB = after.searchRowElement(options);
  same(`searchRowElement(${JSON.stringify(options)})`, rowA.outerHTML, rowB.outerHTML);
}
// clearing and typing behave the same
for (const mod of [before, after]) {
  resetBody('');
  const seenValues = [];
  const row = mod.searchRowElement({ boxId: 'sb', value: 'hi',
    onSearch: (value) => seenValues.push(value) });
  document.body.appendChild(row);
  const box = row.querySelector('input');
  const icon = row.querySelector('i');
  box.value = 'hello';
  box.dispatchEvent(new window.Event('input'));
  icon.dispatchEvent(new window.MouseEvent('click', { bubbles: true }));
  seen[mod === before ? 'before' : 'after'] =
    { values: seenValues, icon: icon.className, box: box.value };
}
same('searchRow interaction', seen.before, seen.after);

console.error('STAGE drag grip');
//// drag grip ////

resetBody('');
same('dragGripElement', before.dragGripElement().outerHTML, after.dragGripElement().outerHTML);
same('dragGripElement fixed',
  before.dragGripElement(true).outerHTML, after.dragGripElement(true).outerHTML);

console.error('STAGE DragSort re-order');
//// DragSort re-order ////

function dragScenario(mod) {
  resetBody('');
  const box = document.createElement('div');
  ['a', 'b', 'c', 'd'].forEach(id => {
    const row = document.createElement('div');
    row.className = 'r';
    row.dataset.id = id;
    row.appendChild(mod.dragGripElement(id === 'd'));
    Object.defineProperty(row, 'offsetHeight', { value: 20, configurable: true });
    box.appendChild(row);
  });
  document.body.appendChild(box);
  let drops = 0;
  const sorter = new mod.DragSort({ rowClass: 'r', onDrop: () => { drops++; } });
  const grip = box.children[0].querySelector('.drag-grip');
  const makeEvent = (y) => ({
    target: grip, pointerId: 1, clientY: y, preventDefault() {}
  });
  const started = sorter.start(makeEvent(0));
  sorter.move(makeEvent(35));
  sorter.move(makeEvent(75));
  sorter.end({ pointerId: 1 });
  const order = [...box.children].map(row => row.dataset.id).join(',');
  return { started, order, drops, dragging: sorter.isDragging() };
}
same('DragSort scenario', dragScenario(before), dragScenario(after));

console.error('STAGE popup');
//// popup ////

for (const mod of [before, after]) {
  resetBody('');
  mod.showPopup('hello <b>world</b>');
  const first = document.body.innerHTML;
  mod.showPopup('second');
  seen[mod === before ? 'before' : 'after'] = { first, second: document.body.innerHTML };
  mod.clearPopupTimer();
}
same('showPopup markup', seen.before, seen.after);

console.error('STAGE removed dead exports are unused elsewhere');
//// removed dead exports are unused elsewhere ////

check('checkElemIsVisibleByID dropped', after.checkElemIsVisibleByID === undefined);
check('scrollToBottom dropped', after.scrollToBottom === undefined);

process.exit(report('utils.js') > 0 ? 1 : 0);
