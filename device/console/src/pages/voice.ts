// Voice: how Molty talks, one line per decision. Who speaks, the wait for the agent and the
// reactions live on the relay; the listen window, acknowledgement and filler delay on the device.
import type { FluidOrder, FluidState } from '../api/types';
import type { AppCtx } from '../app';
import { ACK_CHOICES, ackChoice, ackSupported } from '../ack/ack';
import { FILLER_CHOICES, FILLER_OFF, fillerChoice, fillerSupported } from '../filler/filler';
import { LISTEN_CHOICES, listenChoice, listenSupported } from '../listen/listen';
import { linkContext } from '../ui/agent-link';
import { confirmButton, copyButton, h, mount, note } from '../ui/dom';
import { errText } from '../ui/fmt';
import { dropdown, field, more, pageHead, pills, section, toggle, type Choice } from '../ui/kit';
import {
  LIVELINESS, VoiceApiError, allGaps, allReactions, allVoices, applyGap, boxLine, gapNote, gapOn, getVoice, latencyLine, pairCommand, reflexNote, reflexOn, servingLine, setVoice, setupSteps, unpairBox, voiceChoices, voiceLabel, voiceOn,
  type GapFill, type Note, type VoiceStatus, type VoiceTarget,
} from '../voice/voice';

const RELAY_MS = 10000, DEVICE_MS = 3000;

export function voicePage(root: HTMLElement, { api }: AppCtx) {
  let alive = true, timer = 0, relayAt = 0, sig = '', problem: string | null = null, unsupported = false, open: string | null = null;
  let target: VoiceTarget | null = null, s: VoiceStatus | null = null, fluid: FluidState | null = null;
  const body = h('div', { 'aria-live': 'polite' });
  const adv = h('div');
  const advanced = more('Advanced', adv);
  mount(root, pageHead('Voice', 'How Molty talks with you. Changes save right away.'), body, advanced);

  /** Saves on the relay and shows the answer, or says why it didn't save. */
  const save = async (fn: (t: VoiceTarget) => Promise<VoiceStatus>) => {
    if (!target) return;
    try { s = await fn(target); problem = null; }
    catch (e) { problem = `Not saved: ${e instanceof VoiceApiError ? e.message : errText(e)}`; }
    render();
  };
  const order = async (o: FluidOrder) => {
    try { fluid = await api.fluid(o); problem = null; }
    catch (e) { problem = `Not saved: ${errText(e)}`; }
    render();
  };

  /** One decision: a question, its answers, one line about the current one. A choice this
   *  deployment can't do doesn't save: it opens what it needs. */
  const decide = <T extends string>(title: string, opts: Choice<T>[], on: T | undefined, pick: (id: T) => unknown, line: Note, ...more: unknown[]) => {
    const need = opts.find((o) => o.need && o.need === open)?.need;
    return h('section', { class: 'sec tight' }, h('h2', null, title),
      pills(title.toLowerCase(), opts, on, (id) => {
        const o = opts.find((c) => c.id === id)!;
        if (o.need) { open = open === o.need ? null : o.need; render(); return; }
        open = null;
        return pick(id);
      }),
      need ? needs(need) : null,
      line.text ? h('p', { class: `status ${line.kind}` }, line.text) : null,
      ...(more as never[]));
  };

  function needs(need: string) {
    const { title, steps } = setupSteps(need, target);
    return h('div', { class: 'needs' }, h('p', null, title), h('ol', null, steps.map((st) => h('li', null,
      st.href ? h('a', { href: st.href, target: '_blank', rel: 'noopener' }, st.text) : st.text,
      st.code ? h('div', { class: 'copy' }, h('code', null, st.code), copyButton(() => st.code!, 'Copy', 'Copied', 'pill')) : null))));
  }

  const whoSpeaks = (v: VoiceStatus) => {
    const voices = voiceChoices(v), usesBox = v.mode !== 'off' && v.mode !== 'cloud' && v.box.paired;
    return decide('Who speaks?', allVoices(v), voiceOn(v), (id) => save((t) => setVoice(t, { mode: id })), boxLine(v),
      usesBox && voices.length ? field('Voice', null,
        dropdown('voice', [{ id: '', label: 'Default' }, ...voices.map((id) => ({ id, label: voiceLabel(id) }))], v.voice ?? '', (id) => save((t) => setVoice(t, { voice: id || null })))) : null);
  };

  const whileThinking = (v: VoiceStatus) => {
    const gap = gapOn(v, fluid?.filler_ms);
    const custom = fluid && fillerChoice(fluid.filler_ms) === 'custom';
    const delays = FILLER_CHOICES.filter((c) => c.ms !== FILLER_OFF).map((c) => ({ id: c.id, label: `${c.ms / 1000} s` }));
    return decide<GapFill>('While your agent thinks', allGaps(v), gap, async (id) => {
      await save((t) => applyGap(t, id, api));
      fluid = await api.creatureState().catch(() => fluid);
      render();
    }, gapNote(v, gap),
    gap === 'phrases' && fillerSupported(fluid) ? field('Say it after', null,
      dropdown('filler delay', custom ? [{ id: 'custom', label: `${(fluid!.filler_ms! / 1000).toFixed(1)} s` }, ...delays] : delays, fillerChoice(fluid?.filler_ms),
        (id) => order({ filler_ms: FILLER_CHOICES.find((c) => c.id === id)!.ms }))) : null);
  };

  const whileTalking = (v: VoiceStatus) => {
    const r = v.reflex;
    if (!r) return null;
    const on = reflexOn(r);
    return decide('While you talk', allReactions(v), on, (id) => save((t) => setVoice(t, { reflex: id })), reflexNote(v),
      on !== 'off' && r.liveliness ? field('How expressive', null, dropdown('how expressive', LIVELINESS, r.liveliness, (id) => save((t) => setVoice(t, { liveliness: id })))) : null);
  };

  const listening = (f: FluidState) => {
    const window = listenSupported(f), ack = ackSupported(f);
    if (!window && !ack) return null;
    return h('section', { class: 'sec tight' }, h('h2', null, 'Listening'),
      window ? field('Longest it listens', null, dropdown('longest it listens', LISTEN_CHOICES, listenChoice(f.listen_ms), (id) => order({ listen_ms: LISTEN_CHOICES.find((c) => c.id === id)!.ms }))) : null,
      ack ? field('When you finish', null, dropdown('when you finish', ACK_CHOICES, ackChoice(f.ack), (id) => order({ ack: id }))) : null);
  };

  function renderAdvanced() {
    const v = s;
    const lat = v ? latencyLine(v) : '';
    const cmd = target ? h('code', null, pairCommand(target)) : null;
    mount(adv,
      v?.box.paired ? h('p', { class: 'why' }, `Voice box: ${v.box.name ?? 'unnamed'}${v.box.version ? `, version ${v.box.version}` : ''}.`) : null,
      v ? h('p', { class: 'why' }, servingLine(v)) : null,
      lat ? h('p', { class: 'why' }, lat) : null,
      v?.reflex && reflexOn(v.reflex) !== 'off' ? toggle('Compare reaction engines', 'Runs every engine on each phrase and writes the results to Activity. Molty only shows the chosen one.', v.reflex.compare, (on) => save((t) => setVoice(t, { reflex_compare: on }))) : null,
      target ? h('div', { class: 'sub' }, h('label', null, v?.box.paired ? 'Pair another voice box' : 'Add a voice box'),
        h('p', { class: 'why' }, 'Run this on the computer with the voice box, then tap Molty when it shows the code. The code changes every 15 minutes.'),
        h('div', { class: 'copy' }, cmd, copyButton(() => cmd?.textContent || '', 'Copy', 'Copied', 'pill'))) : null,
      v?.box.paired ? h('div', { class: 'sub' }, h('label', null, 'Remove this voice box'),
        h('p', { class: 'why' }, 'Its access stops working. You can pair it again later.'),
        confirmButton('Unpair voice box', 'Sure? Unpair', () => void save((t) => unpairBox(t)))) : null);
  }

  function render() {
    if (!alive) return;
    const text = s?.mode === 'off';
    mount(body,
      problem ? note('warn', problem) : null,
      s ? whoSpeaks(s) : section('Who speaks?', null, h('p', { class: 'why' }, unsupported ? 'This relay can’t change the voice yet.' : problem ? '' : 'Asking the relay…')),
      s && !text ? whileThinking(s) : null,
      s && !text ? whileTalking(s) : null,
      fluid && !text ? listening(fluid) : null);
    renderAdvanced();
    advanced.hidden = !s;
  }

  const poll = async () => {
    const [link, st] = await Promise.allSettled([api.link(), api.creatureState()]);
    if (!alive) return;
    if (st.status === 'fulfilled') fluid = st.value;
    if (link.status === 'fulfilled' && link.value.device_id && link.value.link_code) {
      const c = linkContext(link.value);
      target = { relay: c.relay, deviceId: c.deviceId, linkCode: c.linkCode };
    }
    if (target && !unsupported && Date.now() - relayAt > RELAY_MS) {
      relayAt = Date.now();
      try { s = await getVoice(target); problem = null; }
      catch (e) {
        if (e instanceof VoiceApiError && e.code === 'unsupported') unsupported = true;
        // the link code rotates: the next poll has the new one
        else if (!(e instanceof VoiceApiError && e.code === 'unauthorized')) problem = `Can’t reach the relay: ${errText(e)}`;
      }
      if (!alive) return;
    }
    // redraw only on news: an open dropdown must survive the poll
    const next = JSON.stringify([s, fluid?.filler_ms, fluid?.listen_ms, fluid?.ack, problem, unsupported, !!target]);
    if (next !== sig && document.activeElement?.tagName !== 'SELECT') { sig = next; render(); }
    timer = window.setTimeout(poll, DEVICE_MS);
  };
  render(); poll();
  return () => { alive = false; clearTimeout(timer); };
}
