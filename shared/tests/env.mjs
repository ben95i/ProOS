// Shared jsdom environment for the before/after comparison tests.
// Sets up the globals the site's ES modules expect, then lets a test import
// the same module from orig/ and from new/ and compare what they produce.

import { JSDOM } from 'jsdom';
import { webcrypto } from 'node:crypto';
import { pathToFileURL } from 'node:url';
import { existsSync, writeFileSync } from 'node:fs';

export const ORIG = '/home/ben/orig/html/';
export const NEW = '/home/ben/new/html/';

let dom = null;

export function setupDom(bodyHTML = '') {
  dom = new JSDOM(`<!DOCTYPE html><html><body>${bodyHTML}</body></html>`, {
    url: 'https://automate.home/',
    pretendToBeVisual: true
  });
  const win = dom.window;
  global.window = win;
  global.document = win.document;
  global.location = win.location;
  Object.defineProperty(global, 'navigator',
    { value: win.navigator, configurable: true, writable: true });
  global.HTMLElement = win.HTMLElement;
  global.Element = win.Element;
  global.Node = win.Node;
  global.Event = win.Event;
  global.CustomEvent = win.CustomEvent;
  global.Blob = win.Blob;
  // jsdom's own btoa stops working once it is copied onto the node global,
  // so the two are supplied here with browser semantics (latin1 only)
  global.btoa = function btoa(input) {
    const text = String(input);
    for (let i = 0; i < text.length; i++) {
      if (text.charCodeAt(i) > 255) {
        const error = new Error('The string to be encoded contains invalid characters.');
        error.name = 'InvalidCharacterError';
        throw error;
      }
    }
    return Buffer.from(text, 'latin1').toString('base64');
  };
  global.atob = function atob(input) {
    return Buffer.from(String(input), 'base64').toString('latin1');
  };
  win.btoa = global.btoa;
  win.atob = global.atob;
  global.localStorage = win.localStorage;
  global.requestAnimationFrame = win.requestAnimationFrame ||
    ((cb) => setTimeout(() => cb(Date.now()), 0));
  global.cancelAnimationFrame = win.cancelAnimationFrame || clearTimeout;
  Object.defineProperty(global, 'crypto',
    { value: webcrypto, configurable: true, writable: true });
  Object.defineProperty(win, 'crypto',
    { value: webcrypto, configurable: true, writable: true });
  global.fetch = async () => ({ ok: true, json: async () => "", text: async () => "" });
  win.fetch = global.fetch;
  return win;
}

export function resetBody(bodyHTML = '') {
  document.body.innerHTML = bodyHTML;
}

// main.js imports wifi.js, which is not in the snapshot (it holds the guest
// password), so a stub stands in for it while the tests run.
export function ensureWifiStub() {
  const stub = 'export function SSIDpwd() {\n  return "";\n}\n';
  for (const root of [ORIG, NEW]) {
    if (!existsSync(root + 'wifi.js')) {
      writeFileSync(root + 'wifi.js', stub);
    }
  }
}

export async function loadPair(file) {
  const before = await import(pathToFileURL(ORIG + file).href);
  const after = await import(pathToFileURL(NEW + file).href);
  return { before, after };
}

//// tiny assertion helpers ////

let passed = 0;
let failed = 0;
const failures = [];

export function check(name, condition, detail = '') {
  if (condition) {
    passed++;
  } else {
    failed++;
    failures.push(`${name}${detail ? ' :: ' + detail : ''}`);
  }
}

export function same(name, a, b) {
  const left = JSON.stringify(a);
  const right = JSON.stringify(b);
  check(name, left === right, `\n    before: ${left}\n    after:  ${right}`);
}

// runs both sides and compares the result, an exception counting as a result
export function attempt(fn) {
  try {
    return { value: fn() };
  } catch (error) {
    return { error: `${error.constructor.name}: ${error.message}` };
  }
}

export function sameCall(name, callBefore, callAfter) {
  same(name, attempt(callBefore), attempt(callAfter));
}

export function report(title) {
  console.log(`\n${title}: ${passed} passed, ${failed} failed`);
  failures.slice(0, 40).forEach(line => console.log('  FAIL ' + line));
  return failed;
}

export function counts() {
  return { passed, failed };
}

//// sample data used across the tests ////

export const SAMPLE_STRINGS = [
  '', ' ', 'abc', 'Hello World', '0', '12', '-5', '3.5', '255',
  'a|b|c', 'x~y~z', 'line1\nline2', '"quoted"', 'Caf\u00e9 na\u00efve',
  '\u4f60\u597d', 'tab\there', 'trailing   spaces   ', '#FF00AA',
  'https://example.com/a?b=c&d=e', 'a-b-c-d', 'LOCK|1|tok|123|0',
  'has|pipe', 'has~tilde', 'a\r\nb', 'null', 'undefined', '!ENC'
];

export function randomHex(rng) {
  const value = Math.floor(rng() * 0x1000000);
  return '#' + value.toString(16).padStart(6, '0').toUpperCase();
}

// deterministic pseudo random, so a failure can be reproduced
export function makeRng(seed = 12345) {
  let state = seed >>> 0;
  return function next() {
    state = (state * 1664525 + 1013904223) >>> 0;
    return state / 0x100000000;
  };
}
