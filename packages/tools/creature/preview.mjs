// SPDX-License-Identifier: MIT
// molty-creature preview (docs/CREATURE.md): a page on this computer that shows a Molty moving as the
// device draws it: the built package through the device's reference player and dots
// (device/creatures/js/clip-player.js, clip-dots.js). Ask for a mood or a gesture, or play one action.
// Local only: it listens on 127.0.0.1.
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { check } from './build.mjs';

const JS = new URL('../../../device/creatures/js/', import.meta.url), SERVED = ['clip-player.js', 'clip-dots.js'];
const PAGE = `<!doctype html><meta charset="utf-8"><title>molty-creature preview</title>
<style>
  body { margin: 0; padding: 16px; background: #101114; color: #c8cce0; font: 14px/1.4 system-ui, sans-serif; display: flex; gap: 20px; flex-wrap: wrap; }
  canvas { width: 466px; max-width: 100%; aspect-ratio: 1; border-radius: 50%; background: #000; }
  aside { flex: 1; min-width: 300px; max-width: 620px; }
  h1 { font-size: 18px; margin: 0 0 4px; } h2 { font-size: 13px; color: #7c8198; margin: 14px 0 6px; font-weight: 600; }
  button { background: #1b1d24; color: inherit; border: 1px solid #3a3e4e; border-radius: 999px; padding: 4px 11px; margin: 0 4px 6px 0; font: inherit; cursor: pointer; }
  button[aria-pressed="true"] { background: #7566c4; border-color: #7566c4; color: #fff; }
  #now { color: #fff; min-height: 1.4em; } #problems { color: #ff7a55; white-space: pre-wrap; } .dim { color: #7c8198; }
</style>
<canvas id="glass" width="824" height="824"></canvas>
<aside>
  <h1 id="name"></h1><div id="look" class="dim"></div><div id="problems"></div>
  <h2>MOODS</h2><div id="moods"></div>
  <h2>GESTURES</h2><div id="gestures"></div>
  <h2>ACTIONS, ONE AT A TIME</h2><div id="actions"></div>
  <p id="now"></p><p id="say" class="dim"></p>
  <button id="reload">reload the Molty</button>
</aside>
<script type="module">
import { clipPlayer } from '/js/clip-player.js';
import { clipDots } from '/js/clip-dots.js';
const $ = (id) => document.getElementById(id), q = new URLSearchParams(location.search), W = 52, H = 32, DW = 104, DH = 128, D = 824, g = $('glass').getContext('2d');
const BIT = (k) => (k < 4 ? ((k & 3) < 3 ? (k & 3) : 6) : ((k & 3) < 3 ? 3 + (k & 3) : 7));   // a dot's bit in its braille cell
let molty, player, rend, frame = 0, timer;
function draw() {
  rend.draw(player.world, player.cam, frame);
  const c = rend.cells(), px = D / DW, py = D / DH;
  g.fillStyle = '#000'; g.fillRect(0, 0, D, D);
  g.save(); g.beginPath(); g.arc(D / 2, D / 2, D / 2 - 2, 0, 6.2832); g.clip();
  for (let k = 0; k < W * H; k++) {
    if (!c.dots[k]) continue;
    g.fillStyle = '#' + c.col[k].toString(16).padStart(6, '0');
    for (let b = 0; b < 8; b++) if (c.dots[k] >> BIT(b) & 1) { g.beginPath(); g.arc((2 * (k % W) + (b >> 2) + 0.5) * px, (4 * ((k / W) | 0) + (b & 3) + 0.5) * py, 3.1, 0, 6.2832); g.fill(); }
  }
  g.restore();
  $('now').textContent = player.clip + '  ' + player.t.toFixed(2) + ' s';
}
function row(id, names, press) {
  $(id).innerHTML = '';
  for (const n of names) { const b = document.createElement('button'); b.textContent = n.replace(/_/g, ' '); b.onclick = () => { for (const x of document.querySelectorAll('button[aria-pressed]')) x.removeAttribute('aria-pressed'); b.setAttribute('aria-pressed', 'true'); press(n); }; $(id).appendChild(b); }
}
async function load() {
  clearInterval(timer);
  molty = await (await fetch('/molty')).json();
  $('problems').textContent = molty.problems.join('\\n');
  if (!molty.rig) return;
  $('name').textContent = molty.name; $('look').textContent = molty.look;
  const fresh = () => { player = clipPlayer(molty.rig); frame = 0; };
  rend = clipDots(molty.rig, W, H); fresh();
  row('moods', Object.keys(molty.moods), (n) => { player.mood(n); $('say').textContent = molty.moods[n]; });
  row('gestures', Object.keys(molty.gestures), (n) => { player.act(n); $('say').textContent = molty.gestures[n]; });
  row('actions', Object.keys(molty.rig.clips), (n) => { fresh(); player.play(n); const c = molty.rig.clips[n]; $('say').textContent = (c.loop ? 'Loops. ' : 'Plays once. ') + 'From ' + c.from + ' to ' + c.to + ', ' + c.dur + ' s.'; });
  if (q.get('action')) { player.play(q.get('action')); for (let i = 0; i < Math.round(+(q.get('t') ?? 0) * 24); i++) { player.update(1 / 24); frame++; } player.update(0); return draw(); }   // one still, for a picture
  timer = setInterval(() => { player.update(1 / 24); frame++; draw(); }, 1000 / 24);
}
$('reload').onclick = load;
load();
</script>`;

/** Serves the preview of the Molty at `dir` on 127.0.0.1:port; returns the server. */
export function preview(dir, port) {
  const molty = () => {
    const r = check(dir), c = r.pkg?.creature;
    return { problems: r.errors.map((e) => `${e.at}  ${e.message}`), ...(c ? { name: r.pkg.name, look: c.about.look, moods: c.about.moods, gestures: c.about.gestures, rig: c.rig } : {}) };
  };
  const server = createServer((req, res) => {
    const path = new URL(req.url, 'http://127.0.0.1').pathname, send = (type, body) => res.writeHead(200, { 'content-type': type, 'cache-control': 'no-store' }).end(body);
    try {
      if (path === '/') return void send('text/html; charset=utf-8', PAGE);
      if (path === '/molty') return void send('application/json', JSON.stringify(molty()));
      if (path.startsWith('/js/') && SERVED.includes(path.slice(4))) return void send('text/javascript', readFileSync(new URL(path.slice(4), JS)));
      res.writeHead(404).end();
    } catch (e) { res.writeHead(500).end(e.message); }
  });
  server.on('error', (e) => { console.error(`molty-creature: ${e.code === 'EADDRINUSE' ? `port ${port} is taken: pass another with --port` : e.message}`); process.exit(1); });
  server.listen(port, '127.0.0.1', () => console.log(`molty-creature preview: http://127.0.0.1:${server.address().port}/  (Ctrl-C to stop)`));
  return server;
}
