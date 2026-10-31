// The owner's voice box (docs/AGENT-LINK.md "Voice box"): the relay's owner API
// GET|POST|DELETE <relay>/v1/devices/<id>/voice, authenticated with the device's CURRENT
// link code (header X-Molty-Link-Code) — the console reads it from Molty over the LAN
// (GET /api/link), the same "on Molty's network = owner" model as every local setting.
// Pure: the Voice card (ui/voice-card.ts) renders what these return.

export type VoiceMode = 'auto' | 'box' | 'cloud' | 'off';
export const VOICE_MODES: ReadonlyArray<{ id: VoiceMode; label: string; hint: string }> = [
  { id: 'auto', label: 'auto', hint: 'the voice box when it is on, else the cloud' },
  { id: 'box', label: 'box only', hint: 'the voice box or nothing' },
  { id: 'cloud', label: 'cloud', hint: 'the relay’s cloud voice' },
  { id: 'off', label: 'text only', hint: 'no voice: Molty shows what the agent says and doesn’t listen' },
];

/** Molty's reflex (docs/AGENT-LINK.md "Reflex"): who classifies what the human says while they talk. */
export type ReflexMode = 'off' | 'auto' | 'local' | 'jev' | 'openai' | 'keywords';
export type ReflexEngine = 'local' | 'jev' | 'openai' | 'keywords';
export const REFLEX_MODES: ReadonlyArray<{ id: ReflexMode; label: string; hint: string }> = [
  { id: 'off', label: 'off', hint: 'Molty only reacts when an agent asks it to' },
  { id: 'auto', label: 'auto', hint: 'the voice box’s classifier, else Jev with a key, else keywords' },
  { id: 'local', label: 'box', hint: 'a small emotion model on the voice box (no extra hop, free)' },
  { id: 'jev', label: 'Jev', hint: 'TypeSafe Jev (the relay’s TYPESAFE_API_KEY)' },
  { id: 'openai', label: 'OpenAI', hint: 'OpenAI Decisions, gpt-6-luna (the relay’s OPENAI_API_KEY)' },
  { id: 'keywords', label: 'keywords', hint: 'a quick word list on the relay, no key' },
];
/** How lively the reflex is while the human talks (the relay's REFLEX_LIVELINESS: cadence + gates). */
export type Liveliness = 'calm' | 'normal' | 'lively';
export const LIVELINESS: ReadonlyArray<{ id: Liveliness; label: string; hint: string }> = [
  { id: 'calm', label: 'calm', hint: 'only clear reactions, at most one every 3 s' },
  { id: 'normal', label: 'normal', hint: 'clear reactions in full, likely ones as a small gesture, every 2 s at most' },
  { id: 'lively', label: 'lively', hint: 'reacts sooner and more often (every 1.5 s at most): small gestures for anything plausible' },
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
  if (!s.box.paired) return { text: 'no voice box paired', kind: 'dim' };
  const name = s.box.name ?? 'voice box';
  if (!s.box.connected) return { text: `${name} ⠂ offline`, kind: 'warn' };
  const can = [s.box.stt && 'listens', s.box.tts && 'speaks'].filter(Boolean).join(' + ');
  return { text: `${name} ⠂ connected${can ? ` ⠂ ${can}` : ''}${s.box.version ? ` ⠂ v${s.box.version}` : ''}`, kind: 'ok' };
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

/** One answer of a setup question. */
export interface SetupOption<T extends string = string> { id: T; label: string; hint: string }
export type GapFill = 'off' | 'phrases' | 'local';

/** The setup's voice answers: only the ones this owner's tools can do. */
export function voiceOptions(s: VoiceStatus): SetupOption<VoiceMode>[] {
  return [
    s.cloud !== 'none' ? { id: 'cloud' as const, label: 'Cloud voice', hint: s.cloud } : null,
    s.box.paired ? { id: 'box' as const, label: 'My voice box', hint: s.box.name ?? 'voice box' } : null,
    { id: 'off' as const, label: 'Text only', hint: 'I show words and emote; no mic, no speaker' },
  ].filter((o) => o !== null);
}

/** What fills the wait until the agent speaks; a local model needs a box that offers one. */
export function gapOptions(s: VoiceStatus): SetupOption<GapFill>[] {
  return [
    s.first_words?.available ? { id: 'local' as const, label: 'First words', hint: 'a small model on your voice box starts the reply' } : null,
    { id: 'phrases' as const, label: 'Filler phrases', hint: 'a short “Hmm…” when the agent is slow' },
    { id: 'off' as const, label: 'Nothing', hint: 'animations only' },
  ].filter((o) => o !== null);
}

/** Who picks Molty's emotes while you talk: only the engines available now. */
export function reflexOptions(s: VoiceStatus): SetupOption<ReflexMode>[] {
  const has = s.reflex?.engines;
  return REFLEX_MODES.filter((m) => m.id !== 'auto' && (m.id === 'off' || m.id === 'keywords' || has?.[m.id]))
    .map((m) => ({ id: m.id, label: m.id === 'local' ? 'Voice box' : m.id === 'off' ? 'Off' : m.id === 'keywords' ? 'Keywords' : m.label, hint: m.hint }));
}
