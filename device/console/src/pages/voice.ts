// Voice (settings): everything about how Molty talks, in one place. The relay's side (voice,
// first words, reflex emotes: ui/voice-card.ts) and the device's (listen window, turn
// acknowledgement, auto filler, kept in NVS).
import type { FluidOrder, FluidState } from '../api/types';
import type { AppCtx } from '../app';
import { ACK_CHOICES, ackChoice, ackSupported } from '../ack/ack';
import { FILLER_CHOICES, fillerChoice, fillerSupported } from '../filler/filler';
import { LISTEN_CHOICES, listenChoice, listenSupported } from '../listen/listen';
import { h, mount, note } from '../ui/dom';
import { errText } from '../ui/fmt';
import { voiceCard } from '../ui/voice-card';

export function voicePage(root: HTMLElement, { api }: AppCtx) {
  let alive = true, timer = 0;
  const voice = voiceCard();
  const talk = h('div');
  const msg = h('div');
  mount(root,
    h('h1', null, 'voice ', h('span', { class: 'dim' }, '⠶ how molty talks')),
    voice.el, h('h2', null, 'while talking'), talk, msg);

  const order = async (o: FluidOrder) => {
    try { render(await api.fluid(o)); mount(msg); }
    catch (e) { mount(msg, note('err', errText(e))); }
  };

  const chips = <T extends { id: string; label: string }>(name: string, hint: string, choices: ReadonlyArray<T>, on: string, pick: (c: T) => FluidOrder) => [
    h('label', null, name),
    h('div', { class: 'chips', role: 'group', 'aria-label': name }, choices.map((c) =>
      h('button', { type: 'button', class: 'chip' + (on === c.id ? ' on' : ''), 'aria-pressed': String(on === c.id), onclick: () => order(pick(c)) }, c.label))),
    h('p', { class: 'dim small' }, hint),
  ];

  function render(s: FluidState) {
    if (!alive) return;
    mount(talk,
      listenSupported(s) ? chips('listen window', 'how long Molty listens at most when an agent doesn’t say; it stops as soon as you finish.', LISTEN_CHOICES, listenChoice(s.listen_ms), (c) => ({ listen_ms: c.ms })) : null,
      ackSupported(s) ? chips('acknowledge', 'what Molty does the moment you finish speaking, before the agent answers.', ACK_CHOICES, ackChoice(s.ack), (c) => ({ ack: c.id })) : null,
      fillerSupported(s) ? chips('filler', 'when the agent is still quiet this long after you finish, Molty says a short “hmm”. First words replace it when they’re on.', FILLER_CHOICES, fillerChoice(s.filler_ms), (c) => ({ filler_ms: c.ms })) : null);
  }

  const poll = async () => {
    const [link, st] = await Promise.allSettled([api.link(), api.creatureState()]);
    if (!alive) return;
    if (link.status === 'fulfilled') voice.update(link.value);
    if (st.status === 'fulfilled') render(st.value);
    timer = window.setTimeout(poll, 3000);
  };
  poll();
  return () => { alive = false; clearTimeout(timer); voice.stop(); };
}
