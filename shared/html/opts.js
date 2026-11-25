// Automate - site options: the names and switches a deploy may change, and the page's fixed
// menu lists, in one file that imports nothing, so any module can read them. www-minify.sh
// leaves it as written, so an installer can rewrite one of its lines with sed.
// by Ben Provenzano III

// true on a Pi that drives LED pixels: its page draws the Pixels group and the LED text
// scroller. The LED modules' installers (ledwall, ledgrid, ktchpi) rewrite this exact
// line to true under "## Enable Pixel Menu in Website"; every deploy brings it back as false.
export const LED_PI = false;

// the site's name: the server page's name, and the name in console logs and error popups
export const SITE_NAME = 'Automate';

// The page's name, shown as its title and in the About window. It is SITE_NAME on the
// server; a Pi's installer rewrites this line to its brand.txt name ('KitchenPi'), which
// makes the page a Pi's. PAGE_NAME === SITE_NAME is the server.
export const PAGE_NAME = SITE_NAME;

// the server's page, opened by a Pi menu's Home row
export const HOME_PAGE_URL = 'https://automate.home';

// The pages the link buttons open in a new tab, as GoToExtPage() takes them (no https://):
// GoToExtPage(EXT_PAGES.media)
export const EXT_PAGES = {
  media: 'media.home',
  youtube: 'youtube.com',
  unifi: 'unifi:11443/network/ff7gqlig/dashboard',
  proxmox: 'pve:8006',
  edgeswitch: 'edgeswitch.home',
  router: 'router.home'
};

// The Pixels group's Animated menu, in menu order, one [command, name] per effect: the
// command sent to the LED hosts and the name shown in the menu and the Pixels window.
// Every command is a video in rpi/effects/videos or an effect program run by
// rpi/ledparser.js; shuffle is picked on the server and textscroll opens the scroller.
export const LED_EFFECTS = [
  ['shuffle',    'Shuffle'],
  ['textscroll', 'Textscroll'],
  ['triangles',  'Triangles'],
  ['abstract',   'Abstract'],
  ['flames',     'Flames'],
  ['tunnel',     'Tunnel'],
  ['maze',       'Maze'],
  ['warp',       'Warp'],
  ['rays',       'Rays'],
  ['noise',      'Noise'],
  ['flurry',     'Flurry'],
  ['orbits',     'Orbits'],
  ['miami',      'Miami'],
  ['plasma',     'Plasma'],
  ['prism',      'Prism'],
  ['clouds',     'Clouds'],
  ['oscope',     'O-scope'],
  ['spectro',    'Spectro'],
  ['mario',      'Mario'],
  ['pokemon',    'Pokémon'],
  ['8bit',       '8-Bit'],
  ['stripes',    'Stripes'],
  ['bubbles',    'Bubbles'],
  ['spotlight',  'Spotlight'],
  ['beachball',  'Beachball'],
  ['spectrum',   'Spectrum'],
  ['fireworks',  'Fireworks'],
  ['vibrant',    'Vibrant'],
  ['voltage',    'Voltage'],
  ['ink',        'Ink'],
  ['gas',        'Gas'],
  ['water',      'Water'],
  ['strobe',     'Strobe'],
  ['gold',       'Gold'],
  ['chase',      'Chase']
];

// The swatches of the theme colour picker (main menu > Colors), one [hex, name] each.
export const THEME_COLORS = [
  ['#e83535', 'Crimson'],
  ['#4b0606', 'Garnet'],
  ['#32cd32', 'Lime'],
  ['#0d3d23', 'Forest'],
  ['#455197', 'Azure'],
  ['#1f2051', 'Navy'],
  ['#A796D1', 'Plum'],
  ['#FF6A00', 'Orange'],
  ['#92C7D9', 'Powder'],
  ['#473299', 'Gem']
];

// The swatches of the colour picker for the LED pixels and the bed lights, one
// [hex, name] each.
export const DEVICE_COLORS = [
  ['#FF9329', 'Candle'],
  ['#FFC58F', 'Tungsten'],
  ['#FFF0DD', 'Halogen'],
  ['#FFF7FF', 'Sunlight'],
  ['#C9E2FF', 'Overcast'],
  ['#FF0000', 'Red'],
  ['#00FF00', 'Green'],
  ['#0000FF', 'Blue'],
  ['#4B00E6', 'Indigo'],
  ['#409CFF', 'Sky'],
  ['#FF1993', 'Pink'],
  ['#FFC0CB', 'Lavender'],
  ['#44B439', 'Forest'],
  ['#EE8262', 'Salmon'],
  ['#FFFF00', 'Yellow'],
  ['#F26D02', 'Orange'],
  ['#A700DC', 'Purple'],
  ['#FF06FF', 'Violet'],
  ['#FF83FA', 'Orchid'],
  ['#CD5B45', 'Coral'],
  ['#FF6347', 'Tomato'],
  ['#C0FF3E', 'Olive'],
  ['#76EEC6', 'Aquamarine'],
  ['#FF4500', 'Orangered'],
  ['#40E0D0', 'Turquoise']
];
