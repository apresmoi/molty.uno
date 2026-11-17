// Rigged creature packages (docs/CREATURES.md section 12): every route between pose families is authored, every mood and
// gesture name plays, the planted paws never slide (through clips, routes and crossfades), and the dots draw.
// node --test tools/creatures/clip.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { clipPlayer, route, pointOn, MOODS } from '../../device/creatures/js/clip-player.js';
import { clipDots, VIEW_SIZE } from '../../device/creatures/js/clip-dots.js';
import { ACTIONS } from '../../device/creatures/js/actions.js';

const ROOT = new URL('../../packages/', import.meta.url);
const rigged = readdirSync(ROOT).filter((d) => existsSync(new URL(`${d}/${d}.json`, ROOT)))
  .map((d) => ({ id: d, pkg: JSON.parse(readFileSync(new URL(`${d}/${d}.json`, ROOT), 'utf8')) }))
  .filter((c) => c.pkg.kind === 'creature' && c.pkg.creature.rig);
const DOT = VIEW_SIZE / 104;                          // a braille dot on the 1.46, in body units

for (const { id, pkg } of rigged) {
  const rig = pkg.creature.rig, g = rig.graph;

  test(`${id}: every ordered pair of pose families has an authored route`, () => {
    const fams = g.families, rows = [];
    for (const a of fams) {
      const row = [];
      for (const b of fams) {
        if (a === b) { row.push('-'); continue; }
        const r = route(g, a, b);
        assert.ok(r && r.length, `${a} -> ${b}: no route`);
        let at = a;
        for (const c of r) {                          // each hop an authored clip from where the last one ended
          const clip = rig.clips[c];
          assert.ok(clip, `${a} -> ${b}: ${c} is not a clip`);
          assert.equal(clip.from, at, `${a} -> ${b}: ${c} starts in ${clip.from}, not ${at}`);
          assert.notEqual(clip.from, clip.to, `${c} is not a transition`);
          at = clip.to;
        }
        assert.equal(at, b);
        row.push(r.join(' + '));
      }
      rows.push(row);
    }
    // between resting families every pair is one authored clip; in and out of the walk at most one stop on the way (standing up first)
    for (const a of fams) for (const b of fams) if (a !== b) assert.ok(route(g, a, b).length <= (a === 'walk' || b === 'walk' ? 2 : 1), `${a} -> ${b}: ${route(g, a, b).join(' + ')}`);
    console.log(`routes (${id}):\n` + fams.map((a, i) => `  ${a.padEnd(7)} ${rows[i].map((c) => c.padEnd(15)).join(' ')}`).join('\n'));
  });

  test(`${id}: every mood and gesture name plays a clip; every family has a resting loop`, () => {
    for (const m of MOODS) assert.ok(rig.clips[g.moods[m]], `mood ${m}`);
    for (const a of Object.keys(ACTIONS)) assert.ok(rig.clips[g.gestures[a]], `shared gesture ${a}`);
    for (const k of Object.keys(pkg.creature.gestures)) assert.ok(rig.clips[g.gestures[k]], `gesture ${k}`);
    for (const f of g.families) assert.ok(rig.clips[g.rest[f]]?.loop, `family ${f} rest`);
    for (const [n, c] of Object.entries(rig.clips)) assert.ok(g.families.includes(c.from) && g.families.includes(c.to), `${n}: families`);
  });

  test(`${id}: called (a tap, a shake) from any state and heading, it comes to the front facing the viewer, attending`, () => {
    const front = (p) => { const f = p.facing, w = f - 2 * Math.PI * Math.round(f / (2 * Math.PI)); return Math.abs(w) < 0.15; };
    const near = (p) => Math.abs(p.place[0]) < 0.3;     // across the frame (depth does not show; the frame is 1.9 wide)
    const states = [];
    for (const m of MOODS) for (const walkS of [0, 2, 5, 9]) states.push({ m, walkS });
    for (const gname of Object.keys(g.gestures)) states.push({ m: 'idle', gest: gname });
    let away = 0;
    for (const st of states) {
      const p = clipPlayer(rig);
      p.mood(st.m); for (let f = 0; f < 30; f++) p.update(1 / 30);
      if (st.walkS) { p.act('walk'); for (let f = 0; f < 30 * st.walkS; f++) p.update(1 / 30); }
      if (st.gest) { p.act(st.gest); for (let f = 0; f < 20; f++) p.update(1 / 30); }
      if (!front(p)) away++;
      assert.ok(p.come(), 'called');
      let f = 0, ok = false;
      for (; f < 30 * 14 && !ok; f++) { p.update(1 / 30); ok = p.clip === 'listening' && front(p) && near(p); }
      const at = `${st.m}${st.walkS ? ` after ${st.walkS} s walking` : ''}${st.gest ? ` in ${st.gest}` : ''}`;
      assert.ok(ok, `${at}: not attending at the front (${p.clip}, facing ${p.facing.toFixed(2)} rad off, at ${p.place.map((v) => v.toFixed(2))})`);
      assert.ok(f < 30 * 12, `${at}: ${(f / 30).toFixed(1)} s to come`);
      for (let k = 0; k < 30 * 6 && p.coming; k++) p.update(1 / 30);
      assert.ok(!p.coming, `${at}: the call never ends`);
    }
    assert.ok(away > 0, 'some states face away (walking back)');
  });

  test(`${id}: the paws never slide on the floor (clips, routes and crossfades, a long schedule)`, () => {
    const p = clipPlayer(rig), DT = 1 / 30, gest = Object.keys(g.gestures), worst = { slip: 0, at: '' };
    const runs = rig.contacts.map(() => null);
    for (let f = 0; f < 30 * 240; f++) {
      if (f % 300 === 0) p.mood(MOODS[(f / 300) % MOODS.length]);
      if (f % 300 === 150) p.act(gest[(f / 300 | 0) % gest.length]);
      p.update(DT);
      for (const [i, [node, at]] of rig.contacts.entries()) {
        const w = pointOn(p.clipWorld, node, at);    // the clips' own floor: the body's turn to a clip's side is not a slide
        assert.ok(w.every(Number.isFinite), 'finite');
        const down = w[1] < 0.012;                    // on the floor (the paw's contact point within 0.7 dot of it)
        if (!down) { runs[i] = null; continue; }
        if (!runs[i]) { runs[i] = w; continue; }
        const d = Math.hypot(w[0] - runs[i][0], w[2] - runs[i][2]);
        if (d > worst.slip) { worst.slip = d; worst.at = `${p.clip} t=${p.t.toFixed(2)} paw ${i}`; }
      }
    }
    console.log(`${id}: worst slip ${(worst.slip / DOT).toFixed(2)} dot (${worst.at})`);
    assert.ok(worst.slip < 0.5 * DOT, `a planted paw slid ${(worst.slip / DOT).toFixed(2)} dot at ${worst.at}`);
  });

  test(`${id}: the dots draw an outline, features and the floor in every family`, () => {
    const p = clipPlayer(rig), r = clipDots(rig, 52, 32);
    for (const f of g.families) {
      p.play(g.rest[f]); for (let k = 0; k < 20; k++) p.update(1 / 30);
      const { on, pri } = r.draw(p.world, p.cam, 0);
      let lines = 0, feats = 0, world = 0;
      for (let i = 0; i < on.length; i++) if (on[i]) { if (pri[i] === 2) lines++; else if (pri[i] >= 3) feats++; else if (pri[i] === 0) world++; }
      assert.ok(lines > 100, `${f}: ${lines} outline dots`);
      assert.ok(world > 150, `${f}: ${world} floor and dust dots`);
      if (f !== 'walk') assert.ok(feats > 5, `${f}: ${feats} feature dots`);
    }
  });
}
