// web_export.mjs — the page's half of the library, run in node: the
// WebAssembly module imports a friend.com export, saves it and writes
// its photographs the way the page does, and friendlocal.js and the
// desktop's fr_export.c (through fl_check) each write the result as a
// .friendlocal. They must be the same bytes, each must read the other's,
// and a damaged one must be refused by both.
//
//   node web_export.mjs build/friend.wasm build/fl_check libfriend/tests/export.json
import { readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { friendHostImports } = require('../host.js');
const { writeFriendLocal, readFriendLocal } = require('../friendlocal.js');

const [wasmPath, checker, exportPath] = process.argv.slice(2);
let failed = 0, checks = 0;
const check = (ok, what) => { checks++; if (!ok) { failed++; console.log('  FAIL ' + what); } };

let memory;
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath),
  friendHostImports(() => memory, () => {}));
const W = instance.exports;
memory = W.memory;
const u8 = () => new Uint8Array(memory.buffer);
const t32 = (p) => new Uint32Array(memory.buffer, p, 32);
const f64 = () => new Float64Array(memory.buffer, W.web_f64(), 8);
const enc = new TextEncoder(), dec = new TextDecoder();
const put = (bytes) => { u8().set(bytes, W.web_input()); return W.web_input(); };
const str = (field, s) => { const b = enc.encode(s); W.web_config_str(field, put(b), b.length); };

// Laid out and configured as the page does it, with a photo file.
const base = W.web_heap_base();
const json = readFileSync(exportPath);
const need = W.web_limits(64, 512, json.length);
const grow = (end) => { if (end > memory.buffer.byteLength) memory.grow(Math.ceil((end - memory.buffer.byteLength) / 65536)); };
grow(base + need + json.length + (1 << 20));
W.web_config_reset();
str(4, 'test.blob');
check(W.web_init(base, need) === 0, 'the friend is set up');
W.web_tick(Date.now());
const at = (base + need + 15) & ~15;
u8().set(json, at);
const rep = t32(W.web_import(at, json.length));
check(rep[0] === 0, 'the export imports');
const memories = rep[1];

// The photographs go out as file writes, served here as the page does.
let blob = new Uint8Array(0);
for (let round = 0; round < 50; round++) {
  W.web_tick(Date.now());
  const n = W.web_pending();
  for (let i = 0; i < n; i++) {
    const t = t32(W.web_request(i));
    const id = f64()[0], offset = f64()[1];
    if (t[0] === 5) { // FILE_WRITE
      const body = u8().slice(t[8], t[8] + t[9]);
      const grown = new Uint8Array(Math.max(blob.length, offset + body.length));
      grown.set(blob); grown.set(body, offset); blob = grown;
      W.web_io_complete(id, 0);
    } else {
      const why = enc.encode('no network in this test');
      W.web_io_failed(id, put(why), why.length);
    }
  }
  if (t32(W.web_stats())[6] === 0 && round > 2) break; // photos_waiting
}
check(blob.length > 0, 'the photographs were written out');

const size = W.web_save_size();
grow(at + size);
const written = W.web_save(at, size);
check(written > 0, 'the friend saves');
const save = u8().slice(at, at + written);
const peek = (bytes) => {
  u8().set(bytes, W.web_input());
  const t = t32(W.web_peek(W.web_input(), bytes.length));
  return t[0] === 0 ? { name: dec.decode(u8().slice(t[1], t[1] + t[2])), memories: t[3], events: f64()[1] } : null;
};

const dir = mkdtempSync(join(tmpdir(), 'friend-local-web-'));
const when = 1790000000123;
writeFileSync(join(dir, 'f.friend'), save);
writeFileSync(join(dir, 'f.blob'), blob);
const ours = writeFriendLocal(save, blob, when);
writeFileSync(join(dir, 'ours.friendlocal'), ours);
const run = (...args) => {
  try { return execFileSync(checker, args, { encoding: 'utf8' }).trim(); } catch (e) { return (e.stdout || '').trim(); }
};
check(run('write', join(dir, 'f.friend'), join(dir, 'f.blob'), join(dir, 'theirs.friendlocal'), String(when)) === 'ok',
  'the desktop writes one');
const theirs = new Uint8Array(readFileSync(join(dir, 'theirs.friendlocal')));
check(theirs.length === ours.length && theirs.every((b, i) => b === ours[i]), 'the page writes the same bytes as the desktop');
check(run('read', join(dir, 'ours.friendlocal')) === `ok ${memories} ${save.length} ${blob.length}`, 'the desktop reads the page\'s');
const back = readFriendLocal(theirs, peek);
check(back.save.length === save.length && back.save.every((b, i) => b === save[i]), 'the page reads the desktop\'s friend');
check(back.photos.length === blob.length && back.photos.every((b, i) => b === blob[i]), 'the page reads the desktop\'s photos');
check(back.info.memories === memories, 'and what it says about itself');

// No photographs: one section, both sides.
const bare = writeFriendLocal(save, null, when);
check(run('write', join(dir, 'f.friend'), '-', join(dir, 'bare.friendlocal'), String(when)) === 'ok', 'the desktop writes one with no photos');
const bareTheirs = new Uint8Array(readFileSync(join(dir, 'bare.friendlocal')));
check(bareTheirs.length === bare.length && bareTheirs.every((b, i) => b === bare[i]), 'and so does the page, the same');

// Damage, refused by both: every byte of the header and table flipped
// in turn, a photo byte, a short file, a longer one.
const refusedBoth = (bytes, what) => {
  writeFileSync(join(dir, 'bad.friendlocal'), bytes);
  let pageRefused = false;
  try { readFriendLocal(bytes, peek); } catch (e) { pageRefused = true; }
  check(pageRefused, 'the page refuses ' + what);
  check(run('read', join(dir, 'bad.friendlocal')).startsWith('refused'), 'the desktop refuses ' + what);
};
const tableEnd = 64 + 2 * 32;
for (let i = 0; i < tableEnd; i++) {
  const bad = ours.slice(); bad[i] ^= 0x41;
  refusedBoth(bad, 'header byte ' + i + ' changed');
}
{ const bad = ours.slice(); bad[bad.length - 20] ^= 1; refusedBoth(bad, 'a photo byte changed'); }
refusedBoth(ours.slice(0, ours.length - 8), 'a file cut short');
{ const longer = new Uint8Array(ours.length + 8); longer.set(ours); refusedBoth(longer, 'a file added to'); }

console.log(`${checks} checks, ${failed} failed`);
process.exit(failed ? 1 : 0);
