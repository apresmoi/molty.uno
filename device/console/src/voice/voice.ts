// The owner's voice box (docs/AGENT-LINK.md "Voice box"): the relay's owner API
// GET|POST|DELETE <relay>/v1/devices/<id>/voice, authenticated with the device's CURRENT
// link code (header X-Molty-Link-Code) — the console reads it from Molty over the LAN
// (GET /api/link), the same "on Molty's network = owner" model as every local setting.
// Pure: the voice page and setup render what these return.

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

/** One status line for the box. */
export function boxLine(s: VoiceStatus): { text: string; kind: 'ok' | 'warn' | 'dim' } {
  if (!s.box.paired) return { text: 'No voice box paired.', kind: 'dim' };
  const name = s.box.name ?? 'voice box';
  if (!s.box.connected) return { text: `${name} is offline.`, kind: 'warn' };
  return { text: `${name} is connected.`, kind: 'ok' };
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

const ENGINE_NAME: Record<ReflexEngine, string> = { local: 'the voice box', jev: 'Jev', openai: 'OpenAI', keywords: 'keywords' };
const WHY: Record<string, string> = { no_key: 'no key on the relay', unavailable: 'no voice box classifier' };

/** "reacts with …" for the reflex, or '' when it is off / unknown. */
export function reflexLine(r: ReflexStatus | undefined): string {
  if (!r || r.mode === 'off' || !r.engine) return '';
  const why = r.fallback ? ` (${WHY[r.fallback] ?? r.fallback} for ${r.mode === 'local' ? 'the box' : r.mode === 'jev' ? 'Jev' : 'OpenAI'})` : '';
  return `reacts with ${ENGINE_NAME[r.engine]}${why}${r.compare ? ' ⠂ comparing every engine in the activity log' : ''}`;
}

export type GapFill = 'off' | 'phrases' | 'local';

/** Who speaks, as the owner chose it: with a box and a cloud, "auto" is the box with a safety net. */
export function voiceOptions(s: VoiceStatus): Choice<VoiceMode>[] {
  const cloud = s.cloud !== 'none', box = s.box.paired;
  return [
    box ? { id: cloud ? 'auto' as const : 'box' as const, label: 'My voice box', hint: `Speech runs on your own computer${s.box.name ? ` (${s.box.name})` : ''}.${cloud ? ' The cloud takes over if it goes offline.' : ''}` } : null,
    cloud ? { id: 'cloud' as const, label: 'Cloud voice', hint: 'Speech runs in the cloud. Nothing to install.' } : null,
    { id: 'off' as const, label: 'Text only', hint: 'Molty shows words and emotes. No microphone, no speaker.' },
  ].filter((o) => o !== null);
}

/** Which of voiceOptions the current mode is. */
export function voiceOn(s: VoiceStatus): VoiceMode {
  if (s.mode === 'off' || s.mode === 'cloud') return s.mode;
  if (!s.box.paired) return 'cloud';
  return s.cloud !== 'none' ? 'auto' : 'box';
}

/** What fills the wait until the agent speaks; a local model needs a box that offers one. */
export function gapOptions(s: VoiceStatus): Choice<GapFill>[] {
  return [
    s.first_words?.available ? { id: 'local' as const, label: 'First words', hint: 'A small model on your computer starts the sentence, and your agent finishes it.' } : null,
    { id: 'phrases' as const, label: 'A short phrase', hint: 'Molty says something like “Hmm…” if your agent is slow.' },
    { id: 'off' as const, label: 'Stay quiet', hint: 'Only the thinking animation.' },
  ].filter((o) => o !== null);
}

export const gapOn = (s: VoiceStatus, fillerMs: number | undefined): GapFill =>
  s.first_words?.mode === 'opener' ? 'local' : fillerMs === FILLER_OFF ? 'off' : 'phrases';

/** Saves a gap choice on both sides: the relay's first words, the device's filler (a tuned delay stays). */
export async function applyGap(t: VoiceTarget, id: GapFill, device: { fluid(o: { filler_ms: number }): Promise<unknown>; creatureState(): Promise<{ filler_ms?: number }> }) {
  const s = await setVoice(t, { first_words: id === 'local' ? 'opener' : 'off' });
  if (id === 'off') await device.fluid({ filler_ms: FILLER_OFF });
  else if (id === 'phrases' && !(await device.creatureState()).filler_ms) await device.fluid({ filler_ms: FILLER_DEFAULT_MS });
  return s;
}

const REACT: Partial<Record<ReflexMode, Choice<ReflexMode>>> = {
  off: { id: 'off', label: 'Stay still', hint: 'Molty only reacts when your agent asks it to.' },
  local: { id: 'local', label: 'Small model on my computer', hint: 'Free and private. Runs on your voice box.' },
  jev: { id: 'jev', label: 'Jev', hint: 'Fast reactions from a hosted TypeSafe model.' },
  openai: { id: 'openai', label: 'OpenAI', hint: 'Reactions from a hosted OpenAI model.' },
  keywords: { id: 'keywords', label: 'Word list', hint: 'Free and simple. Reacts to words it recognises.' },
};

/** Who picks Molty's reactions while you talk: only the engines available now. */
export function reflexOptions(s: VoiceStatus): Choice<ReflexMode>[] {
  const has = s.reflex?.engines;
  return (['off', 'local', 'jev', 'openai', 'keywords'] as const).filter((m) => m === 'off' || m === 'keywords' || has?.[m]).map((m) => REACT[m]!);
}

export const reflexOn = (r: ReflexStatus | undefined): ReflexMode | undefined => (r?.mode === 'auto' ? r.engine ?? undefined : r?.mode);
