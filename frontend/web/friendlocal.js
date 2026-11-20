/* friendlocal.js — the .friendlocal file, as platform/fr_export.h lays it
 * out: one friend, photographs and all, to take to another device. The
 * page writes and reads exactly what the desktop does, and
 * tests/web_export.mjs holds it to that byte for byte. Pure: what it
 * needs of the library (friend_save_peek) is passed in. */

const FRIEND_LOCAL = {
  MAGIC: 'friend-local', VERSION: 1, OLDEST_READER: 1, HEADER: 64, ENTRY: 32, SECTIONS_MAX: 64, REQUIRED: 1,
};
function exportTag(s) { return (s.charCodeAt(0) | (s.charCodeAt(1) << 8) | (s.charCodeAt(2) << 16) | (s.charCodeAt(3) << 24)) >>> 0; }
const TAG_FRIEND = exportTag('FRND');
const TAG_PHOTOS = exportTag('BLOB');

let crcTable = null;
function crc32(crc, bytes) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  crc = ~crc >>> 0;
  for (let i = 0; i < bytes.length; i++) crc = crcTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8);
  return ~crc >>> 0;
}

function headerCrc(file, headerBytes, tableBytes) {
  let c = crc32(0, file.subarray(0, 48));
  c = crc32(c, new Uint8Array(4));
  c = crc32(c, file.subarray(52, headerBytes));
  return crc32(c, file.subarray(headerBytes, headerBytes + tableBytes));
}

const pad8 = (n) => (8 - (n & 7)) & 7;
function put64(view, at, v) { view.setUint32(at, v % 4294967296, true); view.setUint32(at + 4, Math.floor(v / 4294967296), true); }
function get64(view, at) { return view.getUint32(at, true) + view.getUint32(at + 4, true) * 4294967296; }

function writeFriendLocal(save, photos, now) {
  const F = FRIEND_LOCAL;
  const count = photos && photos.length ? 2 : 1;
  const tableEnd = F.HEADER + count * F.ENTRY;
  const friendAt = tableEnd;
  const photosAt = friendAt + save.length + pad8(save.length);
  const size = count === 2 ? photosAt + photos.length + pad8(photos.length) : photosAt;
  const file = new Uint8Array(size);
  const v = new DataView(file.buffer);
  file.set(new TextEncoder().encode(F.MAGIC), 0);
  v.setUint32(12, F.HEADER, true);
  v.setUint32(16, F.VERSION, true);
  v.setUint32(20, F.OLDEST_READER, true);
  v.setUint32(24, count, true);
  put64(v, 32, now);
  put64(v, 40, size);
  const entry = (i, tag, at, bytes) => {
    const e = F.HEADER + i * F.ENTRY;
    v.setUint32(e, tag, true);
    v.setUint32(e + 4, F.REQUIRED, true);
    put64(v, e + 8, at);
    put64(v, e + 16, bytes.length);
    v.setUint32(e + 24, crc32(0, bytes), true);
    file.set(bytes, at);
  };
  entry(0, TAG_FRIEND, friendAt, save);
  if (count === 2) entry(1, TAG_PHOTOS, photosAt, photos);
  v.setUint32(48, headerCrc(file, F.HEADER, tableEnd - F.HEADER), true);
  return file;
}

// Checks all of it before anything is used, as fr_export_open does, and
// says what is wrong in the same words.
function readFriendLocal(file, peek) {
  const F = FRIEND_LOCAL;
  const v = new DataView(file.buffer, file.byteOffset, file.byteLength);
  const size = file.length;
  if (!friendLocalLooksLike(file)) throw new Error('it is not a friend-local export');
  if (size < F.HEADER) throw new Error('it is cut short: it ends inside its header');
  const headerBytes = v.getUint32(12, true);
  const version = v.getUint32(16, true);
  const oldest = v.getUint32(20, true);
  const count = v.getUint32(24, true);
  const fileBytes = get64(v, 40);
  if (oldest > F.VERSION) {
    throw new Error('it was exported by a newer friend-local (export format ' + version + ', readable from format ' +
      oldest + ' on); this one reads format ' + F.VERSION + ', so update it to open this friend');
  }
  if (version === 0 || oldest === 0 || oldest > version || headerBytes < F.HEADER || headerBytes > size ||
      count === 0 || count > F.SECTIONS_MAX) {
    throw new Error('it is damaged: its header does not make sense');
  }
  if (fileBytes !== size) {
    throw new Error('it is ' + size + ' bytes and was written as ' + fileBytes + ': it was ' +
      (size < fileBytes ? 'cut short' : 'added to') + ' on the way here');
  }
  const tableEnd = headerBytes + count * F.ENTRY;
  if (tableEnd > size) throw new Error('it is damaged: its table of contents runs past its end');
  if (v.getUint32(48, true) !== headerCrc(file, headerBytes, tableEnd - headerBytes)) {
    throw new Error('it is damaged: its header does not match its checksum');
  }
  let save = null, photos = null, friends = 0, photoParts = 0;
  for (let i = 0; i < count; i++) {
    const e = headerBytes + i * F.ENTRY;
    const tag = v.getUint32(e, true);
    const flags = v.getUint32(e + 4, true);
    const offset = get64(v, e + 8);
    const length = get64(v, e + 16);
    const crc = v.getUint32(e + 24, true);
    if (tag !== TAG_FRIEND && tag !== TAG_PHOTOS) {
      if (flags & F.REQUIRED) {
        const name = String.fromCharCode(tag & 255, (tag >> 8) & 255, (tag >> 16) & 255, (tag >>> 24) & 255);
        throw new Error('it holds a part this friend-local does not know (' + name +
          ') and cannot do without; update it to open this friend');
      }
      continue;
    }
    if (offset < tableEnd || offset > size || length > size - offset) {
      throw new Error('it is damaged: a part of it runs past its end');
    }
    const part = file.subarray(offset, offset + length);
    if (crc32(0, part) !== crc) {
      throw new Error('it is damaged: its ' + (tag === TAG_FRIEND ? 'memories and messages' : 'photographs') +
        ' do not match their checksum');
    }
    if (tag === TAG_FRIEND) { if (friends++) break; save = part; } else { if (photoParts++) break; photos = length ? part : null; }
  }
  if (friends !== 1 || photoParts > 1) throw new Error('it is damaged: it holds ' + (friends === 0 ? 'no friend' : 'a part twice'));
  const info = peek(save);
  if (!info) throw new Error('the friend in it was saved by a friend-local this one cannot read; update it to open this friend');
  return { save: save.slice(), photos: photos ? photos.slice() : null, info };
}

function friendLocalLooksLike(bytes) {
  const m = FRIEND_LOCAL.MAGIC;
  if (bytes.length < m.length) return false;
  for (let i = 0; i < m.length; i++) if (bytes[i] !== m.charCodeAt(i)) return false;
  return true;
}

if (typeof module !== 'undefined') module.exports = { FRIEND_LOCAL, writeFriendLocal, readFriendLocal, crc32 };
