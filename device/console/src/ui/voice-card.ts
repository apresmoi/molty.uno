// The Voice card on the agent page: the owner's voice box (voice/voice.ts). Mode chips
// (like the listening-ring chips), the box's status and last latencies, its default voice,
// unpair, and how to pair one. Talks to the device's relay, authenticated with the link
// code the page already polls from Molty; follows the update card's shape (quiet when the
// relay can't be reached, a note when an action fails).
import type { LinkInfo } from '../api/types';
import {
  VOICE_MODES, VoiceApiError, boxLine, getVoice, latencyLine, pairCommand, servingLine, setVoice, unpairBox, voiceChoices, voiceLabel,
  type VoiceMode, type VoiceStatus, type VoiceTarget,
} from '../voice/voice';
import { linkContext } from './agent-link';
import { confirmButton, copyButton, h, mount, note } from './dom';

const POLL_MS = 10000;

export interface VoiceCard { el: HTMLElement; update(link: LinkInfo | null): void; stop(): void }

export function voiceCard(fetchImpl: typeof fetch = (...a) => fetch(...a)): VoiceCard {
  const el = h('div', { class: 'voice card', 'aria-live': 'polite' });
  let target: VoiceTarget | null = null;
  let status: VoiceStatus | null = null;
  let problem: string | null = null;
  let unsupported = false;
  let alive = true, timer = 0, busy = false, key = '';

  function update(link: LinkInfo | null) {
    if (!link || !link.device_id || !link.link_code) return;
    const c = linkContext(link);
    target = { relay: c.relay, deviceId: c.deviceId, linkCode: c.linkCode };
    const k = `${c.relay}|${c.deviceId}`;
    if (k !== key) { key = k; status = null; unsupported = false; void refresh(); }
  }

  async function refresh() {
    clearTimeout(timer);
    if (!alive || !target) return;
    if (!busy) {
      try { status = await getVoice(target, fetchImpl); problem = null; }
      catch (e) {
        if (e instanceof VoiceApiError && e.code === 'unsupported') unsupported = true;
        // the code rotates every 15 min: the next poll uses the new one, so a 401 is transient
        else if (!(e instanceof VoiceApiError && e.code === 'unauthorized')) problem = (e as Error).message;
      }
      render();
    }
    if (alive && !unsupported) timer = window.setTimeout(() => void refresh(), POLL_MS);
  }

  async function act(fn: (t: VoiceTarget) => Promise<VoiceStatus>) {
    if (!target) return;
    busy = true;
    try { status = await fn(target); problem = null; }
    catch (e) { problem = `not saved: ${e instanceof VoiceApiError ? e.message : (e as Error).message}`; }
    finally { busy = false; render(); }
  }

  function render() {
    const head = h('div', { class: 'row' }, h('b', { class: 'hi' }, 'voice'), h('span', { class: 'dim small' }, 'who speaks and listens for Molty'));
    if (unsupported) { mount(el, head, h('p', { class: 'dim small' }, 'this relay has no voice box support yet.')); return; }
    if (!status) { mount(el, head, problem ? note('warn', problem) : h('p', { class: 'dim small' }, 'asking the relay…')); return; }
    const s = status;
    const line = boxLine(s);
    const modes = h('div', { class: 'chips', role: 'group', 'aria-label': 'voice mode' },
      VOICE_MODES.map((m) => h('button', {
        type: 'button', class: 'chip' + (s.mode === m.id ? ' on' : ''), 'aria-pressed': String(s.mode === m.id), title: m.hint,
        onclick: () => s.mode !== m.id && void act((t) => setVoice(t, { mode: m.id as VoiceMode }, fetchImpl)),
      }, m.label)));
    const choices = voiceChoices(s);
    const select = choices.length ? h('select', {
      'aria-label': 'box voice',
      onchange: (ev: Event) => { const v = (ev.target as HTMLSelectElement).value; void act((t) => setVoice(t, { voice: v || null }, fetchImpl)); },
    }, h('option', { value: '', selected: !s.voice }, 'the box’s default'),
      choices.map((v) => h('option', { value: v, selected: s.voice === v, title: v }, voiceLabel(v)))) : null;
    const lat = latencyLine(s);
    mount(el, head,
      h('p', { class: `small ${line.kind === 'ok' ? 'ok' : line.kind === 'warn' ? 'warn' : 'dim'}` }, line.text),
      h('label', null, 'mode'), modes,
      h('p', { class: 'dim small' }, servingLine(s)),
      lat ? h('p', { class: 'dim small' }, lat) : null,
      select ? h('div', { class: 'row' }, h('label', null, 'box voice'), select) : null,
      s.box.paired ? h('div', { class: 'row' }, h('span', { class: 'dim small' }, 'the box’s token stops working; pair again to bring it back'),
        confirmButton('Unpair voice box', 'Sure? Unpair', () => void act((t) => unpairBox(t, fetchImpl)))) : null,
      pairHelp(),
      problem ? note('err', problem) : null);
  }

  function pairHelp() {
    if (!target) return null;
    const cmd = h('code', null, pairCommand(target));
    return h('details', { class: 'adv' }, h('summary', null, status?.box.paired ? 'Pair another voice box' : 'Pair a voice box'),
      h('p', { class: 'adv-note' }, 'Run your own speech on a GPU box (Kyutai STT + TTS). On the box, install molty-voice (voicebox/ in the molty repo), run this, then tap Molty when it shows the code:'),
      h('div', { class: 'copy' }, cmd, copyButton(() => cmd.textContent || '', 'Copy', 'Copied', 'pill')),
      h('p', { class: 'adv-note' }, 'Then keep molty-voice run going (systemd or compose: see its README). The code above rotates every 15 min.'));
  }

  return { el, update, stop() { alive = false; clearTimeout(timer); } };
}
