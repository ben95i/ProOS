// Renders the whole page from one side and prints the resulting markup, so
// the two can be diffed as separate processes (each needs its own window).
import { ensureWifiStub, setupDom, ORIG, NEW } from './env.mjs';
import { pathToFileURL } from 'node:url';

const root = process.argv[2] === 'after' ? NEW : ORIG;
ensureWifiStub();
const win = setupDom();
Object.defineProperty(global, 'App', {
  get: () => win.App,
  set: (value) => { win.App = value; },
  configurable: true
});
await import(pathToFileURL(root + 'main.js').href);
win.document.dispatchEvent(new win.Event('DOMContentLoaded', { bubbles: true }));
// let the load settle, then take the markup
await new Promise(r => setTimeout(r, 60));
const body = win.document.body.innerHTML;
// the star canvas holds no markup, and the theme colour is applied to <html>
const themed = win.document.documentElement.getAttribute('style');
const light = win.document.documentElement.className;
console.log(JSON.stringify({ body, themed, light,
  room: win.App.currentRoom, model: win.App.sysModel }, null, 1));
process.exit(0);
