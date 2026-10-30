// Route-2 creatures built in code: 'Mochi' (blob) and 'Burr' (blob + spike ring).
// Everything is device-native: flat Lambert, one colour per face, < 600 tris with items.
//
// The body is a low-poly icosphere deformed EVERY frame from its rest shape by one
// function, deform(p) = Squash · Lean(y) · HeadTurn(w(y)) · p, which is also used for the
// eyes, mouth, spikes and item sockets so all parts stay glued to the surface. This is the
// "rigid parts + a few matrices" runtime the moodboard proposes for Phase 2.

import * as THREE from 'three';

const V = () => new THREE.Vector3();
const smooth = (a, b, x) => { const t = Math.min(1, Math.max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t); };

export const SKINS = {
  mochi: { body: '#f6b8c8', accent: '#fff3ec', pattern: 'belly' },
  burr: { body: '#8fc46b', accent: '#3f6b3a', pattern: 'spots' },
};
export const PALETTE = ['#f6b8c8', '#fff3ec', '#a8d08d', '#8fc46b', '#9ccbf0', '#f5df7a', '#b49be0', '#f39a6b', '#3a3a48', '#3f6b3a'];
export const PATTERNS = ['solid', 'belly', 'spots', 'stripes', 'cap'];
export const HEAD_ITEMS = ['none', 'party hat', 'top hat', 'leaf sprout', 'headphones'];
export const NECK_ITEMS = ['none', 'scarf', 'bow tie', 'bell collar'];

const lam = (c) => new THREE.MeshLambertMaterial({ color: c, flatShading: true });

export class ProcCreature {
  constructor(entry) {
    this.entry = entry; this.variant = entry.variant;
    this.root = new THREE.Group();
    this.body = new THREE.Group(); this.root.add(this.body);
    this.clips = []; this.height = 1;
    this.skin = { ...SKINS[this.variant], blush: this.variant === 'mochi' };
    this.items = { head: this.variant === 'burr' ? 'leaf sprout' : 'none', neck: this.variant === 'mochi' ? 'scarf' : 'none' };
    this._build();
  }
  async load() { return this; }
  defaultClip() { return null; }
  play() {} setSpeed() {} setPaused() {}
  setDevice() { return this.tris(); }

  // ---------- rest shape ----------
  _shape(dir) {
    // map a unit-sphere direction to the rest surface (y up, feet at 0, height 1)
    const p = dir.clone();
    if (this.variant === 'mochi') { p.y *= 0.8; p.z *= 0.93; if (p.y < -0.42) p.y = -0.42 + (p.y + 0.42) * 0.25; }
    else { p.y *= 0.95; if (p.y < -0.62) p.y = -0.62 + (p.y + 0.62) * 0.25; }
    p.y -= this._minY; p.multiplyScalar(this._k);
    return p;
  }
  _build() {
    // compute normalisation (min y, scale to height 1)
    this._minY = 0; this._k = 1;
    const lo = this._shape(new THREE.Vector3(0, -1, 0)).y, hi = this._shape(new THREE.Vector3(0, 1, 0)).y;
    this._minY = lo; this._k = 1 / (hi - lo);

    // body: icosphere detail 3 = 320 tris, non-indexed; deform unique verts then scatter
    const ico = new THREE.IcosahedronGeometry(1, 3);
    const pos = ico.attributes.position;
    const keyOf = (i) => `${pos.getX(i).toFixed(4)},${pos.getY(i).toFixed(4)},${pos.getZ(i).toFixed(4)}`;
    const uniq = new Map(); this.vmap = new Int32Array(pos.count); this.restV = [];
    for (let i = 0; i < pos.count; i++) {
      const k = keyOf(i); let id = uniq.get(k);
      if (id === undefined) { id = this.restV.length; uniq.set(k, id); this.restV.push(this._shape(V().fromBufferAttribute(pos, i).normalize())); }
      this.vmap[i] = id;
    }
    this.faceDir = [];
    for (let f = 0; f < pos.count / 3; f++) {
      const c = V(); for (let k = 0; k < 3; k++) c.add(V().fromBufferAttribute(pos, f * 3 + k)); this.faceDir.push(c.normalize());
    }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(pos.count * 3), 3));
    geo.setAttribute('color', new THREE.BufferAttribute(new Float32Array(pos.count * 3), 3));
    this.bodyGeo = geo; ico.dispose();
    this.bodyMesh = new THREE.Mesh(geo, new THREE.MeshLambertMaterial({ vertexColors: true, flatShading: true }));
    this.body.add(this.bodyMesh);
    this.defV = this.restV.map(() => V());

    // face parts
    this.eyeMat = lam('#1d1b26'); this.mouthMat = lam('#5a1f2a'); this.blushMat = lam('#f48aa6');
    const disc = new THREE.CircleGeometry(0.075, 8);
    const arc = new THREE.TorusGeometry(0.06, 0.02, 3, 5, Math.PI);
    this.eyes = [-1, 1].map((sx) => {
      const g = new THREE.Group(); const d = new THREE.Mesh(disc, this.eyeMat); const a = new THREE.Mesh(arc, this.eyeMat);
      g.add(d, a); this.body.add(g); return { g, d, a, sx };
    });
    this.mouth = new THREE.Mesh(new THREE.CircleGeometry(0.04, 6), this.mouthMat); this.body.add(this.mouth);
    this.blush = [-1, 1].map((sx) => { const m = new THREE.Mesh(new THREE.CircleGeometry(0.05, 6), this.blushMat); this.body.add(m); return { m, sx }; });

    // spikes (Burr): anchors from icosphere-detail-1 directions, skipping the face and the floor
    this.spikes = [];
    if (this.variant === 'burr') {
      const s1 = new THREE.IcosahedronGeometry(1, 1).attributes.position; const seen = new Set();
      for (let i = 0; i < s1.count; i++) {
        const d = V().fromBufferAttribute(s1, i).normalize(); const k = d.toArray().map((x) => x.toFixed(3)).join();
        if (seen.has(k)) continue; seen.add(k);
        if (d.y < -0.4) continue;
        if (d.z > 0.45 && d.y < 0.55 && Math.abs(d.x) < 0.75) continue; // keep the face clear
        this.spikes.push({ dir: d, len: 0.13 * (0.8 + Math.random() * 0.4), tip: null, vel: V() });
      }
      const n = this.spikes.length;
      const sg = new THREE.BufferGeometry();
      sg.setAttribute('position', new THREE.BufferAttribute(new Float32Array(n * 4 * 9), 3));
      sg.setAttribute('color', new THREE.BufferAttribute(new Float32Array(n * 4 * 9), 3));
      this.spikeGeo = sg;
      this.spikeMesh = new THREE.Mesh(sg, new THREE.MeshLambertMaterial({ vertexColors: true, flatShading: true }));
      this.spikeMesh.frustumCulled = false;
      this.body.add(this.spikeMesh);
    }

    // sockets
    this.headSock = new THREE.Group(); this.headSock.matrixAutoUpdate = false; this.body.add(this.headSock);
    this.neckSock = new THREE.Group(); this.neckSock.matrixAutoUpdate = false; this.body.add(this.neckSock);
    this.faceY = this.variant === 'mochi' ? 0.3 : 0.16;
    this.neckY = this.variant === 'mochi' ? 0.34 : 0.3; // just under the mouth
    // ellipse of the body cross-section at neck height (mochi is not round in z)
    let rx = 0, rz = 0; for (const p of this.restV) if (Math.abs(p.y - this.neckY) < 0.06) { rx = Math.max(rx, Math.abs(p.x)); rz = Math.max(rz, Math.abs(p.z)); }
    this.neckRX = rx || 0.6; this.neckRZ = rz || 0.6; this.neckR = (this.neckRX + this.neckRZ) / 2;
    let rh = 0, nh = 0; for (const p of this.restV) if (Math.abs(p.y - 0.62) < 0.07) { rh += Math.hypot(p.x, p.z); nh++; }
    this.headR = nh ? rh / nh : 0.5;
    this.width = Math.max(...this.restV.map((p) => Math.abs(p.x))) * 2;

    this.itemSpring = { x: 0, z: 0, vx: 0, vz: 0 };
    this.setSkin(this.skin); this.setItems(this.items);
    this.pose = null; this._deformAll(null);
  }

  // ---------- customisation ----------
  setSkin(s) {
    Object.assign(this.skin, s);
    const body = new THREE.Color(this.skin.body), acc = new THREE.Color(this.skin.accent);
    const col = this.bodyGeo.attributes.color;
    this.faceDir.forEach((d, f) => {
      let use = false;
      switch (this.skin.pattern) {
        case 'belly': use = d.z > 0.3 && d.y < 0.15; break;
        case 'spots': use = Math.sin(7.3 * d.x + 1.1) * Math.sin(6.1 * d.y + 0.3) * Math.sin(7.7 * d.z + 2.2) > 0.3; break;
        case 'stripes': use = d.z < 0.4 && Math.floor((d.y + 1) * 4.5) % 2 === 0; break;
        case 'cap': use = d.y > 0.55; break;
      }
      const c = use ? acc : body;
      for (let k = 0; k < 3; k++) col.setXYZ(f * 3 + k, c.r, c.g, c.b);
    });
    col.needsUpdate = true;
    // eyes flip to light on a dark body so they stay the highest-contrast thing (EVE/Cozmo rule)
    const lum = body.r * 0.2126 + body.g * 0.7152 + body.b * 0.0722;
    this.eyeMat.color.set(lum < 0.08 ? '#f4f1ea' : '#1d1b26');
    this.blush.forEach((b) => (b.m.visible = !!this.skin.blush));
    if (this.spikeGeo) {
      const sc = acc.clone().lerp(body, this.skin.pattern === 'solid' ? 0.6 : 0.15);
      const a = this.spikeGeo.attributes.color;
      for (let i = 0; i < a.count; i++) a.setXYZ(i, sc.r, sc.g, sc.b);
      a.needsUpdate = true;
    }
  }

  setItems(it) {
    Object.assign(this.items, it);
    for (const s of [this.headSock, this.neckSock]) { while (s.children.length) { const c = s.children[0]; c.traverse((o) => o.geometry?.dispose()); s.remove(c); } }
    const acc = this.skin.accent;
    const h = this.items.head, n = this.items.neck;
    const add = (sock, geo, color, p = [0, 0, 0], r = [0, 0, 0], s = [1, 1, 1]) => { const m = new THREE.Mesh(geo, lam(color)); m.position.set(...p); m.rotation.set(...r); m.scale.set(...s); sock.add(m); return m; };
    if (h === 'party hat') {
      add(this.headSock, new THREE.ConeGeometry(0.13, 0.3, 6, 1, true), '#5ab0e8', [0, 0.13, 0]);
      add(this.headSock, new THREE.IcosahedronGeometry(0.045, 0), '#f5df7a', [0, 0.29, 0]);
    } else if (h === 'top hat') {
      add(this.headSock, new THREE.CylinderGeometry(0.2, 0.2, 0.02, 8), '#26222e', [0, 0.0, 0]);
      add(this.headSock, new THREE.CylinderGeometry(0.12, 0.13, 0.2, 8, 1, true), '#26222e', [0, 0.11, 0]);
      add(this.headSock, new THREE.CircleGeometry(0.12, 8), '#26222e', [0, 0.21, 0], [-Math.PI / 2, 0, 0]);
    } else if (h === 'leaf sprout') {
      this.sprout = add(this.headSock, new THREE.CylinderGeometry(0.012, 0.018, 0.14, 3), '#4f7a3a', [0, 0.06, 0]);
      const leaf = new THREE.OctahedronGeometry(0.07, 0);
      add(this.sprout, leaf, '#7cc25a', [0.06, 0.08, 0], [0, 0, -0.6], [1, 0.35, 0.6]);
      add(this.sprout, leaf, '#7cc25a', [-0.05, 0.06, 0], [0, 0, 0.7], [0.8, 0.3, 0.5]);
    } else if (h === 'headphones') {
      const R = this.headR * 1.04;
      // elliptical band: unit half-torus scaled to the head's width and the height above the cups
      add(this.headSock, new THREE.TorusGeometry(1, 0.022, 3, 5, Math.PI), '#26222e', [0, -0.38, 0], [0, 0, 0], [R, 0.43, 1]);
      for (const sx of [-1, 1]) add(this.headSock, new THREE.CylinderGeometry(0.09, 0.09, 0.06, 4), '#f39a6b', [sx * R, -0.38, 0], [0, 0, Math.PI / 2]);
    }
    const ex = this.neckRX * 1.04, ez = this.neckRZ * 1.04;
    if (n === 'scarf') {
      add(this.neckSock, new THREE.TorusGeometry(1, 0.05, 3, 8), '#e2574c', [0, 0, 0], [Math.PI / 2, 0, 0], [ex, ez, 1]);
      add(this.neckSock, new THREE.BoxGeometry(0.09, 0.2, 0.03), '#e2574c', [0.3 * ex, -0.1, ez * 0.93], [0.1, 0.3, 0.12]);
    } else if (n === 'bow tie') {
      const g = new THREE.ConeGeometry(0.07, 0.1, 3, 1);
      add(this.neckSock, g, '#5ab0e8', [-0.055, 0, ez * 0.97], [0, 0, Math.PI / 2]);
      add(this.neckSock, g, '#5ab0e8', [0.055, 0, ez * 0.97], [0, 0, -Math.PI / 2]);
      add(this.neckSock, new THREE.IcosahedronGeometry(0.025, 0), '#3e86bd', [0, 0, ez * 0.99]);
    } else if (n === 'bell collar') {
      add(this.neckSock, new THREE.TorusGeometry(1, 0.025, 3, 8), '#e2574c', [0, 0, 0], [Math.PI / 2, 0, 0], [ex * 0.99, ez * 0.99, 1]);
      add(this.neckSock, new THREE.ConeGeometry(0.05, 0.07, 5), '#f5df7a', [0, -0.05, ez * 1.0]);
    }
    if (h !== 'leaf sprout') this.sprout = null;
  }

  tris() {
    let n = 0;
    this.root.traverse((o) => { if (o.isMesh && o.visible && o.parent.visible) n += (o.geometry.index ? o.geometry.index.count : o.geometry.attributes.position.count) / 3; });
    return Math.round(n);
  }

  // ---------- per-frame deformation ----------
  _deformFn(pose) {
    const P = pose || { squash: 0, spine: { x: 0, y: 0, z: 0 }, head: { x: 0, y: 0, z: 0 } };
    const sy = 1 + P.squash, sx = 1 / Math.sqrt(sy);
    const pivot = new THREE.Vector3(0, 0.38, 0);
    const qh = new THREE.Quaternion().setFromEuler(new THREE.Euler(P.head.x, P.head.y, P.head.z, 'YXZ'));
    const qi = new THREE.Quaternion();
    const qtmp = new THREE.Quaternion(), e = new THREE.Euler();
    const fn = (rest, out) => {
      out.copy(rest);
      const w = smooth(0.28, 0.85, rest.y);
      if (w > 0) { qtmp.slerpQuaternions(qi, qh, w); out.sub(pivot).applyQuaternion(qtmp).add(pivot); }
      const ly = rest.y; // progressive lean (bend) — more at the top
      e.set(P.spine.x * ly, 0, P.spine.z * ly); qtmp.setFromEuler(e); out.applyQuaternion(qtmp);
      out.x *= sx; out.z *= sx; out.y *= sy;
      return out;
    };
    fn.sockM = (restY) => { // rigid matrix for a socket at height restY (head turn at w(restY), lean, squash)
      const w = smooth(0.28, 0.85, restY);
      const m = new THREE.Matrix4().makeScale(sx, sy, sx);
      m.multiply(new THREE.Matrix4().makeRotationFromEuler(new THREE.Euler(P.spine.x * restY, 0, P.spine.z * restY)));
      m.multiply(new THREE.Matrix4().makeTranslation(pivot.x, pivot.y, pivot.z));
      m.multiply(new THREE.Matrix4().makeRotationFromQuaternion(qtmp.clone().slerpQuaternions(qi, qh, w)));
      m.multiply(new THREE.Matrix4().makeTranslation(-pivot.x, -pivot.y + restY, -pivot.z));
      return m;
    };
    return fn;
  }

  _surf(dirLike) { // rest surface point + outward normal for a direction on the unit sphere
    const d = dirLike.clone().normalize();
    const p = this._shape(d); const p2 = this._shape(d.clone().multiplyScalar(1.02));
    return { p, n: p2.sub(p).normalize() };
  }

  _place(obj, fn, dir, lift = 0.012) {
    const { p, n } = this._surf(dir);
    const a = fn(p, V()); const b = fn(p.clone().addScaledVector(n, 0.05), V());
    const nn = b.sub(a).normalize();
    obj.position.copy(a).addScaledVector(nn, lift);
    obj.lookAt(obj.position.clone().add(nn).applyMatrix4(this.body.matrixWorld));
    // lookAt on a child uses world space; body has identity local transform so this is fine
    return nn;
  }

  _deformAll(pose, dt = 0) {
    const fn = this._deformFn(pose);
    for (let i = 0; i < this.restV.length; i++) fn(this.restV[i], this.defV[i]);
    const a = this.bodyGeo.attributes.position;
    for (let i = 0; i < this.vmap.length; i++) { const v = this.defV[this.vmap[i]]; a.setXYZ(i, v.x, v.y, v.z); }
    a.needsUpdate = true; this.bodyGeo.computeBoundingSphere();

    const P = pose || { look: { x: 0, y: 0 }, lid: 0, eye: 'normal', mouth: 0 };
    this.root.updateMatrixWorld(true);
    // eyes (pupil-less, Cozmo-style); look slides them over the face
    const lx = P.look.x * 0.12, ly = P.look.y * 0.1;
    for (const e of this.eyes) {
      this._place(e.g, fn, new THREE.Vector3(e.sx * 0.3 + lx, this.faceY + ly, 0.95));
      const shape = P.eye;
      e.d.visible = shape !== 'happy'; e.a.visible = shape === 'happy';
      let sxs = 1, sys = 1;
      if (shape === 'wide') { sxs = 1.15; sys = 1.25; }
      if (shape === 'squint') sys = 0.3;
      const open = Math.max(0.1, 1 - P.lid);
      e.d.scale.set(sxs, sys * open, 1);
      e.d.position.y = (1 - open) * 0.075 * sys * 0.6; // upper lid closes downward
      e.a.scale.setScalar(1);
    }
    this._place(this.mouth, fn, new THREE.Vector3(0, this.faceY - 0.22 + ly * 0.3, 0.97));
    this.mouth.scale.set(1 + P.mouth * 0.2, 0.18 + P.mouth * 0.95, 1);
    for (const b of this.blush) this._place(b.m, fn, new THREE.Vector3(b.sx * 0.5, this.faceY - 0.15, 0.84), 0.006);

    // spikes with spring tips (follow-through)
    if (this.spikeGeo) {
      const pa = this.spikeGeo.attributes.position; let o = 0;
      const up = new THREE.Vector3(0, 1, 0), t1 = V(), t2 = V();
      for (const s of this.spikes) {
        const { p, n } = this._surf(s.dir);
        const A = fn(p, V()); const N = fn(p.clone().addScaledVector(n, 0.05), V()).sub(A).normalize();
        const target = A.clone().addScaledVector(N, s.len);
        if (!s.tip || dt === 0) { s.tip = target.clone(); s.vel.set(0, 0, 0); }
        else { const acc = target.clone().sub(s.tip).multiplyScalar(260).addScaledVector(s.vel, -11); s.vel.addScaledVector(acc, dt); s.tip.addScaledVector(s.vel, dt); }
        t1.crossVectors(N, Math.abs(N.y) > 0.9 ? new THREE.Vector3(1, 0, 0) : up).normalize(); t2.crossVectors(N, t1);
        const r = 0.05, base = A.clone().addScaledVector(N, -0.02);
        const q = [t1.clone().multiplyScalar(r), t2.clone().multiplyScalar(r), t1.clone().multiplyScalar(-r), t2.clone().multiplyScalar(-r)].map((d) => base.clone().add(d));
        for (let k = 0; k < 4; k++) { const b0 = q[k], b1 = q[(k + 1) % 4]; pa.setXYZ(o++, b0.x, b0.y, b0.z); pa.setXYZ(o++, b1.x, b1.y, b1.z); pa.setXYZ(o++, s.tip.x, s.tip.y, s.tip.z); }
      }
      pa.needsUpdate = true;
    }

    // sockets (+ springy follow-through on the head items)
    const top = this._shape(new THREE.Vector3(0, 1, 0)).y;
    const sp = this.itemSpring, hx = pose ? pose.head.x + pose.spine.x : 0, hz = pose ? pose.head.z + pose.spine.z : 0;
    if (dt > 0) { sp.vx += (120 * (hx - sp.x) - 9 * sp.vx) * dt; sp.vz += (120 * (hz - sp.z) - 9 * sp.vz) * dt; sp.x += sp.vx * dt; sp.z += sp.vz * dt; }
    const lagX = (sp.x - hx) * 0.8, lagZ = (sp.z - hz) * 0.8;
    const hm = fn.sockM(top).multiply(new THREE.Matrix4().makeRotationFromEuler(new THREE.Euler(lagX, 0, lagZ)));
    this.headSock.matrix.copy(hm); this.headSock.matrixWorldNeedsUpdate = true;
    this.neckSock.matrix.copy(fn.sockM(this.neckY)); this.neckSock.matrixWorldNeedsUpdate = true;
    if (this.sprout) this.sprout.rotation.set(lagX * 2.5, 0, lagZ * 2.5);
  }

  update(dt, pose) {
    this.root.position.set(pose.pos.x, pose.pos.y, pose.pos.z);
    this.root.rotation.set(pose.rot.x, pose.rot.y, pose.rot.z);
    this._deformAll(pose, dt);
  }
  topY() { return 1.15; }
  dispose() { this.root.removeFromParent(); this.root.traverse((o) => o.geometry?.dispose()); }
}
