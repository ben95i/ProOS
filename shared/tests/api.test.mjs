// API modules: before vs after. Internal helpers are reached by loading a
// copy of each file with its private functions added to module.exports.
import { check, same, report, makeRng } from './env.mjs';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
mkdirSync('/tmp/apitest/before', { recursive: true });
mkdirSync('/tmp/apitest/after', { recursive: true });

// loads one API file with extra names exported, so private helpers can be
// compared without changing the shipped file
function loadWith(side, file, names) {
  const root = side === 'before' ? '/home/claude/orig/api/' : '/home/claude/new/api/';
  const src = readFileSync(root + file, 'utf8');
  const extra = names.map(n => `try { module.exports.${n} = ${n}; } catch (e) {}`).join('\n');
  const target = `/tmp/apitest/${side}/probe_${file}`;
  writeFileSync(target, src + '\n' + extra + '\n');
  return require(target);
}

// the real deps live beside the originals, so they are copied in too
for (const side of ['before', 'after']) {
  for (const dep of ['favicon.js', 'ledparser.js', 'statusupd.js', 'sitelookup.js',
      'netscan.js', 'weather.js']) {
    const root = side === 'before' ? '/home/claude/orig/api/' : '/home/claude/new/api/';
    writeFileSync(`/tmp/apitest/${side}/${dep}`, readFileSync(root + dep, 'utf8'));
  }
  writeFileSync(`/tmp/apitest/${side}/weather.json`,
    readFileSync('/home/claude/orig/api/weather.json', 'utf8'));
}

//// controller helpers ////

const ctlNames = ['isNumeric', 'splitByDelimiter', 'padOctet', 'processProgramError',
  'extractResponse', 'decodeXmitCmd', 'parseInputData', 'RoomManager'];
const ctlA = loadWith('before', 'controller.js', ctlNames);
const ctlB = loadWith('after', 'controller.js', ctlNames);

const strings = ['', 'a', '0', '12', ' 3 ', 'a-b-c', 'x~y', 'a|b|c',
  '"quoted"', 'line\\nbreak', 'bed_leds_set_120', 'wl1_dual_set_7', 'plain'];
for (const s of strings) {
  same(`isNumeric(${s})`, ctlA.isNumeric(s), ctlB.isNumeric(s));
  same(`splitByDelimiter(${s})`, ctlA.splitByDelimiter(s), ctlB.splitByDelimiter(s));
  same(`splitByDelimiter~(${s})`,
    ctlA.splitByDelimiter(s, '~'), ctlB.splitByDelimiter(s, '~'));
  same(`padOctet(${s})`, ctlA.padOctet(s), ctlB.padOctet(s));
  same(`parseInputData(${s})`,
    ctlA.parseInputData(s, '_set'), ctlB.parseInputData(s, '_set'));
  for (const delim of ['', '|', '~']) {
    same(`extractResponse(${s},${delim})`,
      ctlA.extractResponse(s, delim), ctlB.extractResponse(s, delim));
  }
  for (const pos of ['', '0', '1', '2']) {
    same(`decodeXmitCmd(${s},${pos})`,
      ctlA.decodeXmitCmd(s, pos), ctlB.decodeXmitCmd(s, pos));
  }
}
for (const value of [0, 7, 120, 255]) {
  same(`padOctet(${value})`, ctlA.padOctet(value), ctlB.padOctet(value));
}
for (const err of [null, 'text', {}, { code: 1, signal: 'SIGT', message: 'm',
    stderr: ' e ', stdout: ' o ' }]) {
  same(`processProgramError`, ctlA.processProgramError(err), ctlB.processProgramError(err));
}
// RoomManager over the same readings
for (const readings of [
  [['Lamp', '1'], ['PC', '0'], ['Amp', 'text']],
  [['A', 0], ['B', '0']],
  [['A', '9']],
  []
]) {
  const one = new ctlA.RoomManager();
  const two = new ctlB.RoomManager();
  readings.forEach(([n, v]) => { one.addDevice(n, v); two.addDevice(n, v); });
  one.updateRoomState();
  two.updateRoomState();
  same(`RoomManager ${JSON.stringify(readings)}`, one.getData(), two.getData());
}

//// executeMain dispatch, with the network stubbed out ////

for (const [side, mod] of [['before', ctlA], ['after', ctlB]]) {
  const calls = [];
  global.fetch = async (url) => {
    calls.push(url);
    return { ok: true, text: async () => '|1|' };
  };
  const out = [];
  for (const [a, b] of [['brxmit', 'brtvon'], ['brxmit', 'bed_leds_set_7'],
      ['brxmit', 'nosuch'], ['lrxmit', 'volup'], ['lrxmit', 'wl1_led2on'],
      ['lrxmit', 'nosuch'], ['nosuchcmd', ''], ['brpi', 'ampstate'],
      ['relaxmenu', ''], ['lrlightson', '']]) {
    out.push([a, b, await mod.executeMain('/tmp/apitest', a, b)]);
  }
  await new Promise(r => setTimeout(r, 20));
  if (side === 'before') { global.__before = { out, calls: calls.slice().sort() }; }
  else { same('executeMain dispatch', global.__before, { out, calls: calls.slice().sort() }); }
}

//// statusupd ////

const stA = loadWith('before', 'statusupd.js', ['parseEntries']);
const stB = loadWith('after', 'statusupd.js', ['parseEntries']);
for (const data of ['', 'a.home|A|ssh\n', '# comment\nb.home|B|ping\n\n',
    'bad line\n', 'c.home|C|\n', '  d.home | D | ssh  \n']) {
  same(`parseEntries(${JSON.stringify(data)})`,
    stA.parseEntries(data), stB.parseEntries(data));
}

//// netscan ////

const nsNames = ['formatCol', 'parseNmapOutput', 'compareByAddress', 'deviceRow'];
const nsA = loadWith('before', 'netscan.js', ['formatCol']);
const nsB = loadWith('after', 'netscan.js', nsNames);
for (const [text, width] of [['', 5], ['abc', 5], ['abcdefgh', 5], [null, 4], [12, 6]]) {
  same(`formatCol(${text},${width})`, nsA.formatCol(text, width), nsB.formatCol(text, width));
}

//// weather ////

const wxA = loadWith('before', 'weather.js', ['decodeWeatherCode']);
const wxB = loadWith('after', 'weather.js', ['decodeWeatherCode', 'decodeTimeOfDay']);
for (let code = -2; code <= 105; code++) {
  same(`decodeWeatherCode(${code})`,
    wxA.decodeWeatherCode(code), wxB.decodeWeatherCode(code));
}

//// sitelookup title extraction ////

const slB = loadWith('after', 'sitelookup.js', ['extractTitle']);
const titleCases = [
  '<title>Hello</title>', '<title >  Spaced \n Out </title>',
  '<TITLE>Caps</TITLE>', '<title></title>', 'no title here',
  '<title>A &amp; B &lt;x&gt; &quot;q&quot; &#39;s&#39;</title>',
  '<title id="t">Attrs</title>'
];
for (const html of titleCases) {
  // the original does the same work inline, so it is compared through the
  // whole lookup with the network stubbed
  global.fetch = async () => ({ ok: true, text: async () => html });
  const slA = loadWith('before', 'sitelookup.js', []);
  const before = await slA.runSiteLookup(Buffer.from('https://x.test').toString('base64'));
  const after = await slB.runSiteLookup(Buffer.from('https://x.test').toString('base64'));
  same(`runSiteLookup title ${JSON.stringify(html)}`, before.title, after.title);
}
// the hard-coded carriers, and a bad URL
for (const url of ['https://www.ups.com/track', 'https://tools.usps.com/x',
    'https://ups.com/a', 'not a url', '']) {
  global.fetch = async () => ({ ok: false, text: async () => '' });
  const slA = loadWith('before', 'sitelookup.js', []);
  const before = await slA.runSiteLookup(Buffer.from(url).toString('base64'));
  const after = await slB.runSiteLookup(Buffer.from(url).toString('base64'));
  same(`runSiteLookup ${url}`, before, after);
}

//// favicon: the PNG bytes must be identical ////

const fvNames = ['iconCandidates', 'quantizeColors', 'resizeToIcon', 'encodeIndexedPNG',
  'crc32', 'backingColor', 'decodeICOBitmap', 'base64URLSafe'];
const fvA = loadWith('before', 'favicon.js', fvNames);
const fvB = loadWith('after', 'favicon.js', fvNames);

const pageHTML = `
<link rel="icon" href="/a.png" sizes="16">
<link rel="shortcut icon" type="image/x-icon" href="b.ico">
<link rel="apple-touch-icon" href="/big.png" sizes="180">
<link rel="icon" type="image/svg+xml" href="/v.svg">
<link rel="stylesheet" href="/s.css">
<link rel="icon">
<link rel="icon" href="::bad::">`;
same('iconCandidates', fvA.iconCandidates(pageHTML, 'https://x.test/p/'),
  fvB.iconCandidates(pageHTML, 'https://x.test/p/'));
same('iconCandidates empty', fvA.iconCandidates('', 'https://x.test/'),
  fvB.iconCandidates('', 'https://x.test/'));

const rng = makeRng(31);
for (let round = 0; round < 8; round++) {
  const w = 8 + Math.floor(rng() * 40);
  const h = 8 + Math.floor(rng() * 40);
  const data = new Uint8Array(w * h * 4);
  for (let i = 0; i < data.length; i++) data[i] = Math.floor(rng() * 256);
  const image = { width: w, height: h, data: data };
  same(`backingColor#${round}`, fvA.backingColor(image), fvB.backingColor(image));
  const rgbaA = fvA.resizeToIcon(image);
  const rgbaB = fvB.resizeToIcon(image);
  same(`resizeToIcon#${round}`, [...rgbaA], [...rgbaB]);
  const qA = fvA.quantizeColors(rgbaA);
  const qB = fvB.quantizeColors(rgbaB);
  same(`quantizeColors#${round}`, qA, qB);
  const pngA = fvA.encodeIndexedPNG(qA);
  const pngB = fvB.encodeIndexedPNG(qB);
  check(`encodeIndexedPNG#${round}`, pngA.equals(pngB),
    `${pngA.length} vs ${pngB.length} bytes`);
  same(`base64URLSafe#${round}`, fvA.base64URLSafe(pngA), fvB.base64URLSafe(pngB));
}
// a flat image, which drives the 4-bit palette path
{
  const flat = { width: 16, height: 16, data: new Uint8Array(16 * 16 * 4).fill(200) };
  const qA = fvA.quantizeColors(fvA.resizeToIcon(flat));
  const qB = fvB.quantizeColors(fvB.resizeToIcon(flat));
  same('quantize flat colors', qA.colors, qB.colors);
  check('encode flat PNG', fvA.encodeIndexedPNG(qA).equals(fvB.encodeIndexedPNG(qB)));
}

//// ledparser host dispatch ////

mkdirSync('/tmp/apitest/menus', { recursive: true });
writeFileSync('/tmp/apitest/menus/mainmenu.txt',
  'http://ledstrip.home:80|link|LEDstrip\nhttp://ledgrid.home:80|link|LEDgrid\n\n');
const lpA = require('/tmp/apitest/before/ledparser.js');
const lpB = require('/tmp/apitest/after/ledparser.js');
for (const [varA, varB] of [['abstract', ''], ['stop', ''], ['fc_leds', ''],
    ['textscroll', 'abc']]) {
  const seen = { before: [], after: [] };
  for (const [side, mod] of [['before', lpA], ['after', lpB]]) {
    global.fetch = async (url) => {
      seen[side].push(url);
      return { ok: true, text: async () => '"128"' };
    };
    seen[side + 'result'] = await mod.OPCLEDparser('/tmp/apitest', varA, varB);
  }
  same(`OPCLEDparser(${varA}) urls`, seen.before.sort(), seen.after.sort());
  same(`OPCLEDparser(${varA}) result`, seen.beforeresult, seen.afterresult);
}
// a missing menu file
same('OPCLEDparser missing file',
  await lpA.OPCLEDparser('/tmp/nowhere', 'stop'),
  await lpB.OPCLEDparser('/tmp/nowhere', 'stop'));

process.exit(report('api modules') > 0 ? 1 : 0);
