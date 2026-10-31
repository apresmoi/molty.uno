// The owner's voice box (docs/AGENT-LINK.md "Voice box"): the relay's owner API
// GET|POST|DELETE <relay>/v1/devices/<id>/voice, authenticated with the device's CURRENT
// link code (header X-Molty-Link-Code) — the console reads it from Molty over the LAN
// (GET /api/link), the same "on Molty's network = owner" model as every local setting.
// Pure: the voice page and setup render what these return. Nothing assumes one machine: every
// option comes from what this relay reports, and what's missing says how to get it.

import type { Choice } from '../ui/kit';
import { FILLER_DEFAULT_MS, FILLER_OFF } from '../filler/filler';

export type VoiceMode = 'auto' | 'box' | 'cloud' | 'off';

/** Molty's reflex (docs/AGENT-LINK.md "Reflex"): who classifies what the human says while they talk. */
export type ReflexMode = 'off' | 'auto' | 'local' | 'jev' | 'openai' | 'keywords';
export type ReflexEngine = 'local' | 'jev' | 'openai' | 'keywords';
/** How lively the reflex is while the human talks (the relay's REFLEX_LIVELINESS: cadence + gates). */
export type Liveliness = 'calm' | 'normal' | 'lively';
export const LIVELINESS: ReadonlyArray<{ id: Liveliness; label: string; hint: string }> = [
  { id: 'calm', label: 'Calm', hint: 'only clear reactions, at most one every 3 s' },
  { id: 'normal', label: 'Normal', hint: 'clear reactions in full, likely ones as a small gesture, every 2 s at most' },
  { id: 'lively', label: 'Lively', hint: 'reacts sooner and more often (every 1.5 s at most): small gestures for anything plausible' },
];
export interface ReflexStatus {
  mode: ReflexMode;
  compare: boolean;
  engines: Record<ReflexEngine, boolean>;
  /** The engine the next listen fires with (null: off), and why not the chosen one. */
  engine: ReflexEngine | null;
  fallback?: string;
  timeout_ms: number;
  /** Absent on relays from before the setting (they behave like calm). */
  liveliness?: Liveliness;
}

export interface VoiceStatus {
  mode: VoiceMode;
  /** Default box voice (null: the box's own default). */
  voice: string | null;
  /** The relay's cloud provider name. */
  cloud: string;
  /** Who would speak / transcribe right now: "box", the cloud's name, or voice_box_offline. */
  serving: { tts: string; stt: string };
  /** Absent on relays from before first words. */
  first_words?: { mode: 'off' | 'opener'; available: boolean };
  /** Absent on relays from before the reflex. */
  reflex?: ReflexStatus;
  box: {
    paired: boolean;
    name: string | null;
    paired_at: number | null;
    connected: boolean;
    connected_since: number | null;
    reported_name: string | null;
    version: string | null;
    voices: string[];
    stt: boolean;
    tts: boolean;
    last: { stt_ms?: number; stt_at?: number; tts_first_audio_ms?: number; tts_at?: number };
  };
}

export class VoiceApiError extends Error {
  constructor(public code: string, message?: string) { super(message || code); }
}

export interface VoiceTarget { relay: string; deviceId: string; linkCode: string }

async function call(t: VoiceTarget, method: 'GET' | 'POST' | 'DELETE', body: unknown, fetchImpl: typeof fetch, timeoutMs = 8000): Promise<VoiceStatus> {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    let res: Response;
    try {
      res = await fetchImpl(`${t.relay.replace(/\/+$/, '')}/v1/devices/${encodeURIComponent(t.deviceId)}/voice`, {
        method,
        headers: { 'x-molty-link-code': t.linkCode, ...(body === undefined ? {} : { 'content-type': 'application/json' }) },
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: ctl.signal,
        cache: 'no-store',
      });
    } catch {
      throw new VoiceApiError('unreachable', 'can’t reach the relay');
    }
    let b: any;
    try { b = await res.json(); } catch { throw new VoiceApiError(res.status === 404 ? 'unsupported' : `http_${res.status}`); }
    if (!b || b.ok !== true) {
      // A relay from before voice boxes answers this route not_found.
      throw new VoiceApiError(b?.error === 'not_found' ? 'unsupported' : String(b?.error ?? `http_${res.status}`), b?.message);
    }
    return b.result as VoiceStatus;
  } finally { clearTimeout(timer); }
}

export const getVoice = (t: VoiceTarget, f: typeof fetch = fetch) => call(t, 'GET', undefined, f);
export const setVoice = (t: VoiceTarget, patch: { first_words?: 'off' | 'opener'; mode?: VoiceMode; voice?: string | null; reflex?: ReflexMode; reflex_compare?: boolean; liveliness?: Liveliness }, f: typeof fetch = fetch) => call(t, 'POST', patch, f);
export const unpairBox = (t: VoiceTarget, f: typeof fetch = fetch) => call(t, 'DELETE', undefined, f);

export interface Note { text: string; kind: 'ok' | 'warn' | 'dim' }

/** What is really speaking right now, whatever was chosen. */
export function boxLine(s: VoiceStatus): Note {
  if (s.mode === 'off') return { text: 'Molty shows words and emotes. No microphone, no speaker.', kind: 'dim' };
  const cloud = s.cloud !== 'none';
  if (s.mode === 'cloud' || !s.box.paired) {
    return cloud ? { text: 'Speech runs in the cloud. Nothing to install.', kind: 'dim' } : { text: 'This relay has no cloud voice, so Molty can’t speak yet.', kind: 'warn' };
  }
  if (s.box.connected) return { text: `Your voice box is connected.${cloud ? ' The cloud takes over if it goes offline.' : ''}`, kind: 'ok' };
  return { text: s.serving.tts === 'voice_box_offline' ? 'Your voice box is offline, so Molty can’t speak right now.' : 'Your voice box is offline, so the cloud is speaking for now.', kind: 'warn' };
}

const who = (v: string, s: VoiceStatus) => (v === 'box' ? 'voice box' : v === 'voice_box_offline' ? 'nobody (box offline)' : v === 'voice_off' ? 'nobody (text only)' : `cloud (${v || s.cloud})`);

/** "speaks with … ⠂ listens with …". */
export function servingLine(s: VoiceStatus): string {
  return s.serving.tts === s.serving.stt ? `Molty uses: ${who(s.serving.tts, s)}` : `speaks with ${who(s.serving.tts, s)} ⠂ listens with ${who(s.serving.stt, s)}`;
}

const secs = (ms: number) => `${(ms / 1000).toFixed(ms < 1000 ? 2 : 1)} s`;

/** The box's last measured latencies, or '' before it served anything. */
export function latencyLine(s: VoiceStatus): string {
  const l = s.box.last ?? {};
  const parts: string[] = [];
  if (typeof l.stt_ms === 'number') parts.push(`transcript ${secs(l.stt_ms)} after you stop`);
  if (typeof l.tts_first_audio_ms === 'number') parts.push(`first word in ${secs(l.tts_first_audio_ms)}`);
  return parts.length ? `last turn: ${parts.join(' ⠂ ')}` : '';
}

/** The voices the select offers: the box's list, plus the saved default when the box no longer lists it. */
export function voiceChoices(s: VoiceStatus): string[] {
  const list = [...s.box.voices];
  if (s.voice && !list.includes(s.voice)) list.unshift(s.voice);
  return list;
}

/** Short label for a Kyutai voice path: "expresso/ex03-ex01_happy_001_channel1_334s.wav" -> "ex03 happy". */
export function voiceLabel(path: string): string {
  const file = path.split('/').pop() ?? path;
  const m = /^(ex\d+)-ex\d+_([a-z]+)_/i.exec(file);
  if (m) return `${m[1]} ${m[2]}`;
  return file.replace(/\.(wav|safetensors)$/i, '').replace(/_/g, ' ');
}

/** The pairing command for the box, filled with this Molty's relay, id and current code. */
export function pairCommand(t: VoiceTarget): string {
  return `molty-voice pair --relay ${t.relay} --device ${t.deviceId} --link-code ${t.linkCode}`;
}

export type GapFill = 'off' | 'phrases' | 'local';

/** Who speaks, everything there is: what this deployment lacks says what it needs. */
export function allVoices(s: VoiceStatus): Choice<VoiceMode>[] {
  const cloud = s.cloud !== 'none';
  return [
    { id: cloud ? 'auto' : 'box', label: 'Voice box', hint: 'Speech runs on your own computer.', need: s.box.paired ? undefined : 'box' },
    { id: 'cloud', label: 'Cloud', hint: 'Speech runs in the cloud. Nothing to install.', need: cloud ? undefined : 'cloud' },
    { id: 'off', label: 'Text only', hint: 'Molty shows words and emotes. No microphone, no speaker.' },
  ];
}

/** Which of allVoices the current mode is. */
export function voiceOn(s: VoiceStatus): VoiceMode {
  if (s.mode === 'off') return 'off';
  const cloud = s.cloud !== 'none';
  if (s.mode === 'cloud' || !s.box.paired) return cloud ? 'cloud' : s.mode === 'cloud' ? 'cloud' : 'box';
  return cloud ? 'auto' : 'box';
}

/** What fills the wait until the agent speaks. */
export function allGaps(s: VoiceStatus): Choice<GapFill>[] {
  return [
    { id: 'local', label: 'First words', hint: 'A small model on your voice box starts the sentence, and your agent finishes it.', need: s.first_words?.available ? undefined : 'opener' },
    { id: 'phrases', label: 'A phrase', hint: 'Molty says something like “Hmm…” if your agent is slow.' },
    { id: 'off', label: 'Quiet', hint: 'Only the thinking animation.' },
  ];
}

export const gapOn = (s: VoiceStatus, fillerMs: number | undefined): GapFill =>
  s.first_words?.mode === 'opener' ? 'local' : fillerMs === FILLER_OFF ? 'off' : 'phrases';

/** The help line for the wait: the chosen one's, or that it isn't really happening. */
export function gapNote(s: VoiceStatus, on: GapFill): Note {
  if (on === 'local' && !s.first_words?.available) return { text: 'First words are on, but no voice box is offering them right now.', kind: 'warn' };
  return { text: allGaps(s).find((o) => o.id === on)!.hint!, kind: 'dim' };
}

/** Saves a gap choice on both sides: the relay's first words, the device's filler (a tuned delay stays). */
export async function applyGap(t: VoiceTarget, id: GapFill, device: { fluid(o: { filler_ms: number }): Promise<unknown>; creatureState(): Promise<{ filler_ms?: number }> }) {
  const s = await setVoice(t, { first_words: id === 'local' ? 'opener' : 'off' });
  if (id === 'off') await device.fluid({ filler_ms: FILLER_OFF });
  else if (id === 'phrases' && !(await device.creatureState()).filler_ms) await device.fluid({ filler_ms: FILLER_DEFAULT_MS });
  return s;
}

const REACTIONS: ReadonlyArray<Choice<ReflexMode>> = [
  { id: 'off', label: 'Off', hint: 'Molty only reacts when your agent asks it to.' },
  { id: 'keywords', label: 'Word list', hint: 'Free and simple. Reacts to words it recognises.' },
  { id: 'local', label: 'Voice box', hint: 'Free and private. A small model on your voice box.' },
  { id: 'jev', label: 'Jev', hint: 'Fast reactions from TypeSafe’s hosted model.' },
  { id: 'openai', label: 'OpenAI', hint: 'Reactions from OpenAI’s hosted model.' },
];

/** Who picks Molty's reactions while you talk. */
export const allReactions = (s: VoiceStatus): Choice<ReflexMode>[] => REACTIONS.map((o) =>
  o.id === 'local' || o.id === 'jev' || o.id === 'openai' ? { ...o, need: s.reflex?.engines[o.id] ? undefined : o.id } : o);

export const reflexOn = (r: ReflexStatus | undefined): ReflexMode | undefined => (r?.mode === 'auto' ? r.engine ?? undefined : r?.mode);

/** The help line for reactions: the chosen one's, or what is standing in for it. */
export function reflexNote(s: VoiceStatus): Note {
  const on = reflexOn(s.reflex);
  const o = REACTIONS.find((c) => c.id === on);
  if (!o) return { text: '', kind: 'dim' };
  if (s.reflex?.fallback) return { text: `${o.label} isn’t set up here, so Molty is using ${REACTIONS.find((c) => c.id === s.reflex?.engine)?.label ?? 'nothing'} instead.`, kind: 'warn' };
  return { text: o.hint!, kind: 'dim' };
}

/** Setup only offers what works here; the Voice page shows the rest with how to get it. */
const ready = <T extends string>(o: Choice<T>[]) => o.filter((c) => !c.need);
export const voiceOptions = (s: VoiceStatus) => ready(allVoices(s));
export const gapOptions = (s: VoiceStatus) => ready(allGaps(s));
export const reflexOptions = (s: VoiceStatus) => ready(allReactions(s));

const REPO = 'https://github.com/apresmoi/molty.uno/blob/main';
const BOX_DOCS = `${REPO}/services/molty-voice/README.md`;
const RELAY_DOCS = `${REPO}/skills/molty/references/self-hosting.md`;

export interface Step { text: string; code?: string; href?: string }

const ownRelay = (who: string, secret: string, where: string, href: string): { title: string; steps: Step[] } => ({
  title: `To use ${who} you need your own relay with your own key. This relay doesn’t have one, and it never stores yours.`,
  steps: [
    { text: `Get a key from ${where}`, href },
    { text: 'Deploy your own relay', href: RELAY_DOCS },
    { text: 'Give it the key', code: `npx wrangler secret put ${secret}` },
  ],
});

/** Exactly what a choice needs, with this Molty's own pairing command. */
export function setupSteps(need: string, t: VoiceTarget | null): { title: string; steps: Step[] } {
  const box: Step[] = [
    { text: 'Install it on a computer with a GPU', href: BOX_DOCS },
    { text: 'Pair it, then tap Molty when it shows the code. This command changes every 15 minutes', code: t ? pairCommand(t) : undefined },
  ];
  if (need === 'opener') return { title: 'First words need a voice box running a small local model.', steps: [...box, { text: 'Get the model', code: 'ollama pull gemma3:4b' }, { text: 'Run the box with it', code: 'molty-voice run --opener' }] };
  if (need === 'local') return { title: 'This needs a voice box running a small reaction model.', steps: [...box, { text: 'Run the box with it', code: 'molty-voice run --reflex' }] };
  if (need === 'jev') return ownRelay('Jev', 'TYPESAFE_API_KEY', 'TypeSafe', 'https://docs.typesafe.ai');
  if (need === 'openai') return ownRelay('OpenAI', 'OPENAI_API_KEY', 'OpenAI', 'https://platform.openai.com/api-keys');
  if (need === 'cloud') return { title: 'This relay has no cloud speech. Add a voice box, or deploy a relay with a speech provider.', steps: [{ text: 'Deploy your own relay', href: RELAY_DOCS }] };
  return { title: 'A voice box runs Molty’s speech on your own computer, so nothing goes to a cloud voice.', steps: box };
}
