// SPDX-License-Identifier: MIT
// molty-display preview (docs/DISPLAY.md): a page on this computer that shows a skill's screens as
// Molty would draw them (play.mjs, the reference player, on the built package). Pick a screen, edit
// what the agent sends, scrub time, click a tap zone. Local only: it listens on 127.0.0.1.
import { createServer } from 'node:http';
import { ParamError, bind, opens } from '../format.mjs';
import { frame, hd, png, spotAt } from '../play.mjs';
import { check, exampleParams } from './build.mjs';

const PAGE = `<!doctype html><meta charset="utf-8"><title>molty-display preview</title>
<style>
  body { margin: 0; padding: 16px; background: #101114; color: #c8cce0; font: 14px/1.4 system-ui, sans-serif; display: flex; gap: 20px; flex-wrap: wrap; }
  #glass { width: 466px; max-width: 100%; aspect-ratio: 1; border-radius: 50%; background: #000; cursor: pointer; }
  aside { flex: 1; min-width: 300px; max-width: 560px; display: flex; flex-direction: column; gap: 10px; }
  select, textarea, button { background: #1b1d24; color: inherit; border: 1px solid #3a3e4e; border-radius: 6px; padding: 6px; font: inherit; }
  textarea { height: 260px; font: 12px ui-monospace, monospace; }
  #reading { min-height: 3em; color: #fff; } #problems { color: #ff7a55; white-space: pre-wrap; } .dim { color: #7c8198; }
</style>
<div><img id="glass" alt="the screen"><p id="reading"></p></div>
<aside>
  <label>screen <select id="screen"></select> <button id="reload">reload the skill</button></label>
  <div id="about" class="dim"></div>
  <label>time <input id="t" type="range" min="0" step="0.05" value="1"> <span id="tv"></span> <button id="run">play</button>
    <label><input id="hd" type="checkbox"> high definition</label></label>
  <label class="dim">inputs, as the agent sends them (params)</label><textarea id="params" spellcheck="false"></textarea>
  <div id="problems"></div>
  <div class="dim">Click a tap zone on the screen to see what it reads and lights. Click elsewhere to let go.</div>
</aside>
<script>
const $ = (id) => document.getElementById(id), q = new URLSearchParams(location.search);
let skill, tap = '', timer, back = null, shown = 0;
async function load() {
  skill = await (await fetch('/skill')).json();
  $('screen').innerHTML = skill.screens.map((s) => '<option>' + s.name + '</option>').join('');
  if (q.get('screen')) $('screen').value = q.get('screen');
  pick();
}
function pick() {
  const s = skill.screens.find((x) => x.name === $('screen').value);
  if (!s) return void ($('problems').textContent = skill.problems.join('\\n'));
  $('about').textContent = s.about + (s.duration ? ' Lasts ' + s.duration + ' s.' : ' Held.');
  $('params').value = JSON.stringify(s.params ?? {}, null, 1);
  $('t').max = s.duration || 10; $('t').value = q.get('t') ?? Math.min(1, $('t').max / 2); $('hd').checked = q.has('hd');
  tap = ''; back = null; draw();
}
async function draw() {
  // the inputs go up in the request's body (a chart's are too long for an address); the picture is then asked for
  const scene = await (await fetch('/scene', { method: 'POST', body: JSON.stringify({ screen: $('screen').value, params: $('params').value }) })).json();
  $('tv').textContent = (+$('t').value).toFixed(2) + ' s';
  $('problems').textContent = [...skill.problems, scene.error ?? ''].filter(Boolean).join('\\n');
  if (!scene.error) $('glass').src = '/frame.png?t=' + $('t').value + '&tap=' + encodeURIComponent(tap) + ($('hd').checked ? '&hd=1' : '') + '&n=' + ++shown;
}
$('glass').onclick = async (e) => {
  const b = $('glass').getBoundingClientRect(), x = ((e.clientX - b.left) / b.width) * 2 - 1, y = 1 - ((e.clientY - b.top) / b.height) * 2;
  const hit = await (await fetch('/tap?t=' + $('t').value + '&x=' + x + '&y=' + y)).json();
  if (!hit.say && back) { $('screen').value = back.screen; $('params').value = back.params; $('about').textContent = back.about; back = null; }   // a tap beside an opened screen goes back
  tap = hit.id ?? ''; $('reading').textContent = hit.say ? hit.say + (hit.id ? '  (tap event: ' + hit.id + ')' : '') : '';
  if (hit.opens) {   // the zone opens another screen, as Molty does: with what this one was sent
    back = { screen: $('screen').value, params: $('params').value, about: $('about').textContent };
    $('screen').value = hit.opens.layout; $('params').value = JSON.stringify(hit.opens.params, null, 1); tap = '';
    $('about').textContent = 'Opened by the tap on ' + hit.id + (hit.opens.ttl ? ', for ' + hit.opens.ttl + ' s' : '') + '. Click beside it to go back.';
  }
  draw();
};
$('screen').onchange = pick; $('reload').onclick = load; $('params').oninput = $('t').oninput = $('hd').onchange = draw;
$('run').onclick = () => { clearInterval(timer); $('t').value = 0; timer = setInterval(() => { $('t').value = +$('t').value + 0.1; draw(); if (+$('t').value >= +$('t').max) clearInterval(timer); }, 100); };
load();
</script>`;

/** Serves the preview of the skill at `dir` on 127.0.0.1:port; returns the server. */
export function preview(dir, port) {
  let built, screen = '', params = {};   // the screen on show and what it was sent (the page posts them to /scene)
  const skill = () => {
    built = check(dir);
    return { problems: built.errors.map((e) => `${e.at}  ${e.message}`), screens: built.pkg ? Object.entries(built.skill.screens).map(([name, { def }]) => ({ name, about: def.about, duration: built.pkg.layouts[name].dur, params: exampleParams(def) })) : [] };
  };
  const server = createServer(async (req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1'), arg = (k) => url.searchParams.get(k) ?? '', json = (v) => res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify(v));
    try {
      if (url.pathname === '/') return void res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' }).end(PAGE);
      if (url.pathname === '/skill') return void json(skill());
      const pkg = (built ?? (skill(), built)).pkg, t = +arg('t');
      if (url.pathname === '/scene') {
        const sent = JSON.parse(Buffer.concat(await req.toArray()).toString() || '{}');
        [screen, params] = [sent.screen, JSON.parse(sent.params || '{}')];
        return void json({ zones: bind(pkg, screen, params).filter((it) => it.k === 'spot').length });
      }
      if (url.pathname === '/tap') {   // the zone a tap lands on, by the device's own rule, and what it opens
        const hit = spotAt(bind(pkg, screen, params), +arg('x'), +arg('y'), t);
        return void json(hit ? { id: hit.id, say: hit.say, opens: hit.id === undefined ? null : opens(pkg, screen, params, hit.id) } : {});
      }
      if (url.pathname !== '/frame.png') return void res.writeHead(404).end();
      const view = arg('hd') ? hd(pkg, screen, params, t, 466, '', arg('tap')) : null;
      res.writeHead(200, { 'content-type': 'image/png', 'cache-control': 'no-store' }).end(view ? png(view.kept, view.rgb, 466) : png(frame(pkg, screen, params, t, null, arg('tap'))));
    } catch (e) {
      json({ error: e instanceof ParamError ? `the device would refuse these inputs: ${e.message}` : e.message });
    }
  });
  server.on('error', (e) => { console.error(`molty-display: ${e.code === 'EADDRINUSE' ? `port ${port} is taken: pass another with --port` : e.message}`); process.exit(1); });
  server.listen(port, '127.0.0.1', () => console.log(`molty-display preview: http://127.0.0.1:${server.address().port}/  (Ctrl-C to stop)`));
  return server;
}
