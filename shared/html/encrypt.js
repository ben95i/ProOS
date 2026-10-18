// Automate Website - encryption
//
// The browser-side encryption for files kept on the server: the key and
// blob helpers, the password record beside each encrypted file, and
// FileCrypt, which holds one file's password and key while the window is
// open. Sits just above utils.js; lists.js is what uses it.

import { readFile, writeFile, splitMenuData, cleanString, isNumeric,
  base64URLSafeEncode, base64URLSafeToStandard } from './utils.js';

//// shared encryption ////
//
// The browser side of encrypted files. Nothing here trusts the server with
// anything it could decrypt with. One PBKDF2 pass produces 64 bytes:
//   bytes  0..31  the AES-256-GCM data key -- stays in this tab, never sent
//   bytes 32..63  the verifier, hashed into the password record
//
// A blob on a row looks like E1.<iv>.<ciphertext+tag> in the site's URL-safe
// base64, so it survives the pipe-delimited files as it stands.
//
// crypto.subtle is only there in a secure context (https, or localhost), so
// cryptoReady() is false over plain http and the caller has to say so rather
// than quietly storing something weaker.

export const CRYPT_TAG = 'E1';          // blob format, first field
export const CRYPT_ITERATIONS = 310000; // PBKDF2 rounds for a new password
export const CRYPT_SALT_BYTES = 16;
export const CRYPT_IV_BYTES = 12;       // 96 bits, what AES-GCM expects
export const CRYPT_KEY_BYTES = 32;      // AES-256
export const CRYPT_MIN_PASSWORD = 8;
export const CRYPT_MAX_PASSWORD = 256;
const CRYPT_BLOB_REGEX = /^E1\.[A-Za-z0-9\-_@]+\.[A-Za-z0-9\-_@]+$/;

// WebCrypto, or nothing -- there is no fallback and there should not be one
export function cryptoReady() {
  if (typeof crypto === 'undefined') {
    return false;
  }
  if (!crypto.subtle || typeof crypto.subtle.deriveBits !== 'function') {
    return false;
  }
  return true;
}

export function randomBytes(count) {
  const bytes = new Uint8Array(count);
  crypto.getRandomValues(bytes);
  return bytes;
}

// bytes -> the site's URL-safe base64
export function bytesToB64(bytes) {
  let binary = '';
  const view = new Uint8Array(bytes);
  // a chunk at a time: fromCharCode is given the array as arguments, and a
  // long one overruns the call stack
  for (let i = 0; i < view.length; i += 4096) {
    binary += String.fromCharCode.apply(null, view.subarray(i, i + 4096));
  }
  return base64URLSafeEncode(binary);
}

export function b64ToBytes(value) {
  const binary = atob(base64URLSafeToStandard(String(value)));
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

export function newCryptSalt() {
  return bytesToB64(randomBytes(CRYPT_SALT_BYTES));
}

// "" when the password will do, or the reason it will not
export function checkPassword(value) {
  let text = '';
  if (typeof value === 'string') {
    text = value;
  }
  if (text.length < CRYPT_MIN_PASSWORD) {
    return `at least ${CRYPT_MIN_PASSWORD} characters`;
  }
  if (text.length > CRYPT_MAX_PASSWORD) {
    return `at most ${CRYPT_MAX_PASSWORD} characters`;
  }
  return "";
}

// The one slow step. Both halves come out of a single PBKDF2 pass, so the
// verifier the server sees tells it nothing about the key it never sees.
// Returns { key, verifier }.
export async function deriveCryptKeys(password, salt, iterations) {
  const encoder = new TextEncoder();
  // NFC first: the same password typed on two devices can arrive as two
  // different byte strings otherwise, and derive two different keys
  const secret = encoder.encode(String(password).normalize('NFC'));
  const material = await crypto.subtle.importKey(
    'raw', secret, { name: 'PBKDF2' }, false, ['deriveBits']
  );
  const bits = await crypto.subtle.deriveBits({
    name: 'PBKDF2',
    salt: b64ToBytes(salt),
    iterations: Number(iterations),
    hash: 'SHA-256'
  }, material, CRYPT_KEY_BYTES * 8 * 2);
  const derived = new Uint8Array(bits);
  const keyBytes = derived.slice(0, CRYPT_KEY_BYTES);
  const verifier = derived.slice(CRYPT_KEY_BYTES);
  const key = await crypto.subtle.importKey(
    'raw', keyBytes, { name: 'AES-GCM' }, false, ['encrypt', 'decrypt']
  );
  // the raw bytes are wiped once the key object holds them; the CryptoKey is
  // not extractable, so nothing on the page can read them back out
  keyBytes.fill(0);
  derived.fill(0);
  return { key: key, verifier: bytesToB64(verifier) };
}

export function isCryptBlob(value) {
  return typeof value === 'string' && CRYPT_BLOB_REGEX.test(value);
}

// A label tying ciphertext to where it is kept, passed to AES-GCM as
// additional data: a blob moved to another list, or another file, fails to
// open rather than opening somewhere it does not belong.
export function cryptContext(file, id) {
  return `automate:${file}:${id}`;
}

export async function encryptText(key, text, context = '') {
  const iv = randomBytes(CRYPT_IV_BYTES);
  const params = { name: 'AES-GCM', iv: iv, tagLength: 128 };
  if (context !== '') {
    params.additionalData = new TextEncoder().encode(context);
  }
  const sealed = await crypto.subtle.encrypt(
    params, key, new TextEncoder().encode(String(text))
  );
  return `${CRYPT_TAG}.${bytesToB64(iv)}.${bytesToB64(new Uint8Array(sealed))}`;
}

// null for anything that will not open: the wrong key, a blob that has been
// meddled with, one that belongs to another list. GCM cannot tell those apart
// and neither should the caller.
export async function decryptText(key, blob, context = '') {
  if (!isCryptBlob(blob)) {
    return null;
  }
  const parts = String(blob).split('.');
  try {
    const params = { name: 'AES-GCM', iv: b64ToBytes(parts[1]), tagLength: 128 };
    if (context !== '') {
      params.additionalData = new TextEncoder().encode(context);
    }
    const opened = await crypto.subtle.decrypt(params, key, b64ToBytes(parts[2]));
    return new TextDecoder().decode(opened);
  } catch (error) {
    return null;
  }
}

//// password record ////
//
// A file of its own beside the data it guards (lists-pwd.save), one row per
// slot:
//   V1|SALT|ITERATIONS|CHECK
// SALT and ITERATIONS are what PBKDF2 needs to arrive back at the same key;
// neither is a secret. CHECK is SHA-256 of the verifier, so a password can be
// told right from wrong without the file holding anything that opens the
// data. A missing or blank file means no password has been set up.
//
// A password change writes a second row while the first still describes what
// the data is encrypted under, and promotes it once the re-encrypted file has
// landed -- so a change cut off part way leaves both, and unlock() tries each.

export const CRYPT_REC_TAG = 'V1';
const CRYPT_REC_COLS = 4;

// SHA-256 of the verifier. The verifier is already the far end of a slow KDF,
// so a fast hash here gives nothing away.
export async function hashVerifier(verifier) {
  const digest = await crypto.subtle.digest('SHA-256', b64ToBytes(verifier));
  return bytesToB64(new Uint8Array(digest));
}

// record text -> [{ salt, iterations, check }], oldest first. Anything that
// will not parse is dropped, and no rows reads as "not set up".
export function parseCryptRecord(text) {
  if (typeof text !== 'string' || text === "") {
    return [];
  }
  const slots = [];
  splitMenuData(text)
    .map(row => cleanString(row))
    .filter(row => row)
    .forEach(row => {
      const cols = row.split('|');
      if (cols.length < CRYPT_REC_COLS || cols[0] !== CRYPT_REC_TAG) {
        return;
      }
      if (!isNumeric(cols[2])) {
        return;
      }
      const iterations = Number(cols[2]);
      if (iterations < 1000 || cols[1] === "" || cols[3] === "") {
        return;
      }
      slots.push({ salt: cols[1], iterations: iterations, check: cols[3] });
    });
  return slots;
}

export function buildCryptRecord(slots) {
  let text = "";
  slots.forEach(slot => {
    text += `${CRYPT_REC_TAG}|${slot.salt}|${slot.iterations}|${slot.check}\n`;
  });
  return text;
}

//// FileCrypt ////
//
// The password for one file, and the key derived from it, for as long as the
// window is open:
//   status()   what the record file says -- whether a password is set at all
//   setup()    the first password, refused once one is set
//   unlock()   derive, check against the record, keep the key in memory
//   lock()     drop the key
//   change()   old password -> new one, re-encrypting in between
//
// The key lives on the instance and nowhere else: not in storage, not in a
// cookie, never on the server.

export class FileCrypt {

  // options: file (the record file, e.g. 'lists-pwd.save')
  constructor(options = {}) {
    this.file = options.file;
    this.key = null;    // CryptoKey while unlocked
    this.params = null; // { salt, iterations } the key came from
    this.slots = null;  // last record read, so a change can rebuild it
  }

  //// state ////

  isUnlocked() {
    return this.key !== null;
  }

  lock() {
    this.key = null;
    this.params = null;
  }

  //// the record file ////

  // { ready, slots }, or null when the file could not be read at all. A blank
  // file is not a failure: it is what "no password yet" looks like.
  async status() {
    const text = await readFile(this.file);
    if (text === null) {
      return null;
    }
    const slots = parseCryptRecord(text);
    this.slots = slots;
    return { ready: slots.length > 0, slots: slots };
  }

  async writeRecord(slots) {
    const sent = await writeFile(this.file, buildCryptRecord(slots));
    if (sent === true) {
      this.slots = slots;
    }
    return sent;
  }

  //// setting up ////

  // First password for this file. The record is re-read immediately before
  // writing: a window sitting open on a stale "not set up" does not get to
  // walk over a password set in the meantime, which would leave whatever was
  // encrypted under it unreadable for good.
  async setup(password) {
    if (!cryptoReady()) {
      return { ok: false, error: 'encryption not available' };
    }
    const bad = checkPassword(password);
    if (bad !== "") {
      return { ok: false, error: bad };
    }
    const state = await this.status();
    if (state === null) {
      return { ok: false, error: 'server connection not available' };
    }
    if (state.ready === true) {
      return { ok: false, error: 'a password is already set' };
    }
    const salt = newCryptSalt();
    const iterations = CRYPT_ITERATIONS;
    const derived = await deriveCryptKeys(password, salt, iterations);
    const slot = {
      salt: salt,
      iterations: iterations,
      check: await hashVerifier(derived.verifier)
    };
    if (await this.writeRecord([slot]) !== true) {
      return { ok: false, error: 'password not saved' };
    }
    this.key = derived.key;
    this.params = { salt: salt, iterations: iterations };
    return { ok: true, error: "" };
  }

  //// unlocking ////

  // Derives against every set of parameters the record holds and keeps the
  // key belonging to the one that matches. There are two only while a
  // password change was started and never finished, and then either password
  // opens the file -- whichever one the data is actually encrypted under.
  async unlock(password) {
    if (!cryptoReady()) {
      return { ok: false, error: 'encryption not available' };
    }
    if (typeof password !== 'string' || password === "") {
      return { ok: false, error: 'enter the password' };
    }
    const state = await this.status();
    if (state === null) {
      return { ok: false, error: 'server connection not available' };
    }
    if (state.ready !== true) {
      return { ok: false, error: 'no password set' };
    }
    for (let i = 0; i < state.slots.length; i++) {
      const slot = state.slots[i];
      const derived = await deriveCryptKeys(password, slot.salt, slot.iterations);
      if (await hashVerifier(derived.verifier) === slot.check) {
        this.key = derived.key;
        this.params = { salt: slot.salt, iterations: slot.iterations };
        return { ok: true, error: "" };
      }
    }
    return { ok: false, error: 'wrong password' };
  }

  //// using it ////

  async encrypt(text, context = '') {
    if (!this.isUnlocked()) {
      return null;
    }
    return await encryptText(this.key, text, context);
  }

  async decrypt(blob, context = '') {
    if (!this.isUnlocked()) {
      return null;
    }
    return await decryptText(this.key, blob, context);
  }

  //// changing the password ////

  // Three steps, because the re-encrypted data and the new record cannot land
  // together:
  //   park     the new record is written as a second row, under the old one,
  //            which is still what the data answers to
  //   rewrite  the caller re-encrypts everything with the new key and gets it
  //            onto the server; it returns true only if that write landed
  //   promote  the new row becomes the only row
  // A rewrite that fails puts the record back as it was, and the old password
  // still opens everything. A browser that dies in between leaves both rows,
  // and unlock() finds its way in with either password.
  //
  // rewrite is handed the new key and must not touch this instance: the swap
  // happens here, once the write is known to have landed.
  async change(oldPassword, newPassword, rewrite) {
    if (!cryptoReady()) {
      return { ok: false, error: 'encryption not available' };
    }
    const bad = checkPassword(newPassword);
    if (bad !== "") {
      return { ok: false, error: bad };
    }
    if (String(oldPassword) === String(newPassword)) {
      return { ok: false, error: 'new password must differ' };
    }
    const opened = await this.unlock(oldPassword);
    if (opened.ok !== true) {
      return opened;
    }
    const current = { ...this.params, check: "" };
    const held = (this.slots || []).find(slot => slot.salt === this.params.salt);
    if (!held) {
      return { ok: false, error: 'password record changed, try again' };
    }
    current.check = held.check;
    const salt = newCryptSalt();
    const iterations = CRYPT_ITERATIONS;
    const derived = await deriveCryptKeys(newPassword, salt, iterations);
    const next = {
      salt: salt,
      iterations: iterations,
      check: await hashVerifier(derived.verifier)
    };
    if (await this.writeRecord([current, next]) !== true) {
      return { ok: false, error: 'server connection not available' };
    }
    let written = false;
    try {
      written = (await rewrite(derived.key)) === true;
    } catch (error) {
      written = false;
    }
    if (written !== true) {
      // the data is still under the old password, so the record goes back to
      // describing only that
      await this.writeRecord([current]);
      return { ok: false, error: 'password not changed, nothing was saved' };
    }
    this.key = derived.key;
    this.params = { salt: salt, iterations: iterations };
    if (await this.writeRecord([next]) !== true) {
      // the data is already under the new password and the old row is still
      // there, which unlock() copes with -- it is tidied on the next change
      return { ok: true, error: 'password changed, record not tidied' };
    }
    return { ok: true, error: "" };
  }
}
