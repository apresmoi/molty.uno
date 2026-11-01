// Molty: who lives in the water, how it looks and how it behaves. Tapping a creature sends it;
// the playground for trying the fluid is closed until asked for.
import { UnsupportedError } from '../api/device';
import type { FluidOrder, FluidState } from '../api/types';
import type { AppCtx } from '../app';
import type { CreatureEntry } from '../catalog/catalog';
import { h, mount, note } from '../ui/dom';
import { debounce, errText } from '../ui/fmt';
import { dropdown, field, more, pills, section } from '../ui/kit';
import { preview } from '../ui/preview';
import { RING_CHOICES, ringChoice, ringSupported } from '../ring/ring';
import { IDLE_CHOICES, idleChoice, idleSupported } from '../idle/idle';
import { shakeChoice, shakeSupported } from '../shake/shake';
import { GESTURE_ACTIONS, GESTURE_INPUTS, gestureChoice, gesturesSupported } from '../shake/gestures';

const STATES = ['blob', 'creature', 'thinking', 'text', 'autoplay', 'effect:ripple', 'effect:vortex', 'effect:wave'];
const PALETTES: Record<string, string> = { green: '#7cf29a', amber: '#ffb347', pastel: '#c9b6ff' };
const cap = (w: string) => w[0].toUpperCase() + w.slice(1);

export function creaturePage(root: HTMLElement, { api, catalog }: AppCtx) {
  let alive = true, timer = 0, picked: string | null = null, sending: string | null = null, fluid: FluidState | null = null, list: CreatureEntry[] = [], sig = '';
  const strip = h('div', { class: 'strip', role: 'group', 'aria-label': 'choose a creature' });
  const sendMsg = h('div', { 'aria-live': 'polite' });
  const look = h('div');
  const behave = h('div');
  const play = h('div');
  const ctlMsg = h('div');
  const text = h('textarea', { rows: 2, placeholder: 'Words for Molty to read…', 'aria-label': 'reader text' });
  const wpm = h('input', { type: 'range', min: 100, max: 800, step: 25, value: '300', 'aria-label': 'words per minute' });
  const wpmOut = h('output', null, '300 wpm');
  const word = h('div', { class: 'word', 'aria-live': 'polite' });
  const file = h('input', { type: 'file', accept: '.molty', hidden: true, 'aria-label': 'your own creature file', onchange: () => { const f = file.files?.[0]; if (f) void uploadOwn(f); file.value = ''; } });

  const order = async (o: FluidOrder, quiet = false) => {
    try { const s = await api.fluid(o); if (alive) { fluid = s; renderControls(); mount(ctlMsg); } }
    catch (e) { if (!quiet) mount(ctlMsg, note('err', errText(e))); }
  };
  const pv = preview(api, { size: 300, onTap: (x, y) => order({ ripple: { x, y } }, true) });
  const sendWpm = debounce((v: number) => order({ wpm: v }), 350);

  mount(root,
    h('div', { class: 'split' },
      h('div', { class: 'stick' }, pv.el, h('p', { class: 'why center' }, 'Tap the screen to make a ripple')),
      h('div', null,
        h('h1', null, 'Molty'),
        h('p', { class: 'lede' }, 'Who lives in the water. Tap one to send it.'),
        strip, sendMsg, file,
        look, behave, ctlMsg,
        more('Playground', play, h('label', null, 'Read this'), text,
          h('div', { class: 'row' }, wpm, wpmOut),
          h('div', { class: 'row' }, word, h('button', { type: 'button', class: 'primary', onclick: () => order({ text: text.value, wpm: +wpm.value, state: 'text' }) }, 'Read it'))))));

  wpm.addEventListener('input', () => { wpmOut.textContent = `${wpm.value} wpm`; sendWpm(+wpm.value); });

  function renderControls() {
    const s = fluid;
    const active = s?.autoplay ? 'autoplay' : s?.state ?? '';
    // redraw only on news: an open dropdown must survive the poll
    const next = JSON.stringify([active, s?.palette, s?.ring_color, s?.shake_talk, s?.gestures, s?.idle_life]);
    if (next !== sig && document.activeElement?.tagName !== 'SELECT') {
      sig = next;
      mount(play, h('label', null, 'What the water does'),
        pills('state', STATES.map((id) => ({ id, label: cap(id.replace('effect:', '')) })), active, (id) => order({ state: id })));
      mount(look, section('Look', null,
        h('div', { class: 'sub first' }, h('label', null, 'Colours'),
          pills('palette', Object.entries(PALETTES).map(([id, sw]) => ({ id, label: cap(id), sw })), s?.palette, (p) => order({ palette: p }))),
        ringSupported(s) ? h('div', { class: 'sub' }, h('label', null, 'Circle while listening'),
          pills('listening ring colour', RING_CHOICES.map((c) => ({ id: c.id, label: c.id === 'accent' ? 'Match colours' : cap(c.id), sw: c.id === 'accent' ? undefined : c.value })), ringChoice(s?.ring_color) as string,
            (id) => order({ ring_color: RING_CHOICES.find((c) => c.id === id)!.value }))) : null));
      const ON = GESTURE_ACTIONS.map((a) => ({ id: a.id, label: cap(a.label) }));
      mount(behave,
        idleSupported(s) ? section('When nobody’s talking', 'Small movements by itself: glances, blinks, a stretch. Always silent.',
          pills('idle life', IDLE_CHOICES, idleChoice(s?.idle_life), (id) => order({ idle_life: id }))) : null,
        shakeSupported(s) || gesturesSupported(s) ? section('Touch and motion', 'What each one does. A tap always stops a recording and sends it.',
          shakeSupported(s) ? field('Shake', 'Start talking to your agent', dropdown('shake', [{ id: 'on', label: 'Talk' }, { id: 'off', label: 'Tell agent' }], shakeChoice(s?.shake_talk) ? 'on' : 'off', (id) => order({ shake_talk: id === 'on' }))) : null,
          gesturesSupported(s) ? GESTURE_INPUTS.map((g) => field(cap(g.label), null,
            dropdown(g.label, ON, gestureChoice(s?.gestures, g), (a) => order({ gestures: { [g.id]: a } })))) : null) : null);
    }
    if (s && document.activeElement !== wpm && s.wpm) { wpm.value = String(s.wpm); wpmOut.textContent = `${s.wpm} wpm`; }
    if (s && !text.value && s.text) text.value = s.text;
    word.textContent = s?.word ? s.word : s?.nwords ? `${s.word_idx ?? 0}/${s.nwords} words` : '';
  }

  function renderStrip() {
    const on = sending ?? picked ?? fluid?.creature_info?.name ?? null;
    mount(strip, list.map((c) => h('button', {
      type: 'button', class: 'tile' + (on === c.name ? ' on' : ''), 'aria-pressed': String(on === c.name), 'aria-busy': sending === c.name ? 'true' : undefined,
      onclick: () => { if (!sending && picked !== c.name) void send(c); },
    }, c.thumb ? h('img', { src: c.thumb, alt: '', width: 64, height: 64 }) : h('span', { class: 'ph' }), h('span', { class: 'n' }, sending === c.name ? 'Sending…' : c.name))),
    h('button', { type: 'button', class: 'tile', onclick: () => file.click() }, h('span', { class: 'ph' }, '+'), h('span', { class: 'n' }, 'Your own')));
  }

  async function deliver(name: string, id: string, bytes: () => Promise<Uint8Array>) {
    if (sending) return;
    sending = name; renderStrip(); mount(sendMsg);
    try {
      await api.uploadCreature(id, await bytes());
      await api.fluid({ state: 'creature' }).catch(() => {});
      picked = name;
      mount(sendMsg, note('ok', `${name} is moving in.`));
    } catch (e) {
      mount(sendMsg, e instanceof UnsupportedError
        ? note('warn', 'This firmware can’t take new creatures yet. An update will add it.')
        : note('err', `Couldn’t send ${name}: ${errText(e)}`));
    } finally { sending = null; if (alive) renderStrip(); }
  }
  const send = (c: CreatureEntry) => deliver(c.name, c.id, () => catalog.fetchFile(c));
  const uploadOwn = (f: File) => { const id = f.name.replace(/\.molty$/, ''); return deliver(id, id, async () => new Uint8Array(await f.arrayBuffer())); };

  const poll = async () => {
    const st = await api.creatureState().catch(() => null);
    if (!alive) return;
    if (st) { const before = fluid?.creature_info?.name; fluid = st; renderControls(); if (before !== st.creature_info?.name && !picked) renderStrip(); }
    timer = window.setTimeout(poll, 2500);
  };

  mount(strip, h('p', { class: 'why' }, 'Loading creatures…'));
  catalog.list().then((l) => { if (alive) { list = l; renderStrip(); } }, (e) => mount(strip, note('err', `Couldn’t load the creatures (${catalog.label}): ${errText(e)}`)));
  renderControls(); poll();
  return () => { alive = false; clearTimeout(timer); pv.stop(); };
}
