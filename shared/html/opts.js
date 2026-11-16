// Automate - site options: the names and switches a deploy may change, in one file that
// imports nothing, so any module can read them. www-minify.sh leaves it as written, so an
// installer can rewrite one of its lines with sed.
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
