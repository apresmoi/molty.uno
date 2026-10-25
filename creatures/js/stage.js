// One viewport: scene + camera + creature + mood engine + (optional) device preview.
//
// Device preview mirrors glyphcss-c: orthographic camera, one white directional light +
// 0.4 ambient, flat Lambert, one colour per face, no AA, a ≤96×96 cell grid rect-filled
// onto a 412×412 round panel. The grid is rendered for real (render target at grid×grid,
// read back, nearest-neighbour upscaled), so coverage/cell counts below are measured.

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { MoodEngine } from './mood.js';
import { ProcCreature } from './proc.js';
import { CuteCreature } from './cute/kit.js';

export const BUDGET = { flushMs: 10, msPerPoly: 0.016, msPerCell: 0.0022, frameMs: 30, targetTris: 600 };
const PANEL = 412;
const DEV_LIGHT = { key: 1.0, amb: 0.4 }; // glyphcss-c defaults (key_intensity, amb_intensity)
const RAMP = ' .:-=+*#%@';
// linear → sRGB 8-bit, 4096-entry LUT
const LUT = new Uint8Array(4096);
for (let i = 0; i < 4096; i++) { const c = i / 4095; LUT[i] = Math.round(255 * (c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055)); }
const enc = (v) => LUT[v <= 0 ? 0 : v >= 1 ? 4095 : (v * 4095) | 0];

export class Stage {
  constructor(host, label) {
    this.label = label;
    this.el = document.createElement('div'); this.el.className = 'stage';
    this.el.innerHTML = `
      <div class="stage-head"><span class="slot">${label}</span><span class="sname">—</span><span class="stris"></span></div>
      <div class="viewport"><canvas class="gl"></canvas><canvas class="dev" width="${PANEL}" height="${PANEL}"></canvas><div class="loading">loading…</div></div>
      <div class="stage-foot"><span class="srig"></span><span class="sbudget"></span></div>`;
    host.appendChild(this.el);
    this.vp = this.el.querySelector('.viewport');
    this.glCanvas = this.el.querySelector('canvas.gl');
    this.devCanvas = this.el.querySelector('canvas.dev');
    this.dev2d = this.devCanvas.getContext('2d');

    this.renderer = new THREE.WebGLRenderer({ canvas: this.glCanvas, antialias: true, alpha: true });
    this.renderer.setPixelRatio(Math.min(2, window.devicePixelRatio || 1));
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;

    this.scene = new THREE.Scene();
    this.camera = new THREE.PerspectiveCamera(35, 1, 0.01, 100);
    this.camera.position.set(1.2, 1.1, 3.0);
    this.ortho = new THREE.OrthographicCamera(-1, 1, 1, -1, 0.01, 100);
    this.controls = new OrbitControls(this.camera, this.glCanvas);
    this.controls.target.set(0, 0.5, 0); this.controls.enableDamping = true;
    this.devControls = new OrbitControls(this.camera, this.devCanvas);
    this.devControls.target = this.controls.target; this.devControls.enableDamping = true;

    // normal-view lights vs device lights (glyphcss-c: 1 white directional + 0.4 ambient)
    this.viewLights = new THREE.Group();
    this.viewLights.add(new THREE.HemisphereLight(0xfff4e8, 0x40384a, 1.6));
    const key = new THREE.DirectionalLight(0xffffff, 1.6); key.position.set(2, 4, 3); this.viewLights.add(key);
    // Device shading is done the way glyphcss-c's gc_shade_faces does it: per face,
    // sRGB colour × min(1, amb + key·max(0, n·L)), in sRGB space, clamped. three's lights
    // carry a 1/π Lambert factor and shade in linear space, which made the old preview
    // (key 1.0 + amb 0.4 as three intensities) ~30 % dim and muddy. So the device frame is
    // rendered twice: (A) ambient π only → each face's unlit linear colour, (B) a white
    // override material lit by key·π + amb·π → the light intensity, then combined on readback.
    this.devLights = new THREE.Group();
    this.devKey = new THREE.DirectionalLight(0xffffff, DEV_LIGHT.key * Math.PI); this.devKey.position.set(0.5, 1, 1); this.devLights.add(this.devKey);
    this.devAmb = new THREE.AmbientLight(0xffffff, DEV_LIGHT.amb * Math.PI); this.devLights.add(this.devAmb);
    this.devWhite = new THREE.MeshLambertMaterial({ color: 0xffffff, flatShading: true });
    this.scene.add(this.viewLights, this.devLights);
    this.ground = new THREE.Mesh(new THREE.CircleGeometry(1.2, 48), new THREE.MeshLambertMaterial({ color: 0xe9e2d8 }));
    this.ground.rotation.x = -Math.PI / 2; this.ground.position.y = -0.002; this.scene.add(this.ground);

    this.emote = this._makeEmote(); this.scene.add(this.emote.sprite);

    this.engine = new MoodEngine();
    this.creature = null;
    this.device = { on: false, decimate: false, target: 600, method: 'lattice', glyph: true, grid: 72, cells: 'rect' };
    this.stats = { tris: 0, cells: 0, coverage: 0 };
    this.gaze = true; this._pokeHandlers = [];

    // pointer → attention (gaze-follow) and click → poke
    const onMove = (ev) => { const r = this.vp.getBoundingClientRect(); this.engine.attention.x = ((ev.clientX - r.left) / r.width) * 2 - 1; this.engine.attention.y = -(((ev.clientY - r.top) / r.height) * 2 - 1); this.engine.attention.active = this.gaze; };
    this.vp.addEventListener('pointermove', onMove);
    this.vp.addEventListener('pointerleave', () => { this.engine.attention.active = false; });
    let down = null;
    this.vp.addEventListener('pointerdown', (e) => { down = [e.clientX, e.clientY]; });
    this.vp.addEventListener('pointerup', (e) => { if (down && Math.hypot(e.clientX - down[0], e.clientY - down[1]) < 4) this._pokeHandlers.forEach((f) => f(this)); down = null; });

    this.ro = new ResizeObserver(() => this._resize()); this.ro.observe(this.vp);
    this._resize();
  }

  onPoke(f) { this._pokeHandlers.push(f); }

  async setCreature(entry) {
    const token = (this._token = {});
    this.el.querySelector('.loading').style.display = 'block';
    const c = entry.kind === 'proc' ? new ProcCreature(entry) : new CuteCreature(entry);
    try { await c.load(); } catch (err) { this.el.querySelector('.loading').textContent = `failed: ${err.message}`; throw err; }
    if (token !== this._token) { c.dispose(); return null; }
    if (this.creature) this.creature.dispose();
    this.creature = c; this.entry = entry;
    this.scene.add(c.root);
    this.el.querySelector('.loading').style.display = 'none';
    this.el.querySelector('.sname').textContent = entry.name;
    this.el.querySelector('.srig').textContent = entry.kind === 'proc' ? 'deform runtime: squash · lean · head-turn · sockets' : c.rig;
    this.vp.dataset.creature = entry.id;
    this.frame();
    this.applyDevice();
    return c;
  }

  frame() {
    if (this.creature?.envelope) return this._frameEnvelope(this.creature.envelope);
    const w = this.creature?.width || 1;
    const h = 1;
    this.fitHalf = Math.max(h * 0.6, w * 0.46) + 0.06; // long animals are seen end-on-ish, so width counts less
    this.controls.target.set(0, 0.5, 0);
    const dist = this.fitHalf / Math.tan(THREE.MathUtils.degToRad(this.camera.fov / 2)) * 1.05;
    const dir = new THREE.Vector3(0.38, 0.28, 1).normalize();
    this.camera.position.copy(this.controls.target).addScaledVector(dir, dist);
    this.baseDist = dist;
    this.controls.update();
  }

  // Fit the creature's MOTION envelope (rest box + hop/float headroom, see kit.js) into
  // the view, so happy hops and floaters stay on screen. A standard box is unioned in so
  // creatures keep a comparable scale (a small creature is not blown up to fill the frame).
  // Perspective distance and the device's ortho half-size are both solved from the 8
  // corners; the target is re-centred so the envelope sits in the middle of the frame.
  _frameEnvelope(env) {
    const STD = { min: [-0.36, 0, -0.36], max: [0.36, 1.26, 0.36] };
    const lo = [Math.min(env.min.x, STD.min[0]), 0, Math.min(env.min.z, STD.min[2])];
    const hi = [Math.max(env.max.x, STD.max[0]), Math.max(env.max.y, STD.max[1]), Math.max(env.max.z, STD.max[2])];
    // the creature turns (yaw sway, attention): make x/z symmetric about the axis
    const rx = Math.max(-lo[0], hi[0]), rz = Math.max(-lo[2], hi[2]);
    // sample an elliptic hull, not the box corners (creatures are round; box corners seen
    // diagonally overstate the silhouette by ~30 %): full rings from the floor to the top of
    // the rest pose, a narrower ring at the top of the hop (heads are domes)
    const restTop = hi[1] - (env.rise || 0), corners = [];
    for (let i = 0; i < 16; i++) {
      const a = (i / 16) * Math.PI * 2, cx = Math.cos(a) * rx, cz = Math.sin(a) * rz;
      for (const [y, r] of [[lo[1], 0.75], [restTop * 0.55, 1], [hi[1], 0.55]]) corners.push(new THREE.Vector3(cx * r, y, cz * r));
    }
    const dir = new THREE.Vector3(0.38, 0.28, 1).normalize();
    const M = 0.96; // keep the envelope inside 96 % of the half-frame
    const tanH = Math.tan(THREE.MathUtils.degToRad(this.camera.fov / 2));
    const asp = Math.min(1, this.camera.aspect || 1);
    const target = new THREE.Vector3(0, (lo[1] + hi[1]) / 2, 0);
    const cam = this.camera, v = new THREE.Vector3();
    let dist = 3;
    for (let it = 0; it < 4; it++) {
      cam.position.copy(target).addScaledVector(dir, 10); cam.lookAt(target); cam.updateMatrixWorld();
      const inv = cam.matrixWorldInverse;
      const vt = target.clone().applyMatrix4(inv);
      dist = 0; let top = -1e9, bot = 1e9;
      for (const c of corners) {
        v.copy(c).applyMatrix4(inv).sub(vt); // view-space offset from the target (camera looks down -z)
        dist = Math.max(dist, v.z + Math.abs(v.y) / (M * tanH), v.z + Math.abs(v.x) / (M * tanH * asp));
      }
      for (const c of corners) { v.copy(c).applyMatrix4(inv).sub(vt); const ndc = v.y / ((dist - v.z) * tanH); top = Math.max(top, ndc); bot = Math.min(bot, ndc); }
      target.y += ((top + bot) / 2) * dist * tanH * 0.9; // re-centre vertically
    }
    // ortho half-size for the device panel (same direction, centred on the same target)
    cam.position.copy(target).addScaledVector(dir, 10); cam.lookAt(target); cam.updateMatrixWorld();
    const vt = target.clone().applyMatrix4(cam.matrixWorldInverse); let half = 0;
    for (const c of corners) { v.copy(c).applyMatrix4(cam.matrixWorldInverse).sub(vt); half = Math.max(half, Math.abs(v.x), Math.abs(v.y)); }
    this.fitHalf = half / M;
    this.controls.target.copy(target);
    cam.position.copy(target).addScaledVector(dir, dist);
    this.baseDist = dist;
    this.controls.update();
  }

  applyDevice() {
    if (!this.creature) return;
    const d = this.device;
    this.stats.tris = this.creature.setDevice({ flat: d.on, decimate: d.decimate, target: d.target, method: d.method });
    this.viewLights.visible = !d.on; this.devLights.visible = d.on; this.ground.visible = !d.on;
    this.glCanvas.style.display = d.on ? 'none' : 'block';
    this.devCanvas.style.display = d.on ? 'block' : 'none';
    this.el.classList.toggle('device', d.on);
    this._resize();
  }

  _resize() {
    const r = this.vp.getBoundingClientRect();
    if (r.width < 2 || r.height < 2) return;
    this.renderer.setSize(r.width, r.height, false);
    this.camera.aspect = r.width / r.height; this.camera.updateProjectionMatrix();
  }

  _makeEmote() {
    const c = document.createElement('canvas'); c.width = 128; c.height = 64;
    const tex = new THREE.CanvasTexture(c); tex.colorSpace = THREE.SRGBColorSpace;
    const sprite = new THREE.Sprite(new THREE.SpriteMaterial({ map: tex, depthTest: false, transparent: true }));
    sprite.scale.set(0.42, 0.21, 1); sprite.renderOrder = 10;
    return { c, g: c.getContext('2d'), tex, sprite, text: null };
  }
  _setEmote(text, on) {
    const e = this.emote; e.sprite.visible = on && !!text;
    if (text === e.text) return; e.text = text;
    e.g.clearRect(0, 0, 128, 64);
    if (!text) return;
    e.g.font = 'bold 44px ui-monospace, Menlo, monospace'; e.g.textAlign = 'center'; e.g.textBaseline = 'middle';
    e.g.lineWidth = 8; e.g.strokeStyle = '#1d1b26'; e.g.strokeText(text, 64, 34);
    e.g.fillStyle = '#fff6d6'; e.g.fillText(text, 64, 34);
    e.tex.needsUpdate = true;
  }

  update(dt, showEmotes) {
    if (!this.creature) return;
    dt = Math.min(dt, 1 / 30); // a stalled tab must not blow up the springs
    const pose = this.engine.update(dt);
    this.pose = pose;
    this.creature.update(dt, pose);
    this._setEmote(pose.emote, showEmotes);
    this.emote.sprite.position.set(0.32, this.creature.topY() + 0.05 + pose.pos.y, 0);
  }

  render() {
    (this.device.on ? this.devControls : this.controls).update();
    if (!this.device.on) { this.renderer.setRenderTarget(null); this.renderer.render(this.scene, this.camera); return; }
    this._renderDevice();
  }

  _renderDevice() {
    const d = this.device;
    const R = d.glyph ? d.grid : PANEL;
    if (!this.rt || this.rt.width !== R) {
      this.rt?.dispose();
      // float target: three writes LINEAR colour into render targets; we encode to sRGB on readback
      this.rt = new THREE.WebGLRenderTarget(R, R, { samples: 0, type: THREE.FloatType });
      this.buf = new Float32Array(R * R * 4); this.lbuf = new Float32Array(R * R * 4);
      this.small = document.createElement('canvas'); this.small.width = R; this.small.height = R;
      this.smallCtx = this.small.getContext('2d'); this.img = this.smallCtx.createImageData(R, R);
    }
    // orthographic camera that follows the orbit camera's direction + zoom
    const t = this.controls.target, dir = this.camera.position.clone().sub(t);
    const zoom = dir.length() / this.baseDist; dir.normalize();
    this.ortho.position.copy(t).addScaledVector(dir, 10); this.ortho.quaternion.copy(this.camera.quaternion);
    const h = this.fitHalf * zoom;
    this.ortho.left = -h; this.ortho.right = h; this.ortho.top = h; this.ortho.bottom = -h; this.ortho.updateProjectionMatrix();

    this.renderer.setRenderTarget(this.rt);
    this.renderer.setClearColor(0x000000, 0);
    // (A) unlit face colours: ambient π alone returns the linear albedo
    this.devKey.visible = false; this.devAmb.intensity = Math.PI;
    this.renderer.clear(); this.renderer.render(this.scene, this.ortho);
    this.renderer.readRenderTargetPixels(this.rt, 0, 0, R, R, this.buf);
    // (B) light intensity on a white flat material (emote sprite hidden: it stays unlit)
    this.devKey.visible = true; this.devAmb.intensity = DEV_LIGHT.amb * Math.PI;
    const sv = this.emote.sprite.visible; this.emote.sprite.visible = false;
    this.scene.overrideMaterial = this.devWhite;
    this.renderer.clear(); this.renderer.render(this.scene, this.ortho);
    this.scene.overrideMaterial = null; this.emote.sprite.visible = sv;
    this.renderer.readRenderTargetPixels(this.rt, 0, 0, R, R, this.lbuf);
    this.renderer.setRenderTarget(null);

    // flip rows, mask to the round panel, count covered cells
    const px = this.img.data, rad = R / 2; let cov = 0, inside = 0;
    for (let y = 0; y < R; y++) {
      const src = (R - 1 - y) * R * 4, dst = y * R * 4;
      for (let x = 0; x < R; x++) {
        const dx = x + 0.5 - rad, dy = y + 0.5 - rad; const ins = dx * dx + dy * dy <= rad * rad;
        const s = src + x * 4, o = dst + x * 4;
        if (ins) inside++;
        if (ins && this.buf[s + 3] > 0) {
          const k = this.lbuf[s + 3] > 0 ? Math.min(1, this.lbuf[s]) : 1; // intensity, clamped to 1 like gc_shade_faces
          px[o] = enc(this.buf[s]) * k; px[o + 1] = enc(this.buf[s + 1]) * k; px[o + 2] = enc(this.buf[s + 2]) * k; px[o + 3] = 255; cov++;
        }
        else { px[o] = px[o + 1] = px[o + 2] = 0; px[o + 3] = 255; }
      }
    }
    this.smallCtx.putImageData(this.img, 0, 0);
    const g = this.dev2d;
    g.save(); g.fillStyle = '#000'; g.fillRect(0, 0, PANEL, PANEL);
    g.beginPath(); g.arc(PANEL / 2, PANEL / 2, PANEL / 2, 0, Math.PI * 2); g.clip();
    if (d.glyph && d.cells === 'glyph') {
      const cs = PANEL / R; g.font = `bold ${Math.ceil(cs * 1.15)}px ui-monospace, Menlo, monospace`; g.textAlign = 'center'; g.textBaseline = 'middle';
      for (let y = 0; y < R; y++) for (let x = 0; x < R; x++) {
        const o = (y * R + x) * 4; const r = px[o], gg = px[o + 1], b = px[o + 2];
        if (!(r | gg | b)) continue;
        const lum = (0.2126 * r + 0.7152 * gg + 0.0722 * b) / 255;
        g.fillStyle = `rgb(${r >> 2},${gg >> 2},${b >> 2})`; g.fillRect(x * cs, y * cs, cs + 0.5, cs + 0.5);
        g.fillStyle = `rgb(${r},${gg},${b})`; g.fillText(RAMP[Math.min(RAMP.length - 1, 1 + Math.floor(lum * (RAMP.length - 1)))], (x + 0.5) * cs, (y + 0.5) * cs);
      }
    } else {
      g.imageSmoothingEnabled = false; g.drawImage(this.small, 0, 0, PANEL, PANEL);
    }
    g.restore();
    // cells on the device grid (coverage fraction × grid cells inside the circle when not in glyph mode)
    const frac = inside ? cov / inside : 0;
    const gridInside = Math.round(Math.PI / 4 * d.grid * d.grid);
    this.stats.coverage = frac; this.stats.cells = d.glyph ? cov : Math.round(frac * gridInside);
  }

  budgetText() {
    const s = this.stats, b = BUDGET;
    const geo = s.tris * b.msPerPoly, cells = s.cells * b.msPerCell, total = b.flushMs + geo + cells;
    return { total, geo, cells, fps: 1000 / total, ok: total <= b.frameMs, over: s.tris > b.targetTris };
  }
  dispose() { this.ro.disconnect(); this.creature?.dispose(); this.renderer.dispose(); this.el.remove(); }
}
