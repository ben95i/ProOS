// Automate - browser-side encryption of a file kept on the server (lists.js uses it): key
// and blob helpers, the password record beside the file, and FileCrypt, holding one
// file's key while the window is open.

import { readFile, writeFile, fileRows, isNumeric,
  base64URLSafeEncode, base64URLSafeToStandard } from './utils.js';

// One PBKDF2 pass gives 64 bytes: 0..31 the AES-256-GCM key, never sent; 32..63 the
// verifier, hashed into the password record. A blob is E1.<iv>.<ciphertext+tag> in URL-safe
// base64. crypto.subtle needs a secure context, so cryptoReady() is false over plain http.

const CRYPT_TAG = 'E1';
const CRYPT_ITERATIONS = 310000; // PBKDF2 rounds for a new password
const CRYPT_SALT_BYTES = 16;
const CRYPT_IV_BYTES = 12;
const CRYPT_KEY_BYTES = 32;
const CRYPT_MIN_PASSWORD = 8;
const CRYPT_MAX_PASSWORD = 256;
const CRYPT_BLOB_REGEX = /^E1\.[A-Za-z0-9\-_@]+\.[A-Za-z0-9\-_@]+$/;

// is WebCrypto here? There is deliberately no fallback.
export function cryptoReady() {
  if (typeof crypto === 'undefined') {
    return false;
  }
  if (!crypto.subtle || typeof crypto.subtle.deriveBits !== 'function') {
    return false;
  }
  return true;
}

// count random bytes
function randomBytes(count) {
  const bytes = new Uint8Array(count);
  crypto.getRandomValues(bytes);
  return bytes;
}

// bytes -> URL-safe base64
function bytesToB64(bytes) {
  let binary = '';
  const view = new Uint8Array(bytes);
  // a chunk at a time: a long array overruns the call stack as arguments
  for (let i = 0; i < view.length; i += 4096) {
    binary += String.fromCharCode.apply(null, view.subarray(i, i + 4096));
  }
  return base64URLSafeEncode(binary);
}

// URL-safe base64 -> bytes
function b64ToBytes(value) {
  const binary = atob(base64URLSafeToStandard(String(value)));
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

// a new record slot and its key for a password:
// { slot: { salt, iterations, check }, key }
async function newCryptSlot(password) {
  const salt = bytesToB64(randomBytes(CRYPT_SALT_BYTES));
  const derived = await deriveCryptKeys(password, salt, CRYPT_ITERATIONS);
  const slot = {
    salt: salt,
    iterations: CRYPT_ITERATIONS,
    check: await hashVerifier(derived.verifier)
  };
  return { slot: slot, key: derived.key };
}

// "" when the password will do, or why not
function checkPassword(value) {
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

// The one slow step; the verifier the server sees tells it nothing about the key.
// Returns { key, verifier }.
async function deriveCryptKeys(password, salt, iterations) {
  const encoder = new TextEncoder();
  // NFC: the same password can arrive as different bytes from two devices
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
  // the raw bytes are wiped once the non-extractable key holds them
  keyBytes.fill(0);
  derived.fill(0);
  return { key: key, verifier: bytesToB64(verifier) };
}

// is this a well-formed blob?
export function isCryptBlob(value) {
  return typeof value === 'string' && CRYPT_BLOB_REGEX.test(value);
}

// A label passed to AES-GCM as additional data, so a blob moved to another list or file
// fails to open.
export function cryptContext(file, id) {
  return `automate:${file}:${id}`;
}

// AES-GCM parameters for a blob's iv and context
function cryptParams(iv, context) {
  const params = { name: 'AES-GCM', iv: iv, tagLength: 128 };
  if (context !== '') {
    params.additionalData = new TextEncoder().encode(context);
  }
  return params;
}

// text -> a blob
export async function encryptText(key, text, context = '') {
  const iv = randomBytes(CRYPT_IV_BYTES);
  const params = cryptParams(iv, context);
  const sealed = await crypto.subtle.encrypt(
    params, key, new TextEncoder().encode(String(text))
  );
  return `${CRYPT_TAG}.${bytesToB64(iv)}.${bytesToB64(new Uint8Array(sealed))}`;
}

// A blob -> its text, or null for anything that will not open (wrong key, tampered,
// another list's); GCM cannot tell those apart.
async function decryptText(key, blob, context = '') {
  if (!isCryptBlob(blob)) {
    return null;
  }
  const parts = String(blob).split('.');
  try {
    const params = cryptParams(b64ToBytes(parts[1]), context);
    const opened = await crypto.subtle.decrypt(params, key, b64ToBytes(parts[2]));
    return new TextDecoder().decode(opened);
  } catch (error) {
    return null;
  }
}

//// password record ////

// A file beside the data (lists-pwd.save), one row per slot: V1|SALT|ITERATIONS|CHECK, CHECK
// being SHA-256 of the verifier. Blank means no password set. A change writes a second row
// and promotes it once the re-encrypted file lands, so unlock() tries each.
const CRYPT_REC_TAG = 'V1';
const CRYPT_REC_COLS = 4;

// SHA-256 of the verifier, already the far end of a slow KDF
async function hashVerifier(verifier) {
  const digest = await crypto.subtle.digest('SHA-256', b64ToBytes(verifier));
  return bytesToB64(new Uint8Array(digest));
}

// Record text -> [{ salt, iterations, check }], oldest first, dropping what will not parse.
function parseCryptRecord(text) {
  if (typeof text !== 'string' || text === "") {
    return [];
  }
  const slots = [];
  fileRows(text).forEach(row => {
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

// slots -> record text
function buildCryptRecord(slots) {
  let text = "";
  slots.forEach(slot => {
    text += `${CRYPT_REC_TAG}|${slot.salt}|${slot.iterations}|${slot.check}\n`;
  });
  return text;
}

//// FileCrypt ////

// One file's password and key, kept on the instance only: never in storage or on the server.

export class FileCrypt {

  // options: file (the record file)
  constructor(options = {}) {
    this.file = options.file;
    this.key = null;    // CryptoKey while unlocked
    this.params = null; // { salt, iterations } the key came from
    this.slots = null;  // last record read, so a change can rebuild it
  }

  // is the key held?
  isUnlocked() {
    return this.key !== null;
  }

  // drops the key
  lock() {
    this.key = null;
    this.params = null;
  }

  // { ready, slots }, or null when the file could not be read. A blank file is "no
  // password yet".
  async status() {
    const text = await readFile(this.file);
    if (text === null) {
      return null;
    }
    const slots = parseCryptRecord(text);
    this.slots = slots;
    return { ready: slots.length > 0, slots: slots };
  }

  // writes the record, keeping it on success
  async writeRecord(slots) {
    const sent = await writeFile(this.file, buildCryptRecord(slots));
    if (sent === true) {
      this.slots = slots;
    }
    return sent;
  }

  // First password for this file. The record is re-read first, so a window stale on "not
  // set up" cannot walk over a password set meanwhile.
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
    const made = await newCryptSlot(password);
    if (await this.writeRecord([made.slot]) !== true) {
      return { ok: false, error: 'password not saved' };
    }
    this.key = made.key;
    this.params = { salt: made.slot.salt, iterations: made.slot.iterations };
    return { ok: true, error: "" };
  }

  // Derives against each set of parameters in the record, keeping the key of the one that
  // matches. Two rows only while a password change never finished; either password opens.
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

  // text -> a blob under the held key, or null when locked
  async encrypt(text, context = '') {
    if (!this.isUnlocked()) {
      return null;
    }
    return await encryptText(this.key, text, context);
  }

  // a blob -> text under the held key, or null
  async decrypt(blob, context = '') {
    if (!this.isUnlocked()) {
      return null;
    }
    return await decryptText(this.key, blob, context);
  }

  // Three steps, as the data and the new record cannot land together: park the new record
  // as a second row; rewrite(newKey) re-encrypts everything and answers true once that
  // landed; promote the new row to the only one. A failed rewrite restores the record, and a
  // browser dying between leaves both rows for unlock().
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
    const made = await newCryptSlot(newPassword);
    const next = made.slot;
    if (await this.writeRecord([current, next]) !== true) {
      return { ok: false, error: 'server connection not available' };
    }
    let written = false;
    try {
      written = (await rewrite(made.key)) === true;
    } catch (error) {
      written = false;
    }
    if (written !== true) {
      // the data is still under the old password
      await this.writeRecord([current]);
      return { ok: false, error: 'password not changed, nothing was saved' };
    }
    this.key = made.key;
    this.params = { salt: next.salt, iterations: next.iterations };
    if (await this.writeRecord([next]) !== true) {
      // the data is under the new password; unlock() copes with the old row until the next change
      return { ok: true, error: 'password changed, record not tidied' };
    }
    return { ok: true, error: "" };
  }
}
