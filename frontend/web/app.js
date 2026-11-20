/* app.js — friend-local.html: libfriend, compiled to WebAssembly, hosted
 * by a page.
 *
 * The library asks for work and takes the results back; this file does
 * the work. Requests to the provider go out with fetch(). The friend's
 * photo file is kept in memory and in IndexedDB, and served to the
 * library's file reads and writes from there. Saves go to IndexedDB too.
 * The screen is ordinary HTML, so typing is the device's own keyboard.
 *
 * One friend is in the module at a time, laid out for its size: opening
 * another saves this one, lays the block out again and loads that one.
 * Nothing the library hands back outlives the next tick, so everything
 * read out of it is copied straight away.
 *
 * The realtime voice mode is not here: a browser cannot open a websocket
 * with the key in a header, and the key goes nowhere else. Spoken turns,
 * which are ordinary requests, are.
 */
'use strict';

const VERSION = document.documentElement.dataset.version || 'dev';

// What friend.h numbers things as.
const R = { OK: 0, BAD_ARGUMENT: 1, TOO_SMALL: 2, FULL: 3, BAD_STATE: 4, BAD_DATA: 5, PROVIDER: 6, IO: 7 };
const IO = { HTTPS: 0, WS_OPEN: 1, WS_SEND: 2, WS_CLOSE: 3, FILE_READ: 4, FILE_WRITE: 5 };
const EV = {
  NONE: 0, COMMITTED: 1, DELTA: 2, DONE: 3, RETRACTED: 4, AUDIO: 5, TRANSCRIPT: 6,
  THINKING: 7, MEMORIES: 8, INTERIORITY: 9, DIRTY: 10, ERROR: 11,
};
const MSG = { USER: 0, FRIEND: 1, AMBIENT: 2, NOTICE: 3 };
const VOICE = { OFF: 0, LISTENING: 1, HEARING: 2, WORKING: 3, SPEAKING: 4 };
// friend_web.c's configuration fields.
const STR = {
  API_KEY: 0, BASE_URL: 1, FRIEND_NAME: 2, USER_NAME: 3, BLOB_PATH: 4, VOICE: 5, PROMPT: 6,
  MODEL: 7, // chat, then the others in Friend_Models order
};
const INT = { PROVIDER: 0, UTC_OFFSET: 1, IMAGES: 2, VOICE: 3, SPEAK_REPLIES: 4, VOICE_MODE: 5, BARGE_IN: 6, EMBODIED: 7 };
const MODELS = [
  ['chat', 'Replying'], ['extract', 'Extracting'], ['relate', 'Relating'],
  ['interiority', 'Interior monologue'], ['rerank', 'Reranking'], ['embed', 'Embedding'],
  ['vision', 'Looking at photographs'], ['transcribe', 'Hearing you'], ['speak', 'Speaking'],
];

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const encoder = new TextEncoder();
const decoder = new TextDecoder();

/* ------------------------------------------------------------------ *
 *  The module
 * ------------------------------------------------------------------ */

let W = null;        // its exports
let memory = null;   // its memory, whose buffer changes when it grows
let heapBase = 0;    // where the friend's block starts
let inputAt = 0, inputSize = 0;

function bytesView() { return new Uint8Array(memory.buffer); }
function table(ptr) { return new Uint32Array(memory.buffer, ptr, 32); }
function doubles() { return new Float64Array(memory.buffer, W.web_f64(), 8); }
function text(ptr, len) { return len ? decoder.decode(bytesView().subarray(ptr, ptr + len)) : ''; }
function copyOut(ptr, len) { return bytesView().slice(ptr, ptr + len); }
function textAt(fn, ...args) { const t = table(fn(...args)); return text(t[0], t[1]); }
function lastError() { return textAt(W.web_last_error) || 'something went wrong'; }

function ensureMemory(end) {
  const have = memory.buffer.byteLength;
  if (end <= have) return;
  try {
    memory.grow(Math.ceil((end - have) / 65536));
  } catch (e) {
    throw new Error('this device would not give the page the memory the friend needs');
  }
}

// Bytes into the module's input buffer, for one call.
function putInput(bytes, offset = 0) {
  if (offset + bytes.length > inputSize) throw new Error('that is too much to hand over at once');
  bytesView().set(bytes, inputAt + offset);
  return inputAt + offset;
}
function putText(s, offset = 0) {
  const b = encoder.encode(s);
  return [putInput(b, offset), b.length];
}

async function loadModule() {
  const b64 = $('friend-wasm').textContent.trim();
  const bin = atob(b64);
  const wasm = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) wasm[i] = bin.charCodeAt(i);
  const imports = friendHostImports(() => memory, (line) => console.log('[friend] ' + line.trimEnd()));
  const { instance } = await WebAssembly.instantiate(wasm, imports);
  W = instance.exports;
  memory = W.memory;
  heapBase = W.web_heap_base();
  inputAt = W.web_input();
  inputSize = W.web_input_size();
}

/* ------------------------------------------------------------------ *
 *  Storage: IndexedDB, or memory when the browser will not give us that
 * ------------------------------------------------------------------ */

const Store = {
  db: null,
  mem: null,
  persisted: false,

  async open() {
    try {
      this.db = await new Promise((resolve, reject) => {
        const req = indexedDB.open('friend-local', 1);
        req.onupgradeneeded = () => {
          const db = req.result;
          db.createObjectStore('friends', { keyPath: 'id' });
          db.createObjectStore('saves');
          db.createObjectStore('chunks');
          db.createObjectStore('settings');
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
        req.onblocked = () => reject(new Error('blocked'));
      });
      // A browser that opens it and then refuses writes is no better.
      await this.tx(['settings'], 'readwrite', (t) => t.objectStore('settings').put(1, 'probe'));
    } catch (e) {
      this.db = null;
      this.mem = { friends: new Map(), saves: new Map(), chunks: new Map(), settings: new Map() };
    }
    if (this.db && navigator.storage && navigator.storage.persisted) {
      try { this.persisted = await navigator.storage.persisted(); } catch (e) { /* not offered */ }
    }
  },

  get lasting() { return !!this.db; },

  async askToLast() {
    if (!this.db || this.persisted || !navigator.storage || !navigator.storage.persist) return;
    try { this.persisted = await navigator.storage.persist(); } catch (e) { /* refused */ }
  },

  tx(stores, mode, fn) {
    return new Promise((resolve, reject) => {
      const t = this.db.transaction(stores, mode);
      let result;
      t.oncomplete = () => resolve(result);
      t.onerror = () => reject(t.error);
      t.onabort = () => reject(t.error || new Error('the browser would not keep it'));
      const r = fn(t);
      if (r && 'onsuccess' in r) r.onsuccess = () => { result = r.result; };
    });
  },

  async friends() {
    if (!this.db) return [...this.mem.friends.values()];
    return (await this.tx(['friends'], 'readonly', (t) => t.objectStore('friends').getAll())) || [];
  },

  async save(meta, bytes) {
    if (!this.db) {
      this.mem.friends.set(meta.id, meta);
      this.mem.saves.set(meta.id, bytes);
      return;
    }
    await this.tx(['friends', 'saves'], 'readwrite', (t) => {
      t.objectStore('friends').put(meta);
      t.objectStore('saves').put(bytes, meta.id);
    });
  },

  async putMeta(meta) {
    if (!this.db) { this.mem.friends.set(meta.id, meta); return; }
    await this.tx(['friends'], 'readwrite', (t) => t.objectStore('friends').put(meta));
  },

  async load(id) {
    if (!this.db) return this.mem.saves.get(id) || null;
    return (await this.tx(['saves'], 'readonly', (t) => t.objectStore('saves').get(id))) || null;
  },

  // The photo file is kept as the writes that made it, by offset, and
  // put back together when the friend is opened.
  async appendChunk(path, offset, bytes) {
    if (!this.db) {
      if (!this.mem.chunks.has(path)) this.mem.chunks.set(path, new Map());
      this.mem.chunks.get(path).set(offset, bytes);
      return;
    }
    await this.tx(['chunks'], 'readwrite', (t) => t.objectStore('chunks').put(bytes, [path, offset]));
  },

  async file(path) {
    let entries;
    if (!this.db) {
      const m = this.mem.chunks.get(path);
      entries = m ? [...m.entries()].sort((a, b) => a[0] - b[0]) : [];
    } else {
      const range = IDBKeyRange.bound([path, 0], [path, Infinity]);
      const keys = [], values = [];
      await this.tx(['chunks'], 'readonly', (t) => {
        const s = t.objectStore('chunks');
        s.getAllKeys(range).onsuccess = (e) => keys.push(...e.target.result);
        s.getAll(range).onsuccess = (e) => values.push(...e.target.result);
      });
      entries = keys.map((k, i) => [k[1], values[i]]);
    }
    let size = 0;
    for (const [off, b] of entries) size = Math.max(size, off + b.length);
    const out = new Uint8Array(size);
    for (const [off, b] of entries) out.set(b, off);
    return out;
  },

  async putFile(path, bytes) {
    if (!bytes.length) return;
    await this.appendChunk(path, 0, bytes);
  },

  async remove(id) {
    const path = blobPathFor(id);
    if (!this.db) {
      this.mem.friends.delete(id);
      this.mem.saves.delete(id);
      this.mem.chunks.delete(path);
      return;
    }
    await this.tx(['friends', 'saves', 'chunks'], 'readwrite', (t) => {
      t.objectStore('friends').delete(id);
      t.objectStore('saves').delete(id);
      t.objectStore('chunks').delete(IDBKeyRange.bound([path, 0], [path, Infinity]));
    });
  },

  async setting(key) {
    if (!this.db) return this.mem.settings.get(key);
    return this.tx(['settings'], 'readonly', (t) => t.objectStore('settings').get(key));
  },

  async setSetting(key, value) {
    if (!this.db) { this.mem.settings.set(key, value); return; }
    await this.tx(['settings'], 'readwrite', (t) => t.objectStore('settings').put(value, key));
  },
};

function blobPathFor(id) { return id + '.blob'; }

// A file the library reads and writes, in memory.
class MemFile {
  constructor(bytes) { this.buf = bytes || new Uint8Array(0); this.len = this.buf.length; }
  write(offset, data) {
    const end = offset + data.length;
    if (end > this.buf.length) {
      const grown = new Uint8Array(Math.max(end, this.buf.length * 2, 65536));
      grown.set(this.buf.subarray(0, this.len));
      this.buf = grown;
    }
    this.buf.set(data, offset);
    this.len = Math.max(this.len, end);
  }
  bytes() { return this.buf.subarray(0, this.len); }
}

/* ------------------------------------------------------------------ *
 *  Settings
 * ------------------------------------------------------------------ */

const Settings = {
  v: {
    provider: 0,
    keys: ['', ''],
    keepKey: true,
    bases: ['', ''],
    models: [{}, {}],
    userName: '',
    images: true,
    voice: true,
    speakReplies: false,
    voiceName: '',
  },

  async load() {
    const saved = await Store.setting('settings');
    if (saved) Object.assign(this.v, saved);
    if (!this.v.keepKey) {
      for (const p of [0, 1]) {
        try { this.v.keys[p] = sessionStorage.getItem('friend-local-key-' + p) || ''; } catch (e) { /* none */ }
      }
    }
  },

  async store() {
    const kept = JSON.parse(JSON.stringify(this.v));
    if (!this.v.keepKey) {
      kept.keys = ['', ''];
      for (const p of [0, 1]) {
        try { sessionStorage.setItem('friend-local-key-' + p, this.v.keys[p]); } catch (e) { /* none */ }
      }
    } else {
      try { for (const p of [0, 1]) sessionStorage.removeItem('friend-local-key-' + p); } catch (e) { /* none */ }
    }
    await Store.setSetting('settings', kept);
  },

  key() { return (this.v.keys[this.v.provider] || '').trim(); },
};

function setConfigString(field, value) {
  const [p, n] = putText(value || '');
  W.web_config_str(field, p, n);
}

// The settings into the library's configuration, and the friend's own
// photo file with them.
function applyConfig() {
  const v = Settings.v;
  W.web_config_reset();
  W.web_config_int(INT.PROVIDER, v.provider);
  W.web_config_int(INT.UTC_OFFSET, -new Date().getTimezoneOffset());
  W.web_config_int(INT.IMAGES, v.images ? 1 : 0);
  W.web_config_int(INT.VOICE, v.voice && Mic.possible ? 1 : 0);
  W.web_config_int(INT.SPEAK_REPLIES, v.speakReplies ? 1 : 0);
  W.web_config_int(INT.VOICE_MODE, 0);
  setConfigString(STR.API_KEY, Settings.key());
  setConfigString(STR.BASE_URL, (v.bases[v.provider] || '').trim());
  setConfigString(STR.FRIEND_NAME, 'friend');
  setConfigString(STR.USER_NAME, v.userName.trim());
  setConfigString(STR.BLOB_PATH, S.blobPath);
  setConfigString(STR.VOICE, v.voiceName.trim());
  MODELS.forEach(([name], i) => setConfigString(STR.MODEL + i, (v.models[v.provider][name] || '').trim()));
}

function defaultModel(provider, i) { return textAt(W.web_default_model, provider, i); }

/* ------------------------------------------------------------------ *
 *  The friend in hand
 * ------------------------------------------------------------------ */

const S = {
  gen: 0,          // bumped whenever the block is laid out again
  blockSize: 0,
  entry: null,     // the list's record of the friend in hand, or null
  blobPath: '',
  files: new Map(),
  saving: null,
  unwritten: false,
  retryAt: 0,
  awaiting: false,
  broken: false,
};

function scratchAt() { return (heapBase + S.blockSize + 15) & ~15; }

// Lay the block out for a friend of this size. Everything that was in it
// is gone, so this comes after the friend in hand has been saved.
function layOut(memories, events, saveBytes) {
  const need = W.web_limits(memories, events, saveBytes);
  ensureMemory(heapBase + need + 65536);
  Net.dropAll();
  S.gen += 1;
  applyConfig();
  const r = W.web_init(heapBase, need);
  if (r !== R.OK) throw new Error('the friend could not be set up here (' + r + ')');
  S.blockSize = need;
  W.web_tick(Date.now());
}

function peek(bytes) {
  const front = bytes.length > inputSize ? bytes.subarray(0, inputSize) : bytes;
  const p = putInput(front);
  const t = table(W.web_peek(p, front.length));
  if (t[0] !== R.OK) return null;
  const d = doubles();
  return { name: text(t[1], t[2]), memories: t[3], created: d[0], events: d[1] };
}

function stats() {
  const t = table(W.web_stats());
  const d = doubles();
  return {
    memories: t[0], connections: t[1], active: t[2], inflight: t[3], scrollback: t[4],
    photos: t[5], photosWaiting: t[6], unsaved: t[7], settled: t[8], lost: d[2],
  };
}

function friendName() { return textAt(W.web_name); }

// friend_save, into the room past the block. Returns the bytes and what
// is at the front of them.
function saveBytes() {
  const size = W.web_save_size();
  const at = scratchAt();
  ensureMemory(at + size);
  const n = W.web_save(at, size);
  if (n < 0) throw new Error(lastError());
  const bytes = copyOut(at, n);
  return { bytes, info: peek(bytes) };
}

function loadBytes(bytes, info) {
  let memories = info.memories;
  for (let attempt = 0; ; attempt++) {
    layOut(memories, info.events, bytes.length);
    const at = scratchAt();
    ensureMemory(at + bytes.length);
    bytesView().set(bytes, at);
    const r = W.web_load(at, bytes.length);
    if (r === R.OK) return;
    if (r === R.FULL && attempt < 2) { memories = memories * 4 + 4096; continue; }
    throw new Error(lastError());
  }
}

function metaFrom(id, info, extra) {
  return Object.assign({
    id, name: info.name || 'friend', created: info.created, memories: info.memories,
    events: info.events, modified: Date.now(),
  }, extra || {});
}

// Writes the friend in hand if it has changed. Quietly, unless forced:
// only when nothing is half done and every photo is in its file.
function saveNow(force) {
  if (S.saving) return S.saving.then(() => (force ? saveNow(force) : true));
  if (!S.entry || S.broken) return Promise.resolve(true);
  const st = stats();
  if (!st.unsaved && !S.unwritten) return Promise.resolve(true);
  if (!force && (!st.settled || st.photosWaiting > 0 || Date.now() < S.retryAt)) return Promise.resolve(true);
  let saved;
  try {
    saved = saveBytes();
  } catch (e) {
    say('could not save ' + S.entry.name + ': ' + e.message, true);
    return Promise.resolve(false);
  }
  const entry = S.entry;
  const meta = metaFrom(entry.id, saved.info);
  S.unwritten = true;
  S.saving = Store.save(meta, saved.bytes).then(() => {
    // A failed write leaves unwritten set, so the next quiet moment
    // tries again even though the friend thinks it is saved.
    if (S.entry && S.entry.id === entry.id) { S.entry = meta; S.unwritten = false; }
    Store.askToLast();
    UI.updateRow(meta);
    if (S.entry === meta) growIfNeeded(saved);
    return true;
  }, (e) => {
    S.retryAt = Date.now() + 10000;
    say('could not keep ' + entry.name + ' in this browser: ' + (e && e.message ? e.message : e) +
        '. Download it to keep a copy.', true);
    return false;
  }).finally(() => { S.saving = null; });
  return S.saving;
}

// A friend that has grown towards the room it was laid out with is laid
// out again, bigger, from the save just taken.
function growIfNeeded(saved) {
  const st = stats();
  if (st.memories < W.web_max_memories() * 0.8 || st.inflight > 0 || !st.settled) return;
  try {
    loadBytes(saved.bytes, saved.info);
    UI.friendChanged();
  } catch (e) {
    say('the friend could not be given more room: ' + e.message, true);
  }
}

// Before the friend in hand is replaced: its photos into their file, then
// it into storage. Returns false, leaving it in hand, if that fails.
async function putAway() {
  if (!S.entry) return true;
  for (let i = 0; i < 100 && stats().photosWaiting > 0; i++) { pump(); await sleep(30); }
  Voice.stop();
  return saveNow(true);
}

function newId() {
  return 'f' + Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

async function openFriend(id, show = true) {
  if (S.entry && S.entry.id === id) { if (show) UI.showChat(); return; }
  if (!(await putAway())) return;
  const list = await Store.friends();
  const meta = list.find((m) => m.id === id);
  const bytes = meta && (await Store.load(id));
  if (!bytes) { say('that friend is not in this browser any more', true); return; }
  const info = peek(bytes);
  if (!info) { say(meta.name + ' was saved by a newer friend-local; this one cannot open it', true); return; }
  const photos = await Store.file(blobPathFor(id));
  adopt(meta, bytes, info, photos, show);
}

// Into the module: a save, and its photo file beside it.
function adopt(meta, bytes, info, photos, show = true) {
  S.entry = null;
  S.blobPath = blobPathFor(meta.id);
  S.files = new Map([[S.blobPath, new MemFile(photos)]]);
  try {
    loadBytes(bytes, info);
  } catch (e) {
    S.blobPath = '';
    S.files = new Map();
    layOut(0, 0, 0);
    UI.friendChanged();
    say(meta.name + ' could not be opened: ' + e.message, true);
    return false;
  }
  S.entry = meta;
  S.unwritten = false;
  S.awaiting = false;
  UI.friendChanged();
  if (show) UI.showChat();
  schedulePump();
  return true;
}

async function createFriend(name) {
  if (!(await putAway())) return;
  const id = newId();
  S.entry = null;
  S.blobPath = blobPathFor(id);
  S.files = new Map([[S.blobPath, new MemFile()]]);
  layOut(0, 0, 0);
  const [p, n] = putText(name);
  if (W.web_begin(p, n) !== R.OK) { say(lastError(), true); return; }
  const saved = saveBytes();
  const meta = metaFrom(id, saved.info);
  await Store.save(meta, saved.bytes);
  S.entry = meta;
  S.unwritten = false;
  await UI.refreshList();
  UI.friendChanged();
  UI.showChat();
  $('text').focus();
}

async function deleteFriend(id) {
  const list = await Store.friends();
  const meta = list.find((m) => m.id === id);
  if (!meta) return;
  const yes = await ask({
    title: 'Delete ' + meta.name + '?',
    text: 'Their memories, your conversation and their photos are removed from this browser, ' +
      'and cannot be brought back. Download them first to keep a copy.',
    ok: 'Delete', danger: true,
  });
  if (!yes) return;
  if (S.entry && S.entry.id === id) {
    Voice.stop();
    S.entry = null;
    S.blobPath = '';
    S.files = new Map();
    layOut(0, 0, 0);
  }
  await Store.remove(id);
  const left = (await Store.friends()).sort((a, b) => b.modified - a.modified);
  await UI.refreshList();
  if (!S.entry && left.length) await openFriend(left[0].id);
  else UI.friendChanged();
  if (!S.entry) UI.showList();
}

/* ------------------------------------------------------------------ *
 *  Bringing a friend in
 * ------------------------------------------------------------------ */

function startsWith(bytes, s) {
  if (bytes.length < s.length) return false;
  for (let i = 0; i < s.length; i++) if (bytes[i] !== s.charCodeAt(i)) return false;
  return true;
}

// friend_<name>.zip holds one deflated friend_state.json; the library's
// own unzip turns it back into the JSON, a chunk at a time.
function unzip(zip) {
  if (W.web_unzip_begin() !== R.OK) throw new Error('the zip could not be opened');
  const outCap = 1 << 20;
  const chunks = [];
  let total = 0, pos = 0;
  const inCap = Math.min(inputSize, 4 << 20);
  for (let guard = 0; ; guard++) {
    const piece = zip.subarray(pos, pos + inCap);
    const inPtr = putInput(piece);
    const outPtr = scratchAt();
    ensureMemory(outPtr + outCap);
    const t = table(W.web_unzip_push(inPtr, piece.length, pos + piece.length >= zip.length ? 1 : 0, outPtr, outCap));
    const [result, used, written, done] = [t[0], t[1], t[2], t[3]];
    if (result !== R.OK) throw new Error('the zip is damaged: ' + textAt(W.web_unzip_error));
    if (written) { chunks.push(copyOut(outPtr, written)); total += written; }
    pos += used;
    if (done) break;
    if (!used && !written && guard > 4) throw new Error('the zip ends early');
  }
  const json = new Uint8Array(total);
  let at = 0;
  for (const c of chunks) { json.set(c, at); at += c.length; }
  return json;
}

function plural(n, one, many) { return n.toLocaleString() + ' ' + (n === 1 ? one : many || one + 's'); }

async function importJson(json) {
  if (!(await putAway())) return;
  const id = newId();
  S.entry = null;
  S.blobPath = blobPathFor(id);
  S.files = new Map([[S.blobPath, new MemFile()]]);
  // The JSON says how much it holds only by being read, so the first
  // layout is a guess from its size (an embedding is several kilobytes
  // of text) and a second is made from what the first found.
  let memories = Math.ceil(json.length / 6000), events = Math.ceil(json.length / 300);
  let rep;
  for (let attempt = 0; ; attempt++) {
    layOut(memories, events, json.length / 2);
    const at = scratchAt();
    ensureMemory(at + json.length);
    bytesView().set(json, at);
    const t = table(W.web_import(at, json.length));
    rep = {
      result: t[0], memories: t[1], connections: t[2], messages: t[3], images: t[4],
      imagesWithoutPixels: t[5], memoriesDropped: t[6], connectionsDropped: t[7],
      messagesDropped: t[8], truncated: t[9],
    };
    if (rep.result !== R.OK) {
      const why = lastError();
      layOut(0, 0, 0);
      S.blobPath = '';
      S.files = new Map();
      UI.friendChanged();
      throw new Error(why);
    }
    const dropped = rep.memoriesDropped || rep.connectionsDropped || rep.messagesDropped;
    if (!dropped || attempt >= 2) break;
    memories = rep.memories + rep.memoriesDropped + Math.ceil(rep.connectionsDropped / 4) + 1024;
    events = rep.messages + rep.messagesDropped + 1024;
  }
  const saved = saveBytes();
  const meta = metaFrom(id, saved.info);
  S.entry = meta;
  await Store.save(meta, saved.bytes);
  S.unwritten = stats().unsaved ? true : false;
  // The photographs go to their file through the pump.
  for (let i = 0; i < 400 && stats().photosWaiting > 0; i++) { pump(); await sleep(15); }
  await saveNow(true);
  await UI.refreshList();
  UI.friendChanged();
  UI.showChat();
  let line = 'Brought ' + meta.name + ' across: ' + plural(rep.memories, 'memory', 'memories') + ', ' +
    plural(rep.messages, 'message') + (rep.images ? ', ' + plural(rep.images, 'photo') : '') + '.';
  if (rep.imagesWithoutPixels) line += ' ' + plural(rep.imagesWithoutPixels, 'photo') + ' came across as a description only.';
  if (rep.memoriesDropped || rep.messagesDropped) {
    line += ' ' + plural(rep.memoriesDropped, 'memory', 'memories') + ' and ' + plural(rep.messagesDropped, 'message') +
      ' did not fit.';
  }
  say(line, false);
}

async function importExport(file) {
  const e = readFriendLocal(file, peek);
  if (!(await putAway())) return;
  const id = newId();
  const meta = metaFrom(id, e.info);
  if (!adopt(meta, e.save, e.info, e.photos)) return;
  await Store.putFile(blobPathFor(id), e.photos || new Uint8Array(0));
  await Store.save(meta, e.save);
  await UI.refreshList();
  say('Brought ' + meta.name + ' across: ' + plural(e.info.memories, 'memory', 'memories') + ', ' +
    plural(e.info.events, 'message') + '.', false);
}

async function importFile(file) {
  say('Reading ' + file.name + '…', false);
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    if (startsWith(bytes, FRIEND_LOCAL.MAGIC)) {
      await importExport(bytes);
    } else if (startsWith(bytes, 'PK\x03\x04')) {
      await importJson(unzip(bytes));
    } else if (/^\s*\{/.test(decoder.decode(bytes.subarray(0, 64)))) {
      await importJson(bytes);
    } else {
      const info = peek(bytes);
      if (!info) {
        throw new Error('that is not a friend file. friend.com gives you friend_<name>.zip; ' +
          'friend-local writes <name>.friendlocal');
      }
      if (!(await putAway())) return;
      const meta = metaFrom(newId(), info);
      if (!adopt(meta, bytes, info, null)) return;
      await Store.save(meta, bytes);
      await UI.refreshList();
      say('Opened ' + meta.name + '. A .friend file has no photographs in it; a .friendlocal does.', false);
    }
  } catch (e) {
    say('Could not import ' + file.name + ': ' + e.message, true);
    // The friend in hand was put away to make room; bring it back.
    if (!S.entry) {
      const list = (await Store.friends()).sort((a, b) => b.modified - a.modified);
      if (list.length) await openFriend(list[0].id);
      const message = $('trouble-text').textContent;
      if (message.indexOf('Could not import') !== 0) say('Could not import ' + file.name + ': ' + e.message, true);
    }
  }
}

function slug(name) {
  const s = name.toLowerCase().normalize('NFKD').replace(/[^\p{L}\p{N}]+/gu, '-').replace(/^-+|-+$/g, '');
  return s || 'friend';
}

async function exportFriend(id) {
  let save, photos, name;
  if (S.entry && S.entry.id === id) {
    for (let i = 0; i < 100 && stats().photosWaiting > 0; i++) { pump(); await sleep(30); }
    if (!(await saveNow(true))) { say('the friend could not be saved, so it was not exported', true); return; }
    save = await Store.load(id);
    const f = S.files.get(S.blobPath);
    photos = f ? f.bytes() : null;
    name = S.entry.name;
  } else {
    const meta = (await Store.friends()).find((m) => m.id === id);
    if (!meta) return;
    save = await Store.load(id);
    photos = await Store.file(blobPathFor(id));
    name = meta.name;
  }
  if (!save) { say('there is nothing saved to export', true); return; }
  const out = writeFriendLocal(save, photos, Date.now());
  const fileName = slug(name) + '.friendlocal';
  const blob = new Blob([out], { type: 'application/octet-stream' });
  const coarse = matchMedia('(pointer: coarse)').matches;
  if (coarse && navigator.canShare) {
    try {
      const f = new File([blob], fileName, { type: 'application/octet-stream' });
      if (navigator.canShare({ files: [f] })) {
        await navigator.share({ files: [f], title: name });
        say('Exported ' + name + '. Import ' + fileName + ' on the other device.', false);
        return;
      }
    } catch (e) {
      if (e && e.name === 'AbortError') return;
      /* fall back to a download */
    }
  }
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = fileName;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60000);
  say('Exported ' + name + ' to ' + fileName + '. Import it on the other device.', false);
}

/* ------------------------------------------------------------------ *
 *  The transport: fetch for requests, memory and IndexedDB for files
 * ------------------------------------------------------------------ */

const Net = {
  live: new Map(), // id -> { controller, gen }

  dropAll() {
    for (const r of this.live.values()) r.controller.abort();
    this.live.clear();
  },

  abandon(id) {
    const r = this.live.get(id);
    if (r) { r.controller.abort(); this.live.delete(id); }
  },

  alive(id, gen) { return gen === S.gen && this.live.has(id); },

  finish(id) { this.live.delete(id); schedulePump(); },

  https(req) {
    const controller = new AbortController();
    const gen = S.gen;
    const id = req.id;
    this.live.set(id, { controller, gen });
    const headers = new Headers();
    for (const line of req.headers.split('\r\n')) {
      const colon = line.indexOf(':');
      if (colon > 0) headers.append(line.slice(0, colon).trim(), line.slice(colon + 1).trim());
    }
    let host = req.url;
    try { host = new URL(req.url).host; } catch (e) { /* as it is */ }
    const fail = (why) => {
      if (!this.alive(id, gen)) return;
      const [p, n] = putText(why);
      W.web_io_failed(id, p, n);
      this.finish(id);
    };
    fetch(req.url, {
      method: req.method || 'POST', headers, body: req.body.length ? req.body : undefined,
      signal: controller.signal, cache: 'no-store', credentials: 'omit', referrerPolicy: 'no-referrer',
    }).then(async (res) => {
      if (!this.alive(id, gen)) return;
      const reader = res.body.getReader();
      let got = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (!this.alive(id, gen)) { controller.abort(); return; }
        if (done) break;
        if (got + value.length > req.recvCapacity) {
          controller.abort();
          fail('the answer from ' + host + ' is bigger than the ' + Math.round(req.recvCapacity / 1048576) +
            ' MB there is room for');
          return;
        }
        bytesView().set(value, req.recv + got);
        got += value.length;
        W.web_io_progress(id, value.length);
        schedulePump();
      }
      W.web_io_complete(id, res.status);
      this.finish(id);
    }).catch((e) => {
      if (!this.alive(id, gen)) return;
      fail(navigator.onLine === false
        ? 'this device is offline'
        : 'could not reach ' + host + ' (' + (e && e.message ? e.message : 'no answer') + ')');
    });
  },

  read(req) {
    const f = S.files.get(req.path);
    const want = Math.min(req.length, req.recvCapacity);
    const fail = (why) => { const [p, n] = putText(why); W.web_io_failed(req.id, p, n); };
    if (!f) { fail('that file cannot be read: it is not there'); return; }
    if (req.offset + want > f.len) { fail('that file ends before the thing being read out of it'); return; }
    bytesView().set(f.buf.subarray(req.offset, req.offset + want), req.recv);
    if (want) W.web_io_progress(req.id, want);
    W.web_io_complete(req.id, 0);
  },

  write(req) {
    if (!S.files.has(req.path)) S.files.set(req.path, new MemFile());
    const f = S.files.get(req.path);
    f.write(req.offset, req.body);
    const gen = S.gen, id = req.id;
    this.live.set(id, { controller: new AbortController(), gen });
    Store.appendChunk(req.path, req.offset, req.body).then(() => {
      if (!this.alive(id, gen)) return;
      W.web_io_complete(id, 0);
      this.finish(id);
    }, (e) => {
      if (!this.alive(id, gen)) return;
      const [p, n] = putText('that file cannot be written to: ' + (e && e.message ? e.message : e));
      W.web_io_failed(id, p, n);
      this.finish(id);
    });
  },

  submit(req) {
    switch (req.kind) {
    case IO.HTTPS: this.https(req); break;
    case IO.FILE_READ: this.read(req); break;
    case IO.FILE_WRITE: this.write(req); break;
    default: {
      const [p, n] = putText('realtime talk needs the desktop app: a web page cannot open it without putting your key in the address');
      W.web_io_failed(req.id, p, n);
    }
    }
  },
};

function readRequest(i) {
  const t = table(W.web_request(i));
  const d = doubles();
  return {
    kind: t[0], flags: t[1], method: text(t[2], t[3]), url: text(t[4], t[5]), headers: text(t[6], t[7]),
    body: copyOut(t[8], t[9]), path: text(t[10], t[11]), length: t[12], recv: t[13], recvCapacity: t[14],
    id: d[0], offset: d[1], channel: d[2],
  };
}

/* ------------------------------------------------------------------ *
 *  Sound: the microphone in, the friend's voice out
 * ------------------------------------------------------------------ */

const Sound = {
  ctx: null,
  playAt: 0,
  sources: new Set(),

  // Made, or woken, inside a tap: browsers keep sound off until then.
  wake() {
    if (!this.ctx) {
      const C = window.AudioContext || window.webkitAudioContext;
      if (!C) return null;
      this.ctx = new C();
    }
    if (this.ctx.state === 'suspended') this.ctx.resume();
    return this.ctx;
  },

  play(samples, rate) {
    const ctx = this.wake();
    if (!ctx || !samples.length) return;
    const buffer = ctx.createBuffer(1, samples.length, rate);
    const ch = buffer.getChannelData(0);
    for (let i = 0; i < samples.length; i++) ch[i] = samples[i] / 32768;
    const src = ctx.createBufferSource();
    src.buffer = buffer;
    src.connect(ctx.destination);
    const at = Math.max(ctx.currentTime + 0.04, this.playAt);
    src.start(at);
    this.playAt = at + buffer.duration;
    this.sources.add(src);
    src.onended = () => this.sources.delete(src);
  },

  hush() {
    for (const s of this.sources) { try { s.stop(); } catch (e) { /* done */ } }
    this.sources.clear();
    this.playAt = 0;
  },
};

const Mic = {
  possible: !!(navigator.mediaDevices && navigator.mediaDevices.getUserMedia) && window.isSecureContext !== false,
  stream: null,
  node: null,
  source: null,
  heard: [],

  async on() {
    const ctx = Sound.wake();
    if (!ctx) throw new Error('this browser has no sound');
    // Woken in the tap that got here, and waited for: a context still
    // asleep hears nothing and says nothing about it.
    try { await ctx.resume(); } catch (e) { /* tried */ }
    this.stream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true },
    });
    try {
      if (ctx.state !== 'running') await ctx.resume();
      this.source = ctx.createMediaStreamSource(this.stream);
      this.node = await this.listener(ctx);
      // Silent, but connected: some browsers only run a node that leads
      // somewhere.
      const mute = ctx.createGain();
      mute.gain.value = 0;
      this.source.connect(this.node);
      this.node.connect(mute);
      mute.connect(ctx.destination);
    } catch (e) {
      this.off(); // not left listening with nothing to hear it
      throw e;
    }
  },

  // An AudioWorklet where the browser will load one, which a page opened
  // from a file often cannot (its blob: URLs belong to no origin), and
  // the older ScriptProcessor otherwise, which works everywhere.
  async listener(ctx) {
    const take = (data) => this.heard.push(data);
    if (ctx.audioWorklet && window.AudioWorkletNode) {
      const code = 'class M extends AudioWorkletProcessor{process(i){const c=i[0]&&i[0][0];' +
        'if(c)this.port.postMessage(c.slice(0));return true}}registerProcessor("friend-mic",M)';
      const blobUrl = URL.createObjectURL(new Blob([code], { type: 'application/javascript' }));
      const dataUrl = 'data:application/javascript;charset=utf-8,' + encodeURIComponent(code);
      const places = location.protocol === 'file:' ? [dataUrl, blobUrl] : [blobUrl, dataUrl];
      for (const url of places) {
        try {
          await ctx.audioWorklet.addModule(url);
          const node = new AudioWorkletNode(ctx, 'friend-mic');
          node.port.onmessage = (e) => take(e.data);
          URL.revokeObjectURL(blobUrl);
          return node;
        } catch (e) { /* the next way */ }
      }
      URL.revokeObjectURL(blobUrl);
    }
    const node = ctx.createScriptProcessor(2048, 1, 1);
    node.onaudioprocess = (e) => take(e.inputBuffer.getChannelData(0).slice(0));
    return node;
  },

  off() {
    if (this.stream) for (const t of this.stream.getTracks()) t.stop();
    try { if (this.source) this.source.disconnect(); if (this.node) this.node.disconnect(); } catch (e) { /* gone */ }
    this.stream = this.node = this.source = null;
    this.heard = [];
  },

  get on_() { return !!this.stream; },

  // What was heard since last time, into the friend, at the device's rate.
  feed() {
    if (!this.heard.length || !Sound.ctx) return;
    let n = 0;
    for (const c of this.heard) n += c.length;
    const max = Math.floor(inputSize / 2);
    const samples = new Int16Array(Math.min(n, max));
    let at = 0;
    for (const c of this.heard) {
      for (let i = 0; i < c.length && at < samples.length; i++) {
        const s = Math.max(-1, Math.min(1, c[i]));
        samples[at++] = s < 0 ? s * 32768 : s * 32767;
      }
    }
    this.heard = [];
    const p = putInput(new Uint8Array(samples.buffer));
    W.web_voice_push(p, samples.length, Math.round(Sound.ctx.sampleRate));
  },
};

const Voice = {
  async toggle() {
    if (!S.entry) return;
    if (!Settings.key()) { openSettings(); return; }
    if (W.web_voice_state() !== VOICE.OFF || Mic.on_) { this.stop(); return; }
    Sound.wake();
    if (W.web_voice_open() !== R.OK) { say(lastError(), true); return; }
    try {
      await Mic.on();
    } catch (e) {
      W.web_voice_close();
      say(e && e.name === 'NotAllowedError'
        ? 'the browser was not allowed to use the microphone'
        : 'the microphone could not be opened: ' + (e && e.message ? e.message : e), true);
    }
    UI.voice();
    schedulePump();
  },

  stop() {
    if (!W) return;
    if (W.web_voice_state() !== VOICE.OFF) W.web_voice_close();
    Mic.off();
    Sound.hush();
    UI.voice();
  },
};

/* ------------------------------------------------------------------ *
 *  The pump
 * ------------------------------------------------------------------ */

let pumpTimer = 0;
let pumpQueued = false;

function schedulePump() {
  if (pumpQueued) return;
  pumpQueued = true;
  setTimeout(pump, 0);
}

function pump() {
  pumpQueued = false;
  clearTimeout(pumpTimer);
  if (!W || S.broken) return;
  try {
    Mic.feed();
    W.web_tick(Date.now());
    for (;;) {
      const n = W.web_pending();
      const reqs = [];
      for (let i = 0; i < n; i++) reqs.push(readRequest(i));
      for (const r of reqs) Net.submit(r);
      if (n < 16) break;
    }
    for (;;) {
      const n = W.web_abandoned();
      for (let i = 0; i < n; i++) Net.abandon(W.web_abandoned_id(i));
      if (n < 16) break;
    }
    drainEvents();
    UI.refresh();
    Photos.pump();
    saveNow(false);
  } catch (e) {
    broke(e);
    return;
  }
  let wait = W.web_deadline();
  if (Mic.on_ || W.web_voice_state() !== VOICE.OFF) wait = 40;
  if (wait < 0 || wait > 1000) wait = 1000;
  pumpTimer = setTimeout(pump, wait);
}

function broke(e) {
  console.error(e);
  S.broken = true;
  Net.dropAll();
  say('friend-local stopped: ' + (e && e.message ? e.message : e) +
    '. What was last saved is safe; reload the page to carry on.', true);
}

function drainEvents() {
  for (;;) {
    const p = W.web_next_event();
    if (!p) break;
    const t = table(p);
    const kind = t[0];
    const index = t[1] | 0;
    switch (kind) {
    case EV.COMMITTED: UI.dirty.add(index); UI.headDirty = true; break;
    case EV.DELTA: UI.dirty.add(index); break;
    case EV.DONE:
      S.awaiting = false;
      UI.headDirty = true;
      if (index < 0) UI.rebuild = true; else UI.dirty.add(index);
      break;
    case EV.RETRACTED:
      S.awaiting = false;
      Sound.hush();
      UI.rebuild = true;
      break;
    case EV.AUDIO: {
      const n = t[6];
      const samples = new Int16Array(memory.buffer.slice(t[5], t[5] + n * 2));
      Sound.play(samples, t[7]);
      break;
    }
    case EV.THINKING: S.awaiting = true; break;
    case EV.MEMORIES: case EV.INTERIORITY: UI.headDirty = true; break;
    case EV.ERROR:
      S.awaiting = false;
      say(text(t[3], t[4]), true);
      break;
    default: break;
    }
  }
}

/* ------------------------------------------------------------------ *
 *  Photographs on messages, one at a time, as the library holds them
 * ------------------------------------------------------------------ */

const Photos = {
  queue: [],
  urls: [],

  want(message, index, el) { this.queue.push({ message, index, el, gen: S.gen, tries: 0 }); },

  forget() {
    for (const u of this.urls) URL.revokeObjectURL(u);
    this.urls = [];
    this.queue = [];
  },

  pump() {
    while (this.queue.length) {
      const job = this.queue[0];
      if (job.gen !== S.gen || !job.el.isConnected) { this.queue.shift(); continue; }
      const t = table(W.web_attachment(job.message, job.index));
      const r = t[0];
      if (r === R.BAD_STATE && ++job.tries < 300) return; // being read
      this.queue.shift();
      const caption = text(t[3], t[4]);
      if (r === R.OK) {
        const url = URL.createObjectURL(new Blob([copyOut(t[1], t[2])], { type: 'image/jpeg' }));
        this.urls.push(url);
        const img = document.createElement('img');
        img.className = 'photo';
        img.src = url;
        img.alt = caption || 'a photo';
        img.loading = 'lazy';
        img.onclick = () => showPhoto(url, caption);
        job.el.replaceWith(img);
      } else {
        job.el.textContent = caption ? caption : 'this photo was not kept';
        job.el.title = 'Its pixels did not come across; this is what was seen in it.';
      }
    }
  },
};

function showPhoto(url, caption) {
  $('viewer-img').src = url;
  $('viewer-caption').textContent = caption || '';
  $('viewer').showModal();
}

/* ------------------------------------------------------------------ *
 *  The screen
 * ------------------------------------------------------------------ */

function hashColor(name) {
  let h = 5381n;
  for (const b of encoder.encode(name)) h = ((h * 33n) ^ BigInt(b)) & 0xFFFFFFFFFFFFFFFFn;
  const i = Number(h % 5n);
  const hue = [335, 212, 145, 48][i];
  if (hue === undefined) return { top: 'hsl(0 0% 97%)', bottom: 'hsl(0 0% 82%)', wash: 'hsl(0 0% 97%)' };
  return { top: `hsl(${hue} 83% 82%)`, bottom: `hsl(${hue} 54% 61%)`, wash: `hsl(${hue} 32% 93%)` };
}

function paintAvatar(el, name) {
  const c = hashColor(name);
  el.style.setProperty('--top', c.top);
  el.style.setProperty('--bottom', c.bottom);
}

function icon(name) {
  const NS = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('viewBox', '0 0 256 256');
  svg.setAttribute('class', 'icon');
  svg.setAttribute('aria-hidden', 'true');
  const path = document.createElementNS(NS, 'path');
  path.setAttribute('d', ICONS[name] || '');
  svg.append(path);
  return svg;
}

function el(tag, cls, content) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (content !== undefined) e.textContent = content;
  return e;
}

function readMessage(i) {
  const t = table(W.web_message(i));
  if (t[0] !== R.OK) return null;
  return { kind: t[1], text: text(t[2], t[3]), attachments: t[4], spoken: t[5], streaming: t[6], ms: doubles()[0] };
}

const timeFormat = new Intl.DateTimeFormat(undefined, {
  year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: '2-digit',
});
const sinceFormat = new Intl.DateTimeFormat(undefined, { month: 'long', year: 'numeric' });
const HOUR = 3600 * 1000;
const WINDOW = 150;

const UI = {
  dirty: new Set(),
  rebuild: true,
  headDirty: true,
  first: 0,     // the oldest message drawn
  count: 0,     // how many there were when last drawn
  nodes: [],    // index - first -> { el, text, streaming, ms }
  gen: -1,
  lost: 0,
  narrow: matchMedia('(max-width: 767px)'),

  // On a phone, going into a conversation is a step the back gesture
  // undoes; on a wider screen both are there and nothing is pushed.
  showChat() {
    const app = $('app');
    if (!app.classList.contains('show-chat')) {
      app.classList.add('show-chat');
      if (this.narrow.matches && S.entry && !(history.state && history.state.chat)) {
        try { history.pushState({ chat: 1 }, ''); } catch (e) { /* file: */ }
      }
    }
    // It may have been drawn while hidden, with nothing to scroll.
    this.toEnd();
  },

  toEnd() {
    const sc = $('scroller');
    sc.scrollTop = sc.scrollHeight;
    requestAnimationFrame(() => { sc.scrollTop = sc.scrollHeight; });
  },

  showList() {
    $('app').classList.remove('show-chat');
  },

  async refreshList() {
    const list = (await Store.friends()).sort((a, b) => b.modified - a.modified);
    const ul = $('friends');
    ul.replaceChildren();
    for (const m of list) ul.append(this.row(m));
    $('welcome').hidden = !!S.entry;
    $('chat').classList.toggle('empty', !S.entry);
    if (!list.length && !S.entry) $('app').classList.add('show-chat');
    return list;
  },

  row(m) {
    const li = el('li', 'friend-row');
    li.dataset.id = m.id;
    li.tabIndex = 0;
    if (S.entry && S.entry.id === m.id) li.classList.add('current');
    const av = el('span', 'avatar');
    paintAvatar(av, m.name);
    const who = el('div', 'who');
    who.append(el('div', 'name', m.name),
      el('div', 'meta', plural(m.memories || 0, 'memory', 'memories') + ' · ' + plural(m.events || 0, 'message')));
    const actions = el('div', 'row-actions');
    const dl = el('button', 'icon-button');
    dl.type = 'button';
    dl.setAttribute('aria-label', 'Download ' + m.name);
    dl.append(icon('download'));
    dl.onclick = (e) => { e.stopPropagation(); exportFriend(m.id); };
    const del = el('button', 'icon-button');
    del.type = 'button';
    del.setAttribute('aria-label', 'Delete ' + m.name);
    del.append(icon('trash'));
    del.onclick = (e) => { e.stopPropagation(); deleteFriend(m.id); };
    actions.append(dl, del);
    li.append(av, who, actions);
    li.onclick = () => openFriend(m.id);
    li.onkeydown = (e) => { if (e.key === 'Enter') openFriend(m.id); };
    return li;
  },

  updateRow(meta) {
    const old = document.querySelector(`.friend-row[data-id="${meta.id}"]`);
    if (old) old.replaceWith(this.row(meta));
  },

  friendChanged() {
    Photos.forget();
    this.rebuild = true;
    this.headDirty = true;
    this.dirty.clear();
    const has = !!S.entry;
    $('chat').classList.toggle('empty', !has);
    $('welcome').hidden = has;
    for (const row of document.querySelectorAll('.friend-row')) {
      row.classList.toggle('current', has && row.dataset.id === S.entry.id);
    }
    const name = has ? friendName() || S.entry.name : '';
    document.title = has ? name + ' · friend' : 'friend';
    $('chat').style.setProperty('--wash', has ? hashColor(name).wash : 'var(--no-wash)');
    if (has) paintAvatar($('chat-avatar'), name);
    this.voice();
    this.composer();
    this.refresh();
  },

  head() {
    if (!S.entry) return;
    const name = friendName() || S.entry.name;
    $('chat-name').textContent = name;
    const st = stats();
    W.web_name();
    const created = doubles()[0];
    const parts = [plural(st.memories, 'memory', 'memories'), plural(st.connections, 'connection'),
      plural(W.web_message_count(), 'message') + ' held'];
    if (created > 0) parts.push('since ' + sinceFormat.format(new Date(created)));
    $('chat-stats').textContent = parts.join(' · ');
  },

  composer() {
    const t = $('text');
    const has = !!S.entry;
    if (!Settings.key()) t.placeholder = 'Add your API key in Settings to talk';
    else t.placeholder = has ? 'Message ' + (friendName() || S.entry.name) : 'Message';
    $('attach').hidden = !Settings.v.images;
    this.sendState();
  },

  sendState() {
    $('send').disabled = !$('text').value.trim() && !Composer.photos.length;
  },

  voice() {
    const state = W && S.entry ? W.web_voice_state() : VOICE.OFF;
    const on = state !== VOICE.OFF || Mic.on_;
    $('mic').hidden = !(Settings.v.voice && Mic.possible);
    $('mic').classList.toggle('on', on);
    $('mic').replaceChildren(icon(on ? 'mic-off' : 'mic'));
    $('mic').setAttribute('aria-label', on ? 'Stop talking' : 'Talk');
    $('voice-bar').hidden = !on;
    if (on) {
      $('voice-state').textContent = ['Listening', 'Listening', 'Hearing you', 'Thinking', 'Speaking'][state] || '';
      $('voice-level').firstElementChild.style.width = Math.round(Math.min(1, W.web_voice_level()) * 100) + '%';
    }
  },

  refresh() {
    if (!W) return;
    if (!S.entry) {
      if (this.count || this.nodes.length) { $('messages').replaceChildren(); this.nodes = []; this.count = 0; }
      $('typing').hidden = true;
      $('earlier').hidden = true;
      return;
    }
    const sc = $('scroller');
    const atEnd = sc.scrollHeight - sc.scrollTop - sc.clientHeight < 120;
    const n = W.web_message_count();
    const lost = stats().lost;
    if (this.rebuild || this.gen !== S.gen || n < this.count || lost !== this.lost) {
      this.draw(Math.max(0, n - WINDOW), n);
      this.gen = S.gen;
      this.lost = lost;
      this.rebuild = false;
      this.dirty.clear();
      this.toEnd();
    } else {
      for (const i of this.dirty) if (i >= this.first && i < this.count) this.update(i);
      for (let i = this.count; i < n; i++) this.append(i);
      this.count = n;
      this.dirty.clear();
      if (atEnd) sc.scrollTop = sc.scrollHeight;
    }
    const last = this.nodes[this.nodes.length - 1];
    const streamingText = last && last.streaming && last.text;
    $('typing').hidden = !(S.awaiting && !streamingText);
    if (atEnd && !$('typing').hidden) sc.scrollTop = sc.scrollHeight;
    if (this.headDirty) { this.head(); this.headDirty = false; }
    this.voice();
  },

  draw(first, n) {
    const box = $('messages');
    box.replaceChildren();
    this.nodes = [];
    this.first = first;
    this.count = first;
    for (let i = first; i < n; i++) this.append(i);
    this.count = n;
    $('earlier').hidden = first === 0;
  },

  showEarlier() {
    const sc = $('scroller');
    const fromEnd = sc.scrollHeight - sc.scrollTop;
    this.draw(Math.max(0, this.first - 200), W.web_message_count());
    sc.scrollTop = sc.scrollHeight - fromEnd;
  },

  append(i) {
    const m = readMessage(i);
    const box = $('messages');
    if (!m) { this.nodes.push({ el: null }); return; }
    const prev = this.nodes.length ? this.nodes[this.nodes.length - 1] : null;
    if (m.kind !== MSG.NOTICE && m.ms && (!prev || !prev.ms || m.ms - prev.ms > HOUR)) {
      box.append(el('div', 'time', timeFormat.format(new Date(m.ms))));
    }
    const node = { el: this.build(i, m), text: m.text, streaming: m.streaming, ms: m.ms || (prev && prev.ms) };
    this.nodes.push(node);
    box.append(node.el);
  },

  update(i) {
    const node = this.nodes[i - this.first];
    if (!node || !node.el) return;
    const m = readMessage(i);
    if (!m || (m.text === node.text && m.streaming === node.streaming)) return;
    const fresh = this.build(i, m);
    node.el.replaceWith(fresh);
    node.el = fresh;
    node.text = m.text;
    node.streaming = m.streaming;
  },

  build(i, m) {
    if (m.kind === MSG.NOTICE) return el('div', 'notice', m.text);
    if (m.kind === MSG.AMBIENT) {
      const d = el('div', 'msg ambient');
      d.append(icon('ear'), el('span', '', m.text));
      return d;
    }
    const mine = m.kind === MSG.USER;
    const d = el('div', 'msg ' + (mine ? 'user' : 'friend') + (m.streaming ? ' streaming' : ''));
    if (m.attachments) {
      const photos = el('div', 'photos');
      for (let a = 0; a < m.attachments; a++) {
        const holder = el('div', 'photo missing', '');
        photos.append(holder);
        Photos.want(i, a, holder);
      }
      d.append(photos);
    }
    if (m.text || !m.attachments) d.append(el('div', mine ? 'bubble' : 'text', m.text));
    if (m.spoken) {
      const s = el('div', 'spoken');
      s.append(icon('mic'), el('span', '', 'spoken'));
      d.append(s);
    }
    if (!mine && !m.streaming && Settings.v.voice && Settings.key() && m.text) {
      const b = el('button', 'icon-button tiny speak');
      b.type = 'button';
      b.setAttribute('aria-label', 'Say it out loud');
      b.append(icon('speaker'));
      b.onclick = () => {
        Sound.wake();
        if (W.web_speak(i) !== R.OK) say(lastError(), true);
        schedulePump();
      };
      d.append(b);
    }
    return d;
  },
};

let sayTimer = 0;
function say(message, isError) {
  const box = $('trouble');
  $('trouble-text').textContent = message;
  box.hidden = false;
  box.style.background = isError ? '' : '#f3f4f6';
  box.style.color = isError ? '' : 'var(--text)';
  clearTimeout(sayTimer);
  if (!isError) sayTimer = setTimeout(() => { box.hidden = true; }, 9000);
  if (isError) console.warn('[friend-local] ' + message);
}

/* ------------------------------------------------------------------ *
 *  Writing to it
 * ------------------------------------------------------------------ */

const Composer = {
  photos: [], // { bytes, url }

  async addPhotos(files) {
    for (const f of files) {
      if (this.photos.length >= 5) { say('a message carries five photos at most', true); break; }
      try {
        const bytes = await toJpeg(f);
        this.photos.push({ bytes, url: URL.createObjectURL(new Blob([bytes], { type: 'image/jpeg' })) });
      } catch (e) {
        say(f.name + ' could not be read as a picture', true);
      }
    }
    this.draw();
  },

  draw() {
    const box = $('attachments');
    box.replaceChildren();
    this.photos.forEach((p, i) => {
      const a = el('div', 'attachment');
      const img = document.createElement('img');
      img.src = p.url;
      img.alt = '';
      const x = el('button');
      x.type = 'button';
      x.setAttribute('aria-label', 'Remove');
      x.append(icon('x'));
      x.onclick = () => { URL.revokeObjectURL(p.url); this.photos.splice(i, 1); this.draw(); };
      a.append(img, x);
      box.append(a);
    });
    box.hidden = !this.photos.length;
    UI.sendState();
  },

  send() {
    if (!S.entry || S.broken) return;
    if (!Settings.key()) { openSettings(); return; }
    const t = $('text');
    const message = t.value.trim();
    if (!message && !this.photos.length) return;
    Sound.wake();
    let offset = 0;
    for (const p of this.photos) {
      W.web_attach(putInput(p.bytes, offset), p.bytes.length);
      offset += (p.bytes.length + 15) & ~15;
    }
    const [tp, tn] = putText(message, offset);
    const r = W.web_say(tp, tn);
    if (r !== R.OK) { say(lastError(), true); return; }
    for (const p of this.photos) URL.revokeObjectURL(p.url);
    this.photos = [];
    this.draw();
    t.value = '';
    fitComposer();
    $('trouble').hidden = true;
    UI.sendState();
    schedulePump();
    const sc = $('scroller');
    requestAnimationFrame(() => { sc.scrollTop = sc.scrollHeight; });
  },
};

// Around a thousand pixels on the long edge, which is what the service
// sent and what a request has room for.
async function toJpeg(file) {
  let source, w, h;
  try {
    source = await createImageBitmap(file, { imageOrientation: 'from-image' });
    w = source.width; h = source.height;
  } catch (e) {
    source = await new Promise((resolve, reject) => {
      const img = new Image();
      img.onload = () => resolve(img);
      img.onerror = reject;
      img.src = URL.createObjectURL(file);
    });
    w = source.naturalWidth; h = source.naturalHeight;
  }
  const scale = Math.min(1, 1024 / Math.max(w, h));
  const canvas = document.createElement('canvas');
  canvas.width = Math.max(1, Math.round(w * scale));
  canvas.height = Math.max(1, Math.round(h * scale));
  canvas.getContext('2d').drawImage(source, 0, 0, canvas.width, canvas.height);
  const blob = await new Promise((r) => canvas.toBlob(r, 'image/jpeg', 0.85));
  if (!blob) throw new Error('no picture');
  return new Uint8Array(await blob.arrayBuffer());
}

function fitComposer() {
  const t = $('text');
  t.style.height = 'auto';
  t.style.height = Math.min(t.scrollHeight, 160) + 'px';
}

/* ------------------------------------------------------------------ *
 *  Dialogs
 * ------------------------------------------------------------------ */

function ask({ title, text: body, input, ok, danger }) {
  const d = $('ask');
  $('ask-title').textContent = title;
  $('ask-text').textContent = body || '';
  $('ask-text').hidden = !body;
  const field = $('ask-input');
  field.hidden = !input;
  if (input) { field.value = input.value || ''; field.placeholder = input.placeholder || ''; field.maxLength = input.max || 64; }
  $('ask-ok').textContent = ok || 'OK';
  $('ask-ok').classList.toggle('danger', !!danger);
  d.returnValue = '';
  $('ask-form').onsubmit = (e) => {
    e.preventDefault();
    d.close(e.submitter ? e.submitter.value : 'ok');
  };
  return new Promise((resolve) => {
    d.onclose = () => {
      if (d.returnValue !== 'ok') resolve(null);
      else resolve(input ? field.value.trim() : true);
    };
    d.showModal();
    if (input) { field.focus(); field.select(); }
  });
}

function openSettings() {
  const v = Settings.v;
  const f = $('settings-form');
  f.querySelector(`input[name=provider][value="${v.provider}"]`).checked = true;
  fillProvider();
  $('keep-key').checked = v.keepKey;
  $('user-name').value = v.userName;
  $('images').checked = v.images;
  $('voice').checked = v.voice;
  $('voice').disabled = !Mic.possible;
  $('speak-replies').checked = v.speakReplies;
  $('voice-name').value = v.voiceName;
  storageStatus();
  $('settings').showModal();
}

function fillProvider() {
  const v = Settings.v;
  const p = v.provider;
  $('key').value = v.keys[p] || '';
  $('base-url').value = v.bases[p] || '';
  $('key-hint').replaceChildren();
  const a = document.createElement('a');
  a.target = '_blank';
  a.rel = 'noopener noreferrer';
  if (p === 0) {
    a.href = 'https://aistudio.google.com/apikey';
    a.textContent = 'Get a Gemini key';
    $('key-hint').append(a, document.createTextNode(' — there is a free tier. Gemini behaves most like the service did.'));
  } else {
    a.href = 'https://platform.openai.com/api-keys';
    a.textContent = 'Get an OpenAI key';
    $('key-hint').append(a, document.createTextNode(', or use one for whatever the base URL points at.'));
  }
  const box = $('models');
  box.replaceChildren();
  MODELS.forEach(([name, label], i) => {
    const l = el('label', 'field');
    const input = document.createElement('input');
    input.type = 'text';
    input.autocapitalize = 'off';
    input.spellcheck = false;
    input.dataset.model = name;
    input.placeholder = defaultModel(p, i);
    input.value = v.models[p][name] || '';
    l.append(el('span', '', label), input);
    box.append(l);
  });
}

async function storageStatus() {
  let line;
  if (!Store.lasting) {
    line = 'This browser will not keep anything here between visits. Download your friend before you close the page.';
  } else {
    line = Store.persisted ? 'Kept in this browser, which has agreed not to clear it.'
      : 'Kept in this browser. It may clear it if the device runs short of space.';
    if (navigator.storage && navigator.storage.estimate) {
      try {
        const e = await navigator.storage.estimate();
        if (e.usage) line += ' ' + (e.usage / 1048576).toFixed(1) + ' MB in use.';
      } catch (err) { /* not offered */ }
    }
  }
  $('storage-status').textContent = line;
  $('version').textContent = 'friend-local ' + VERSION;
}

async function settingsChanged(e) {
  const v = Settings.v;
  const target = e.target;
  if (target.name === 'provider') {
    v.provider = Number(target.value);
    fillProvider();
  } else if (target.id === 'key') v.keys[v.provider] = target.value.trim();
  else if (target.id === 'keep-key') v.keepKey = target.checked;
  else if (target.id === 'base-url') v.bases[v.provider] = target.value.trim();
  else if (target.id === 'user-name') v.userName = target.value;
  else if (target.id === 'images') v.images = target.checked;
  else if (target.id === 'voice') v.voice = target.checked;
  else if (target.id === 'speak-replies') v.speakReplies = target.checked;
  else if (target.id === 'voice-name') v.voiceName = target.value;
  else if (target.dataset.model) v.models[v.provider][target.dataset.model] = target.value.trim();
  else return;
  await Settings.store();
  $('welcome-key').hidden = !!Settings.key();
  if (!v.voice) Voice.stop();
  applyConfig();
  if (W.web_reconfigure() !== R.OK) say(lastError(), true);
  UI.composer();
  UI.voice();
  UI.rebuild = true;
  schedulePump();
}

/* ------------------------------------------------------------------ *
 *  Wiring
 * ------------------------------------------------------------------ */

function fitViewport() {
  // A phone's keyboard shrinks the visual viewport and not always the
  // layout one; the app is sized to what can be seen.
  const vv = window.visualViewport;
  const h = vv ? vv.height : window.innerHeight;
  document.documentElement.style.setProperty('--app-height', h + 'px');
  if (vv && vv.offsetTop) window.scrollTo(0, 0);
}

function wire() {
  for (const e of document.querySelectorAll('[data-icon]')) {
    e.prepend(icon(e.dataset.icon));
  }
  const mark = 'data:image/png;base64,' + $('friend-wordmark').textContent.trim();
  for (const img of document.querySelectorAll('.wordmark')) img.src = mark;

  fitViewport();
  if (window.visualViewport) {
    visualViewport.addEventListener('resize', fitViewport);
    visualViewport.addEventListener('scroll', fitViewport);
  }
  window.addEventListener('resize', fitViewport);

  $('open-settings').onclick = openSettings;
  for (const b of document.querySelectorAll('[data-open-settings]')) b.onclick = openSettings;
  $('settings-form').addEventListener('change', settingsChanged);
  $('settings-form').addEventListener('input', (e) => { if (e.target.type !== 'radio' && e.target.type !== 'checkbox') settingsChanged(e); });

  const newFriend = async () => {
    const name = await ask({ title: 'A new friend', text: 'What should it be called?', input: { placeholder: 'Name', max: 64 }, ok: 'Start' });
    if (name) createFriend(name);
  };
  $('new-friend').onclick = newFriend;
  $('welcome-new').onclick = newFriend;
  const importIt = () => $('import-input').click();
  $('import-friend').onclick = importIt;
  $('welcome-import').onclick = importIt;
  $('import-input').onchange = (e) => {
    const f = e.target.files && e.target.files[0];
    e.target.value = '';
    if (f) importFile(f);
  };

  $('back').onclick = () => {
    if (history.state && history.state.chat) history.back();
    else UI.showList();
  };
  window.addEventListener('popstate', () => {
    if (!(history.state && history.state.chat)) UI.showList();
    else if (S.entry) $('app').classList.add('show-chat');
  });
  $('export-current').onclick = () => S.entry && exportFriend(S.entry.id);
  $('delete-current').onclick = () => S.entry && deleteFriend(S.entry.id);
  $('show-earlier').onclick = () => UI.showEarlier();
  $('trouble-close').onclick = () => { $('trouble').hidden = true; };

  const t = $('text');
  t.addEventListener('input', () => { fitComposer(); UI.sendState(); });
  t.addEventListener('keydown', (e) => {
    // A keyboard with a send key sends with it; a phone's return is a
    // new line, and the button sends.
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing && !matchMedia('(pointer: coarse)').matches) {
      e.preventDefault();
      Composer.send();
    }
  });
  t.addEventListener('focus', () => {
    if (!Settings.key()) { t.blur(); openSettings(); }
  });
  $('composer').onsubmit = (e) => { e.preventDefault(); Composer.send(); };
  // Tapping send keeps the keyboard up.
  $('send').addEventListener('pointerdown', (e) => { if (document.activeElement === t) e.preventDefault(); });
  $('attach').onclick = () => $('photo-input').click();
  $('photo-input').onchange = (e) => {
    const files = [...(e.target.files || [])];
    e.target.value = '';
    Composer.addPhotos(files);
  };
  $('mic').onclick = () => Voice.toggle();
  $('voice-stop').onclick = () => Voice.stop();

  // Drop a file anywhere to import it.
  let depth = 0;
  const hasFiles = (e) => e.dataTransfer && [...e.dataTransfer.types].includes('Files');
  window.addEventListener('dragenter', (e) => { if (hasFiles(e)) { depth++; $('drop').hidden = false; } });
  window.addEventListener('dragleave', () => { if (--depth <= 0) { depth = 0; $('drop').hidden = true; } });
  window.addEventListener('dragover', (e) => { if (hasFiles(e)) e.preventDefault(); });
  window.addEventListener('drop', (e) => {
    depth = 0;
    $('drop').hidden = true;
    if (!hasFiles(e)) return;
    e.preventDefault();
    const f = e.dataTransfer.files[0];
    if (!f) return;
    if (f.type.startsWith('image/') && S.entry) Composer.addPhotos([...e.dataTransfer.files]);
    else importFile(f);
  });

  // Saved when the page goes away or out of sight, which on a phone is
  // the last chance there is.
  document.addEventListener('visibilitychange', () => { if (document.hidden) saveNow(true); });
  window.addEventListener('pagehide', () => saveNow(true));
}

// Served from a web address, the page can be put on a home screen and
// kept for offline: the manifest and sw.js beside it (frontend/web/site/)
// do that. Opened from a file there is nothing beside it to ask for.
function installable() {
  if (!/^https?:$/.test(location.protocol)) return;
  const link = (rel, href) => {
    const l = document.createElement('link');
    l.rel = rel;
    l.href = href;
    document.head.append(l);
  };
  link('manifest', 'manifest.webmanifest');
  link('apple-touch-icon', 'icon-180.png');
  if (navigator.serviceWorker) navigator.serviceWorker.register('sw.js').catch(() => { /* a plain page, then */ });
}

async function start() {
  installable();
  // A reload starts from the top, whatever the history says.
  try { if (history.state) history.replaceState(null, ''); } catch (e) { /* file: */ }
  wire();
  try {
    if (typeof WebAssembly !== 'object') throw new Error('this browser cannot run WebAssembly');
    await loadModule();
    await Store.open();
    await Settings.load();
    layOut(0, 0, 0);
  } catch (e) {
    $('chat').classList.add('empty');
    $('app').classList.add('show-chat');
    $('welcome').hidden = false;
    say('friend-local cannot start here: ' + (e && e.message ? e.message : e), true);
    return;
  }
  if (!Store.lasting) {
    const w = $('storage-warning');
    w.textContent = 'This browser will not keep your friends here between visits. Download them before you close the page.';
    w.hidden = false;
  }
  const list = await UI.refreshList();
  $('welcome-key').hidden = !!Settings.key();
  if (list.length) {
    // A phone opens on the list when there is a choice to make.
    await openFriend(list[0].id, !(UI.narrow.matches && list.length > 1));
  } else {
    UI.friendChanged();
  }
  schedulePump();
}

start();
