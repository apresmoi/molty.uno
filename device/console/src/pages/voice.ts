// Voice: how Molty talks, one question at a time. Who speaks, the wait for the agent and the
// reactions live on the relay; the listen window, acknowledgement and filler delay on the device.
import type { FluidOrder, FluidState } from '../api/types';
import type { AppCtx } from '../app';
import { ACK_CHOICES, ackChoice, ackSupported } from '../ack/ack';
import { FILLER_CHOICES, FILLER_OFF, fillerChoice, fillerSupported } from '../filler/filler';
import { LISTEN_CHOICES, listenChoice, listenSupported } from '../listen/listen';
import { linkContext } from '../ui/agent-link';
import { confirmButton, copyButton, h, mount, note } from '../ui/dom';
import { errText } from '../ui/fmt';
import { choices, dropdown, more, pageHead, pills, section, toggle } from '../ui/kit';
import {
  LIVELINESS, VoiceApiError, applyGap, boxLine, gapOn, gapOptions, getVoice, latencyLine, pairCommand, reflexLine, reflexOn, reflexOptions, servingLine, setVoice, unpairBox, voiceChoices, voiceLabel, voiceOn, voiceOptions,
  type GapFill, type VoiceStatus, type VoiceTarget,
} from '../voice/voice';

const RELAY_MS = 10000, DEVICE_MS = 3000;

export function voicePage(root: HTMLElement, { api }: AppCtx) {
  let alive = true, timer = 0, relayAt = 0, sig = '', problem: string | null = null, unsupported = false;
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

  const whoSpeaks = (v: VoiceStatus) => {
    const line = boxLine(v), voices = voiceChoices(v), usesBox = voiceOn(v) !== 'cloud' && voiceOn(v) !== 'off';
    return section('Who speaks?', 'Where Molty’s voice and hearing run.',
      choices('who speaks', voiceOptions(v), voiceOn(v), (id) => save((t) => setVoice(t, { mode: id }))),
      usesBox ? h('p', { class: `status ${line.kind}` }, line.text) : null,
      usesBox && voices.length ? h('div', { class: 'sub' }, h('label', null, 'Molty’s voice'),
        dropdown('voice', [{ id: '', label: 'Voice box default' }, ...voices.map((id) => ({ id, label: voiceLabel(id) }))], v.voice ?? '', (id) => save((t) => setVoice(t, { voice: id || null })))) : null);
  };

  const whileThinking = (v: VoiceStatus) => {
    const gap = gapOn(v, fluid?.filler_ms);
    const custom = fluid && fillerChoice(fluid.filler_ms) === 'custom';
    return section('While your agent thinks', 'Agents can take seconds to answer. What should fill the silence?',
      choices<GapFill>('while your agent thinks', gapOptions(v), gap, async (id) => {
        await save((t) => applyGap(t, id, api));
        fluid = await api.creatureState().catch(() => fluid);
        render();
      }),
      gap === 'phrases' && fillerSupported(fluid) ? h('div', { class: 'sub' }, h('label', null, custom ? `Say it after (now ${(fluid!.filler_ms! / 1000).toFixed(1)} s)` : 'Say it after'),
        pills('filler delay', FILLER_CHOICES.filter((c) => c.ms !== FILLER_OFF).map((c) => ({ id: c.id, label: `${c.ms / 1000} s` })), fillerChoice(fluid?.filler_ms),
          (id) => order({ filler_ms: FILLER_CHOICES.find((c) => c.id === id)!.ms }))) : null);
  };

  const whileTalking = (v: VoiceStatus) => {
    const r = v.reflex;
    if (!r) return null;
    const on = reflexOn(r), live = LIVELINESS.find((l) => l.id === r.liveliness);
    return section('While you talk', 'Molty can react to what you say with small expressions, before your agent answers.',
      choices('reactions', reflexOptions(v), on, (id) => save((t) => setVoice(t, { reflex: id }))),
      on !== 'off' && r.liveliness ? h('div', { class: 'sub' }, h('label', null, 'How expressive'),
        pills('how expressive', LIVELINESS, r.liveliness, (id) => save((t) => setVoice(t, { liveliness: id }))),
        live ? h('p', { class: 'why' }, `${live.hint[0].toUpperCase()}${live.hint.slice(1)}.`) : null) : null,
      r.fallback ? h('p', { class: 'status warn' }, `That one isn’t available right now, so Molty is using another. ${reflexLine(r)}`) : null);
  };

  const listening = (f: FluidState) => {
    const window = listenSupported(f), ack = ackSupported(f);
    if (!window && !ack) return null;
    return section('Listening', null,
      window ? h('div', { class: 'sub first' }, h('label', null, 'Longest it listens'),
        pills('longest it listens', LISTEN_CHOICES, listenChoice(f.listen_ms), (id) => order({ listen_ms: LISTEN_CHOICES.find((c) => c.id === id)!.ms })),
        h('p', { class: 'why' }, 'Molty stops as soon as you finish. This is only the limit.')) : null,
      ack ? h('div', { class: 'sub' }, h('label', null, 'When you finish talking'),
        pills('when you finish talking', ACK_CHOICES, ackChoice(f.ack), (id) => order({ ack: id })),
        h('p', { class: 'why' }, 'Lets you know Molty heard you, before your agent answers.')) : null);
  };

  function renderAdvanced() {
    const v = s;
    const lat = v ? latencyLine(v) : '';
    const cmd = target ? h('code', null, pairCommand(target)) : null;
    mount(adv,
      v ? h('p', { class: 'why' }, servingLine(v)) : null,
      lat ? h('p', { class: 'why' }, lat) : null,
      v?.reflex && reflexOn(v.reflex) !== 'off' ? toggle('Compare reaction engines', 'Runs every engine on each phrase and writes the results to Activity. Molty only shows the chosen one.', v.reflex.compare, (on) => save((t) => setVoice(t, { reflex_compare: on }))) : null,
      target ? h('div', { class: 'sub' }, h('label', null, v?.box.paired ? 'Pair another voice box' : 'Add a voice box'),
        h('p', { class: 'why' }, 'Run speech on your own GPU computer. Install molty-voice there, run this command, then tap Molty when it shows the code. The code changes every 15 minutes.'),
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
