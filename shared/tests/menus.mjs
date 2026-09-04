// Draws every dynamic menu row type, and the lists window, from one side and
// prints the markup so the two can be diffed.
import { ensureWifiStub, setupDom, ORIG, NEW } from './env.mjs';
import { pathToFileURL } from 'node:url';

const root = process.argv[2] === 'after' ? NEW : ORIG;
ensureWifiStub();
const win = setupDom();
Object.defineProperty(global, 'App', {
  get: () => win.App, set: (value) => { win.App = value; }, configurable: true
});
await import(pathToFileURL(root + 'main.js').href);
win.document.dispatchEvent(new win.Event('DOMContentLoaded', { bubbles: true }));
await new Promise(r => setTimeout(r, 60));

const out = {};

// every row type drawMenu() knows, drawn into a container of its own
const menuCases = {
  links: 'http://a.home|link|LEDstrip\nhttp://b.home|link|LEDgrid\n',
  indicators: 'cmd~lrxmit~usb|grnind|USB\ncmd~lrxmit~aux|blkind|Aux\n' +
    'x|redind|Error\nx|ylwind|Warn\nx|noind|Muted\n',
  toggles: '1~lrxmit~clr1|toggle|\n0~brxmit~brtv|toggle|\n',
  sliders: '128~lrxmit~wl1_dual|slider|\n7~brxmit~bed_leds|slider|\n',
  subs: 'subs|subs|Subs Gain\n',
  stars: 'stars|chk|fad fa-stars\n',
  rooms: 'br|room|Bedroom\n',
  relax: 'rain|relaxmenu|Rain\nfan|relaxactive|Fan\nx|relaxstop|Stop\n',
  leds: 'textscroll|ledeffectmenu|Textscroll\nflames|ledeffectmenu|Flames\n',
  unknown: 'x|nosuchtype|Ignored\n'
};
for (const [name, data] of Object.entries(menuCases)) {
  const box = win.document.createElement('div');
  box.id = `case-${name}`;
  win.document.body.appendChild(box);
  win.drawMenu(data, box.id);
  out[name] = box.innerHTML;
  // the switches a menu built are torn down with it
  win.removeDropdowns();
}
// two column mode, as the LED colour / effect file menus use
{
  const box = win.document.createElement('div');
  box.id = 'case-twocol';
  win.document.body.appendChild(box);
  win.drawMenu('candle|Candle\nhalogen|Halogen\n', box.id,
    { TwoColumns: true, MenuScroll: false });
  out.twocol = box.innerHTML;
  win.removeDropdowns();
}

// the lists window: drawn, then filled with a parsed file
const listsMod = await import(pathToFileURL(root + 'lists.js').href);
const lists = new listsMod.default();
lists.drawWindow();
out.listsWindow = win.document.getElementById('list__win').outerHTML;
lists.store.data = lists.store.parse(
  '0|Shopping|0|Milk|0\n0|Shopping|1|Eggs|1\n1|Empty|||0\n2|!ENC|0|E1.AA.BB|0\n');
lists.store.loaded = true;
lists.settling = false;
lists.render();
out.listsRows = win.document.getElementById('list__list').innerHTML;
lists.showItems(0);
out.listsItems = win.document.getElementById('list__list').innerHTML;
lists.setMode(1); // select mode
out.listsSelect = win.document.getElementById('list__list').innerHTML;
lists.setMode(0);
lists.showLists();
lists.newId = 1; // the row the encryption lock is offered on
lists.render();
out.listsNewRow = win.document.getElementById('list__list').innerHTML;
out.listsTotals = [lists.totalText(1, 'item'), lists.totalText(4, 'total item')];

// the bookmarks menu, drawn from a file
const bmMod = await import(pathToFileURL(root + 'bookmarks.js').href);
const bookmarks = new bmMod.default({ removeDropdowns: () => {} });
bookmarks.drawMenu('https://a.test|Alpha|iVBORw0KGgoAAA\n' +
  'https://b.test|Beta\nLOCK|1|tok|9|0\n');
out.bookmarkRows = win.document.getElementById('bookmarks').innerHTML;
out.bookmarkBody = bookmarks.buildBody();
out.bookmarkFromText = bookmarks.bodyFromText('https://a.test|Alpha|xyz\n');
bookmarks.drawPrompt(true, null, null, bookmarks.items()[0]);
out.bookmarkPrompt = win.document.getElementById('editFav__prompt').outerHTML;

console.log(JSON.stringify(out, null, 1));
process.exit(0);
