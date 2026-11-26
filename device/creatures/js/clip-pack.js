// SPDX-License-Identifier: MIT
// A clip's channels packed into one string (docs/CREATURES.md section 12, "Packed clips"): the same keys and the same
// small integers as the plain form ([node, path, S|L|C, bytes, keys, values], each in its own base64), so a packed
// clip plays exactly as its plain form did; only how they are written changes. Per channel a short header, then each
// series (the key frames, then each component of the values over the keys) as its first number and the steps from one
// to the next, all steps of a series in the fewest bits that hold the largest. The C port is creature_clip.c unpack().
//
//   bits, least significant first in each byte, the last byte padded with zeros:
//   clip      count (16), then each channel
//   channel   node (8), path (2: t r s), interpolation (2: S L C), one byte a value (1), keys n (16),
//             keys:  first (16), then if n > 1 width w (5) and n - 1 steps less one (w each)
//             each value component (3 for t and s, 4 for r; three times that for C: in-tangent, value, out-tangent):
//                    first (16, or 8 for one byte a value, two's complement), then if n > 1 width w (5) and n - 1
//                    steps (w each, zigzag: 0, -1, 1, -2, ...)

const WIDTH = { t: 3, r: 4, s: 3 }, PATHS = ['t', 'r', 's'], INTERP = ['S', 'L', 'C'];
const b64 = (s) => (typeof atob === 'function' ? Uint8Array.from(atob(s), (c) => c.charCodeAt(0)) : new Uint8Array(Buffer.from(s, 'base64')));
const toB64 = (u) => (typeof btoa === 'function' ? btoa(String.fromCharCode(...u)) : Buffer.from(u).toString('base64'));
const zig = (v) => (v < 0 ? -2 * v - 1 : 2 * v), unzig = (u) => (u & 1 ? -(u + 1) / 2 : u / 2);
const bitsOf = (u) => (u ? 32 - Math.clz32(u) : 0);

/** The plain channels of a clip ([node, path, S|L|C, bytes, keys b64, values b64]) as one packed string. */
export function packClip(ch, frames) {
  const out = [];
  let acc = 0, nb = 0;
  const put = (v, n) => { for (let i = 0; i < n; i++) { acc |= ((v >>> i) & 1) << nb; if (++nb === 8) { out.push(acc); acc = 0; nb = 0; } } };
  if (ch.length > 65535) throw new Error('packClip: too many channels');
  put(ch.length, 16);
  for (const [node, path, interp, bytes, inp, outp] of ch) {
    const kb = b64(inp), keys = frames < 256 ? [...kb] : [...new Uint16Array(kb.buffer, kb.byteOffset, kb.length / 2)];
    const ob = b64(outp), vals = bytes === 1 ? [...new Int8Array(ob.buffer, ob.byteOffset, ob.length)] : [...new Int16Array(ob.buffer.slice(ob.byteOffset, ob.byteOffset + ob.length))];
    const per = WIDTH[path] * (interp === 'C' ? 3 : 1), n = keys.length;
    if (node < 0 || node > 255 || !PATHS.includes(path) || !INTERP.includes(interp) || n < 1 || n > 65535 || vals.length !== per * n) throw new Error('packClip: a channel');
    put(node, 8); put(PATHS.indexOf(path), 2); put(INTERP.indexOf(interp), 2); put(bytes === 1 ? 1 : 0, 1); put(n, 16);
    put(keys[0], 16);
    if (n > 1) { const d = keys.slice(1).map((k, i) => k - keys[i] - 1), w = Math.max(...d.map(bitsOf)); put(w, 5); for (const u of d) put(u, w); }
    for (let c = 0; c < per; c++) {
      const xs = Array.from({ length: n }, (_, k) => vals[k * per + c]);
      put(xs[0] & (bytes === 1 ? 255 : 65535), bytes === 1 ? 8 : 16);
      if (n < 2) continue;
      const d = xs.slice(1).map((x, k) => zig(x - xs[k])), w = Math.max(...d.map(bitsOf));
      put(w, 5);
      for (const u of d) put(u, w);
    }
  }
  if (nb) out.push(acc);
  return toB64(Uint8Array.from(out));
}

/** A packed clip back into its channels, decoded: { node, path, interp ('S' 'L' 'C'), b8, keys, vals } with vals the
 *  small integers the plain form holds (Int8Array or Int16Array). */
export function unpackClip(s) {
  const u = b64(s);
  let pos = 0;
  const get = (n) => {
    let v = 0;
    for (let i = 0; i < n; i++, pos++) {
      if (pos >> 3 >= u.length) throw new Error('unpackClip: it ends early');
      v |= ((u[pos >> 3] >> (pos & 7)) & 1) << i;
    }
    return v >>> 0;
  };
  const nch = get(16), ch = [];
  for (let h = 0; h < nch; h++) {
    const node = get(8), path = PATHS[get(2)], interp = INTERP[get(2)], b8 = get(1) === 1, n = get(16);
    if (!path || !interp || !n || (b8 && path !== 'r')) throw new Error('unpackClip: a channel');
    const keys = new Uint16Array(n);
    keys[0] = get(16);
    if (n > 1) { const w = get(5); for (let k = 1; k < n; k++) keys[k] = keys[k - 1] + get(w) + 1; }
    const per = WIDTH[path] * (interp === 'C' ? 3 : 1), vals = b8 ? new Int8Array(per * n) : new Int16Array(per * n);
    for (let c = 0; c < per; c++) {
      let x = get(b8 ? 8 : 16);
      x = b8 ? (x << 24) >> 24 : (x << 16) >> 16;
      vals[c] = x;
      if (n < 2) continue;
      const w = get(5);
      for (let k = 1; k < n; k++) { x += unzig(get(w)); vals[k * per + c] = x; }
    }
    ch.push({ node, path, interp, b8, keys, vals });
  }
  return ch;
}
