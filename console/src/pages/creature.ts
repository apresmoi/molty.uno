// Molty (settings): pick a Molty from the catalog (sent to the device as a .molty file),
// and play with the fluid — state, speed-reader text + WPM, palette, tap-to-ripple.
import { UnsupportedError } from '../api/device';
import type { FluidOrder, FluidState } from '../api/types';
import type { AppCtx } from '../app';
import type { CreatureEntry } from '../catalog/catalog';
import { h, mount, note } from '../ui/dom';
import { debounce, errText } from '../ui/fmt';
import { preview } from '../ui/preview';

const STATES = ['blob', 'creature', 'thinking', 'text', 'autoplay'];
const EFFECTS = ['ripple', 'vortex', 'wave'];
const PALETTES: Record<string, string> = { green: '#7cf29a', amber: '#ffb347', pastel: '#c9b6ff' };

export function creaturePage(root: HTMLElement, { api, catalog }: AppCtx) {
  let alive = true, timer = 0, chosen: CreatureEntry | null = null, fluid: FluidState | null = null;
  const cards = h('div', { class: 'cards', role: 'listbox', 'aria-label': 'moltys' });
  const send = h('button', { class: 'primary', type: 'button', disabled: true, onclick: () => upload() }, 'send to molty');
  const sendMsg = h('div');
  const current = h('div', { class: 'dim small' });
  const ctlMsg = h('div');
  const stateRow = h('div', { class: 'chips', role: 'group', 'aria-label': 'state' });
  const palRow = h('div', { class: 'chips', role: 'group', 'aria-label': 'palette' });
  const text = h('textarea', { rows: 3, placeholder: 'words for the speed reader…', 'aria-label': 'reader text' });
  const wpm = h('input', { type: 'range', min: 100, max: 800, step: 25, value: '300', 'aria-label': 'words per minute' });
  const wpmOut = h('output', null, '300 wpm');
  const word = h('div', { class: 'word', 'aria-live': 'polite' });

  const order = async (o: FluidOrder, quiet = false) => {
    try { const s = await api.fluid(o); if (alive) { fluid = s; renderControls(); mount(ctlMsg); } }
    catch (e) { if (!quiet) mount(ctlMsg, note('err', errText(e))); }
  };
  const pv = preview(api, { size: 320, onTap: (x, y) => order({ ripple: { x, y } }, true) });
  const sendWpm = debounce((v: number) => order({ wpm: v }), 350);

  mount(root,
    h('h1', null, 'molty ', h('span', { class: 'dim' }, '⠶ who lives in the water')),
    cards, h('div', { class: 'row' }, current, send), sendMsg,
    h('h2', null, 'the fluid'),
    h('div', { class: 'split' },
      h('div', null, pv.el, h('p', { class: 'dim small center' }, 'tap the screen to make a ripple')),
      h('div', { class: 'controls' },
        h('label', null, 'state'), stateRow,
        h('label', null, 'palette'), palRow,
        h('label', null, 'speed reader'), text,
        h('div', { class: 'row' }, wpm, wpmOut),
        h('div', { class: 'row' }, word, h('button', { type: 'button', onclick: () => order({ text: text.value, wpm: +wpm.value, state: 'text' }) }, 'read it ⠶')),
        ctlMsg)));

  wpm.addEventListener('input', () => { wpmOut.textContent = `${wpm.value} wpm`; sendWpm(+wpm.value); });

  function renderControls() {
    const s = fluid;
    const active = s?.autoplay ? 'autoplay' : s?.state ?? '';
    mount(stateRow,
      STATES.map((st) => h('button', { type: 'button', class: 'chip' + (active === st ? ' on' : ''), 'aria-pressed': String(active === st), onclick: () => order({ state: st }) }, st)),
      EFFECTS.map((fx) => {
        const v = `effect:${fx}`;
        return h('button', { type: 'button', class: 'chip fx' + (active === v ? ' on' : ''), 'aria-pressed': String(active === v), onclick: () => order({ state: v }) }, fx);
      }));
    mount(palRow, Object.entries(PALETTES).map(([p, c]) =>
      h('button', { type: 'button', class: 'chip' + (s?.palette === p ? ' on' : ''), 'aria-pressed': String(s?.palette === p), style: `--sw:${c}`, onclick: () => order({ palette: p }) },
        h('span', { class: 'swatch' }), p)));
    if (s && document.activeElement !== wpm && s.wpm) { wpm.value = String(s.wpm); wpmOut.textContent = `${s.wpm} wpm`; }
    if (s && !text.value && s.text) text.value = s.text;
    word.textContent = s?.word ? `⠶ ${s.word}` : s?.nwords ? `${s.word_idx ?? 0}/${s.nwords} words` : '';
  }

  const poll = async () => {
    const [reg, st] = await Promise.allSettled([api.creatures(), api.creatureState()]);
    if (!alive) return;
    if (reg.status === 'fulfilled') {
      const cur = reg.value.current;
      mount(current, 'on molty now: ', h('b', null, cur || 'nothing'),
        cur && cur !== 'fluid' && reg.value.available.includes('fluid')
          ? [' ⠂ ', h('button', { type: 'button', class: 'ghost small', onclick: async () => { try { await api.selectCreature('fluid'); poll(); } catch (e) { mount(ctlMsg, note('err', errText(e))); } } }, 'wake the fluid')] : null);
    }
    if (st.status === 'fulfilled') { fluid = st.value; renderControls(); }
    timer = window.setTimeout(poll, 2500);
  };

  function renderCards(list: CreatureEntry[]) {
    mount(cards, list.map((c) => h('button', {
      type: 'button', role: 'option', class: 'creature' + (chosen?.id === c.id ? ' on' : ''), 'aria-selected': String(chosen?.id === c.id),
      onclick: () => { chosen = c; send.disabled = false; send.textContent = `send ${c.name} to molty`; renderCards(list); mount(sendMsg); },
    }, c.thumb ? h('img', { src: c.thumb, alt: '', width: 72, height: 72 }) : null, h('b', null, c.name), c.blurb ? h('span', { class: 'dim small' }, c.blurb) : null)));
  }

  async function upload() {
    if (!chosen) return;
    send.disabled = true; const label = send.textContent; send.textContent = 'sending ⠴';
    try {
      const bytes = await catalog.fetchFile(chosen);
      await api.uploadCreature(chosen.id, bytes);
      await api.fluid({ state: 'creature' }).catch(() => {});
      mount(sendMsg, note('ok', `${chosen.name} is moving in ⠶`));
      poll();
    } catch (e) {
      mount(sendMsg, e instanceof UnsupportedError
        ? note('warn', 'this firmware can\'t take new moltys yet — an update will add it. the fluid below still works.')
        : note('err', errText(e)));
    } finally { send.disabled = false; send.textContent = label; }
  }

  catalog.list().then((l) => alive && renderCards(l), (e) => mount(cards, note('err', `catalog (${catalog.label}): ${errText(e)}`)));
  renderControls(); poll();
  return () => { alive = false; clearTimeout(timer); pv.stop(); };
}
