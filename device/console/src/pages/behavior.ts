// Behavior: what Molty can do in a conversation, one tab per stage, each set up where it is used.
// Who speaks, the wait for the agent and the reactions live on the relay; the listen window,
// acknowledgement and filler delay on the device.
import type { FluidOrder, FluidState } from '../api/types';
import type { AppCtx } from '../app';
import { ACK_CHOICES, ackChoice, ackSupported } from '../ack/ack';
import { FILLER_CHOICES, FILLER_OFF, fillerChoice, fillerSupported } from '../filler/filler';
import { LISTEN_CHOICES, listenChoice, listenSupported } from '../listen/listen';
import { linkContext } from '../ui/agent-link';
import { confirmButton, copyButton, h, mount, note } from '../ui/dom';
import { errText } from '../ui/fmt';
import { dropdown, field, more, pageHead, pills, statusRow, toggle, type Choice } from '../ui/kit';
import {
  BOX_DOCS, LIVELINESS, VoiceApiError, allGaps, allReactions, allVoices, applyGap, boxLine, gapNote, gapOn, getVoice, latencyLine, pairCommand, reflexNote, reflexOn, setVoice, setupSteps, unpairBox, voiceChoices, voiceLabel, voiceOn,
  type GapFill, type Note, type VoiceStatus, type VoiceTarget,
} from '../voice/voice';

const RELAY_MS = 10000, DEVICE_MS = 3000;

const TABS = ['listening', 'thinking', 'speaking'] as const;
type Tab = typeof TABS[number];

export function behaviorPage(root: HTMLElement, { api }: AppCtx) {
  let alive = true, timer = 0, relayAt = 0, sig = '', problem: string | null = null, unsupported = false, open: string | null = null;
  let target: VoiceTarget | null = null, s: VoiceStatus | null = null, fluid: FluidState | null = null;
  const asked = location.hash.split('/')[2] as Tab;
  let tab: Tab = TABS.includes(asked) ? asked : 'listening';
  const tabs = h('div', { class: 'tabs', role: 'tablist', 'aria-label': 'behavior' });
  const body = h('div', { 'aria-live': 'polite' });
  mount(root, pageHead('Behavior', 'How Molty acts when you talk together. Changes save right away.'), tabs, body);

  /** A closed "more" that stays as the owner left it across redraws. */
  const opened = new Set<string>();
  const keep = (label: string, ...kids: Parameters<typeof more>[1][]) => {
    const d = more(label, ...kids) as HTMLDetailsElement;
    d.open = opened.has(label);
    d.addEventListener('toggle', () => { if (d.open) opened.add(label); else opened.delete(label); });
    return d;
  };

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

  /** One decision: what it changes, its answers, one line about the current one. A choice this
   *  deployment can't do doesn't save: it opens what it needs. */
  const decide = <T extends string>(title: string, head: unknown, opts: Choice<T>[], on: T | undefined, pick: (id: T) => unknown, line: Note, ...rest: unknown[]) => {
    const need = opts.find((o) => o.need && o.need === open)?.need;
    return h('section', { class: 'sec tight' }, h('h2', null, title), typeof head === 'string' ? h('p', { class: 'why' }, head) : head as never,
      pills(title.toLowerCase(), opts, on, (id) => {
        const o = opts.find((c) => c.id === id)!;
        if (o.need) { open = open === o.need ? null : o.need; render(); return; }
        open = null;
        return pick(id);
      }),
      need ? needs(need) : null,
      line.text ? h('p', { class: `status ${line.kind}` }, line.text) : null,
      ...(rest as never[]));
  };

  function needs(need: string) {
    const { title, steps } = setupSteps(need, target);
    return h('div', { class: 'needs' }, h('p', null, title), h('ol', null, steps.map((st) => h('li', null,
      st.href ? h('a', { href: st.href, target: '_blank', rel: 'noopener' }, st.text) : st.text,
      st.code ? h('div', { class: 'copy' }, h('code', null, st.code), copyButton(() => st.code!, 'Copy', 'Copied', 'pill')) : null))));
  }

  const whoSpeaks = (v: VoiceStatus) => {
    const voices = voiceChoices(v), usesBox = v.mode !== 'off' && v.mode !== 'cloud' && v.box.paired;
    return decide('Molty’s voice', 'How Molty talks to you out loud. This also sets what hears you.', allVoices(v), voiceOn(v), (id) => save((t) => setVoice(t, { mode: id })), boxLine(v),
      usesBox && voices.length ? field('Which voice', null,
        dropdown('voice', [{ id: '', label: 'Default' }, ...voices.map((id) => ({ id, label: voiceLabel(id) }))], v.voice ?? '', (id) => save((t) => setVoice(t, { voice: id || null })))) : null,
      usesBox ? boxNow(v) : null);
  };

  const whileThinking = (v: VoiceStatus) => {
    const gap = gapOn(v, fluid?.filler_ms);
    const custom = fluid && fillerChoice(fluid.filler_ms) === 'custom';
    const delays = FILLER_CHOICES.filter((c) => c.ms !== FILLER_OFF).map((c) => ({ id: c.id, label: `${c.ms / 1000} s` }));
    return decide<GapFill>('Waiting for your agent', 'What Molty does between you finishing and your agent answering.', allGaps(v), gap, async (id) => {
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
    const react = toggle('React while you talk', 'Molty’s face responds to what you say, before your agent answers.', on !== 'off', (c) => save((t) => setVoice(t, { reflex: c ? 'auto' : 'off' })));
    if (on === 'off') return h('section', { class: 'sec tight' }, h('h2', null, 'Reactions'), react);
    return decide('Reactions', [react, h('p', { class: 'why' }, 'What Molty uses to understand you:')], allReactions(v).filter((o) => o.id !== 'off'), on, (id) => save((t) => setVoice(t, { reflex: id })), reflexNote(v),
      r.liveliness ? field('How expressive', 'How often and how strongly Molty reacts.', dropdown('how expressive', LIVELINESS, r.liveliness, (id) => save((t) => setVoice(t, { liveliness: id })))) : null,
      keep('Advanced', toggle('Compare reaction engines', 'Runs every engine on each phrase and writes the results to Activity. Molty only shows the chosen one.', r.compare, (c) => save((t) => setVoice(t, { reflex_compare: c })))));
  };

  const listenWindow = (f: FluidState) => {
    const window = listenSupported(f), ack = ackSupported(f);
    if (!window && !ack) return null;
    return h('section', { class: 'sec tight' }, h('h2', null, 'Microphone'),
      window ? field('Longest it listens', 'The mic closes after this, even if you are still talking.', dropdown('longest it listens', LISTEN_CHOICES, listenChoice(f.listen_ms), (id) => order({ listen_ms: LISTEN_CHOICES.find((c) => c.id === id)!.ms }))) : null,
      ack ? field('When you finish', 'What Molty does the moment you stop talking.', dropdown('when you finish', ACK_CHOICES, ackChoice(f.ack), (id) => order({ ack: id }))) : null);
  };

  /** The voice box in use: how it is set up right now, and where to change that. */
  function boxNow(v: VoiceStatus) {
    const lat = latencyLine(v);
    const cmd = target ? h('code', null, pairCommand(target)) : null;
    const can = (label: string, on: boolean) => statusRow(label, on ? 'Yes' : 'No', 'dim');
    return h('div', { class: 'needs' }, h('p', null, 'What your voice box does right now'),
      statusRow('Status', v.box.connected ? 'Connected' : 'Offline', v.box.connected ? 'ok' : 'warn'),
      can('Hears you', v.box.stt),
      can('Speaks', v.box.tts),
      can('Starts answers', !!v.first_words?.available),
      can('Picks reactions', !!v.reflex?.engines.local),
      lat ? h('p', { class: 'why' }, lat) : null,
      h('p', null, h('a', { href: BOX_DOCS, target: '_blank', rel: 'noopener' }, 'How to set up or change your voice box')),
      keep('Replace or remove it',
        target ? h('div', { class: 'sub' }, h('label', null, 'Use a different voice box'),
          h('p', { class: 'why' }, 'Run this on that computer, then tap Molty when it shows the code. The code changes every 15 minutes.'),
          h('div', { class: 'copy' }, cmd, copyButton(() => cmd?.textContent || '', 'Copy', 'Copied', 'pill'))) : null,
        h('div', { class: 'sub' }, h('label', null, 'Remove this voice box'),
          h('p', { class: 'why' }, 'Its access stops working. You can pair it again later.'),
          confirmButton('Unpair voice box', 'Sure? Unpair', () => void save((t) => unpairBox(t))))));
  }

  const textOnly = h('p', { class: 'status dim' }, 'Molty is text only, so there is nothing to set here. Pick a voice under Speaking.');

  function pane() {
    const off = s?.mode === 'off';
    if (!s) return h('p', { class: 'why' }, unsupported ? 'This relay can’t change the voice yet.' : problem ? '' : 'Asking the relay…');
    if (tab === 'speaking') return whoSpeaks(s);
    if (off) return textOnly;
    return tab === 'thinking' ? whileThinking(s) : [whileTalking(s), fluid ? listenWindow(fluid) : null];
  }

  function render() {
    if (!alive) return;
    mount(tabs, TABS.map((t) => h('button', {
      type: 'button', role: 'tab', class: t === tab ? 'here' : '', 'aria-selected': String(t === tab),
      onclick: () => { tab = t; open = null; history.replaceState(null, '', `#/behavior/${t}`); render(); },
    }, t)));
    mount(body, problem ? note('warn', problem) : null, pane());
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
