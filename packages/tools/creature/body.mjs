// SPDX-License-Identifier: MIT
// A Molty's body and its actions (docs/CREATURE.md), read into one plain form whatever they were
// written as: a glTF file (.glb or .gltf: the skeleton as nodes, the actions as animations, the
// shapes as nodes marked in `extras`), or the same thing written out by hand inside molty.json.
//   { nodes: [{ name, parent, t, r, s, foot? }],          parent first; y up, facing +z, its left +x
//     shapes: [{ shape, node, at, turn?, radii?, to?, radius?, flat?, flat_axis?, stiffness?, color, outline }],
//     actions: [{ name, view?, follow?, stride?, from?, to?, tracks: [{ node, path: 't'|'r'|'s', times, values }] }] }
// Nothing here knows which tool made the file.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';

export const SHAPES = ['blob', 'ellipsoid', 'capsule', 'cone', 'cylinder', 'patch'];
const PATHS = { translation: 't', rotation: 'r', scale: 's' }, WIDTH = { t: 3, r: 4, s: 3 };
const fail = (m) => { throw new Error(m); };

/** A .glb or .gltf file as the plain form. Throws with what is wrong and what to do. */
export function readGltf(file) {
  const bytes = readFileSync(file);
  let doc, bin = null;
  if (bytes.readUInt32LE(0) === 0x46546c67) {   // "glTF": a binary file, a JSON chunk then a data chunk
    const n = bytes.readUInt32LE(12);
    doc = JSON.parse(bytes.toString('utf8', 20, 20 + n));
    if (20 + n < bytes.length) bin = bytes.subarray(28 + n, 28 + n + bytes.readUInt32LE(20 + n));
  } else doc = JSON.parse(bytes.toString('utf8'));
  const buffers = (doc.buffers ?? []).map((b) => (b.uri === undefined ? bin : b.uri.startsWith('data:') ? Buffer.from(b.uri.slice(b.uri.indexOf(',') + 1), 'base64') : readFileSync(join(dirname(file), decodeURIComponent(b.uri)))));
  /** An accessor's numbers (floats only: what every exporter writes for animation). */
  const read = (i, what) => {
    const a = doc.accessors[i], view = doc.bufferViews[a.bufferView], w = { SCALAR: 1, VEC3: 3, VEC4: 4 }[a.type];
    if (a.componentType !== 5126 || !w || a.sparse) fail(`${what} is not stored as plain floats: export the animation with float keys`);
    const buf = buffers[view.buffer], at = (view.byteOffset ?? 0) + (a.byteOffset ?? 0), out = new Float64Array(a.count * w);
    for (let k = 0; k < out.length; k++) out[k] = buf.readFloatLE(at + k * 4);
    return out;
  };
  // every node is a bone, unless it is marked as a shape, or is only there to carry a mesh, a camera or a light
  const all = doc.nodes ?? [], up = new Map(), kept = [], index = new Map(), shapes = [];
  all.forEach((n, i) => (n.children ?? []).forEach((c) => up.set(c, i)));
  const isBone = (n) => !n.extras?.shape && !((n.mesh !== undefined || n.camera !== undefined) && !(n.children ?? []).length);
  for (let left = all.map((_, i) => i).filter((i) => isBone(all[i])); left.length;) {   // as the file has them, a bone moved after its parent
    const before = kept.length;
    for (const i of left) if (!up.has(i) || index.has(up.get(i)) || !isBone(all[up.get(i)])) { index.set(i, kept.length); kept.push(i); }
    if (kept.length === before) fail('the nodes form a loop');
    left = left.filter((i) => !index.has(i));
  }
  const nodes = kept.map((i) => {
    const n = all[i];
    if (n.matrix) fail(`node ${n.name}: its place is a matrix; export translation, rotation and scale`);
    // a glTF number is single precision, whatever digits its text has: the same value its keys hold
    const f32 = (v) => v.map(Math.fround);
    return { name: n.name ?? `node${i}`, parent: index.get(up.get(i)) ?? -1, t: f32(n.translation ?? [0, 0, 0]), r: f32(n.rotation ?? [0, 0, 0, 1]), s: f32(n.scale ?? [1, 1, 1]), ...(n.extras?.foot ? { foot: n.extras.foot } : {}) };
  });
  all.forEach((n, i) => {
    if (!n.extras?.shape) return;
    if (!index.has(up.get(i))) fail(`shape ${n.name}: it must be the child of a bone`);
    const e = n.extras, round = ['blob', 'ellipsoid', 'patch'].includes(e.shape);
    shapes.push({ ...e, node: index.get(up.get(i)), at: n.translation ?? [0, 0, 0], ...(round ? { turn: n.rotation ?? [0, 0, 0, 1], radii: n.scale ?? [1, 1, 1] } : {}) });
  });
  const actions = (doc.animations ?? []).map((a, k) => {
    const name = a.name ?? `action${k}`, tracks = [];
    for (const c of a.channels) {
      const s = a.samplers[c.sampler], path = PATHS[c.target.path], node = index.get(c.target.node);
      if (!path || node === undefined) continue;   // morph weights, or a node that is not a bone
      if ((s.interpolation ?? 'LINEAR') === 'CUBICSPLINE') fail(`action ${name}: ${all[c.target.node].name} uses curve keys; export with the animation sampled (one key a frame)`);
      tracks.push({ node, path, times: read(s.input, `action ${name}`), values: read(s.output, `action ${name}`), step: s.interpolation === 'STEP' });
    }
    return { name, ...a.extras, tracks };
  });
  return { nodes, shapes, actions };
}

const RAD = Math.PI / 180;
/** Degrees about x, then y, then z, as a quaternion (x, y, z, w). */
function quat([x = 0, y = 0, z = 0]) {
  const [cx, sx, cy, sy, cz, sz] = [x, y, z].flatMap((a) => [Math.cos((a * RAD) / 2), Math.sin((a * RAD) / 2)]);
  return [sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz];
}

/**
 * A body written by hand in molty.json: { bones, shapes, actions }. Turns are degrees (about x, y, z); an action is a
 * list of keys ({ time, <bone>: { at, turn, scale } }), joined by straight lines and read once a frame at `fps`.
 */
export function readInline(body, fps) {
  const names = body.bones.map((b) => b.name), at = (name, what) => { const i = names.indexOf(name); if (i < 0) fail(`${what}: there is no bone named ${name} (bones: ${names.join(', ')})`); return i; };
  const nodes = body.bones.map((b, i) => {
    const parent = b.parent === undefined ? -1 : at(b.parent, `bone ${b.name}`);
    if (parent >= i) fail(`bone ${b.name}: list it after its parent ${b.parent}`);
    return { name: b.name, parent, t: b.at ?? [0, 0, 0], r: quat(b.turn ?? []), s: b.scale ?? [1, 1, 1], ...(b.foot ? { foot: b.foot } : {}) };
  });
  const shapes = (body.shapes ?? []).map(({ on, turn, ...s }) => ({ ...s, node: at(on, `a ${s.shape}`), at: s.at ?? [0, 0, 0], ...(['blob', 'ellipsoid', 'patch'].includes(s.shape) ? { turn: quat(turn ?? []), radii: s.radii ?? [0.1, 0.1, 0.1] } : {}) }));
  const actions = Object.entries(body.actions ?? {}).map(([name, { keys, ...facts }]) => {
    // read once a frame, and at its end when that falls between two frames
    const end = keys.at(-1).time, whole = Math.floor(end * fps + 1e-6), moments = [...Array.from({ length: whole + 1 }, (_, i) => i / fps), ...(end - whole / fps > 1e-6 ? [end] : [])], tracks = [];
    for (const bone of new Set(keys.flatMap((k) => Object.keys(k).filter((n) => n !== 'time')))) {
      const node = at(bone, `action ${name}`);
      for (const [prop, path] of [['at', 't'], ['turn', 'r'], ['scale', 's']]) {
        const set = keys.filter((k) => k[bone]?.[prop]).map((k) => [k.time, k[bone][prop]]);
        if (!set.length) continue;
        // a straight line between the keys around each frame; before the first and after the last it holds
        const value = (time) => { const b = set.findIndex(([t]) => t >= time), [t0, v0] = set[Math.max(0, (b < 0 ? set.length : b) - 1)], [t1, v1] = set[b < 0 ? set.length - 1 : b], u = t1 > t0 ? Math.min(1, Math.max(0, (time - t0) / (t1 - t0))) : 0; return v0.map((v, c) => v + ((v1[c] ?? v) - v) * u); };
        const w = WIDTH[path], values = new Float64Array(moments.length * w);
        moments.forEach((time, i) => values.set(prop === 'turn' ? quat(value(time)) : value(time), i * w));
        tracks.push({ node, path, times: Float64Array.from(moments), values });
      }
    }
    return { name, ...facts, tracks };
  });
  return { nodes, shapes, actions };
}
