// The turn trace (docs/AGENT-LINK.md "Turn trace"): what the device, the voice box and the relay
// saw of ONE listen, stored with its job in the activity log (`data.trace`, <= 4 KB), plus the
// later facts about that turn as `trace` entries joined by job id (webhook deliveries, the
// agent's reply with its self-reported client_timing). Pure: turns them into a timeline model
// (ms from the trace's t0, the relay clock when the listen began) that the Activity page draws.
// Every part is optional: old firmware or no voice box = a partial trace, never an error.
import type { ActivityEntry } from './activity';

export type Lane = 'device' | 'box' | 'relay' | 'agent';
export type Tone = 'info' | 'ok' | 'warn' | 'err';

export interface TraceEvent { at: number; lane: Lane; label: string; tone: Tone }
export interface TraceModel {
  trigger?: string;
  /** Where audio time 0 lands (ms from t0): the mic start plus the trimmed shake noise. */
  origin: number;
  /** One level per 100 ms of captured audio, in dBFS (lv byte / 2 - 100). */
  levels: Array<{ at: number; db: number }>;
  clips: Array<{ at: number; n: number }>;
  clipTotal: number;
  /** Speech-activity spans (the box's), ms from t0. */
  speech: Array<[number, number]>;
  words: Array<{ at: number; end?: number; text: string }>;
  /** The box's pause estimate (0..1) over time, and its threshold. */
  pause: Array<{ at: number; v: number }>;
  threshold?: number;
  /** Frames as they reached the relay: retries (rt) and lost ranges. */
  frames: Array<{ seq: number; at: number; rt: number; wq?: number }>;
  lost: Array<[number, number]>;
  rssi?: [number | null, number | null];
  events: TraceEvent[];
  /** Attention lines first ("lost 3.5 s of audio"), else one "clean" line. */
  summary: Array<{ text: string; tone: Tone }>;
  /** Earliest / latest time covered (ms from t0). */
  from: number;
  to: number;
  trimmed?: boolean;
}

const num = (v: unknown): number | undefined => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
const str = (v: unknown): string | undefined => (typeof v === 'string' && v ? v : undefined);
const obj = (v: unknown): Record<string, any> | undefined => (v && typeof v === 'object' && !Array.isArray(v) ? v as Record<string, any> : undefined);
const arr = (v: unknown): any[] => (Array.isArray(v) ? v : []);
const s = (ms: number) => (Math.abs(ms) >= 1000 ? `${(ms / 1000).toFixed(1)} s` : `${Math.round(ms)} ms`);

/** "a1b2…" → bytes (2 hex chars each); malformed pairs are skipped. */
export function hexBytes(hex: unknown): number[] {
  if (typeof hex !== 'string') return [];
  const out: number[] = [];
  for (let i = 0; i + 1 < hex.length; i += 2) {
    const b = parseInt(hex.slice(i, i + 2), 16);
    if (Number.isFinite(b)) out.push(b);
  }
  return out;
}

/** The device's level byte → dBFS (byte = 2·(dBFS + 100), 0..200). */
export const levelDb = (b: number) => Math.min(0, b / 2 - 100);

const RULES: Record<string, string> = {
  punct: 'the last word ended a sentence',
  hold: 'a mid-sentence pause held',
  unmute: 'the pause rule (no hold)',
};

/** A turn's trace (its job entry's data.trace) and its `trace` entries → the timeline model; null when there is no trace. */
export function traceModel(jobEntry: ActivityEntry | undefined, extra: ActivityEntry[] = []): TraceModel | null {
  const t = obj(jobEntry?.data?.trace);
  if (!t) return null;
  const t0 = num(t.t0);
  const dev = obj(t.dev) ?? {}, box = obj(t.box) ?? {}, relay = obj(t.relay) ?? {};
  const shake = obj(dev.shake);
  const set = num(dev.set) ?? 0;
  // the mic opened at the shake (a shake listen) or when the device took the listen
  const micStart = shake && num(shake.at) !== undefined ? num(shake.at)! : num(relay.cmd) ?? 0;
  const origin = micStart + set;
  const events: TraceEvent[] = [];
  const ev = (at: number | undefined, lane: Lane, label: string, tone: Tone = 'info') => { if (at !== undefined) events.push({ at, lane, label, tone }); };
  const summary: Array<{ text: string; tone: Tone }> = [];

  // ── device ──
  if (shake) {
    const g = num(shake.peak_g);
    ev(num(shake.at), 'device', `shake${g !== undefined ? ` ${g.toFixed(1)} g` : ''}${num(shake.rev) !== undefined ? `, ${shake.rev} reversals` : ''}`, g !== undefined && g >= 4 ? 'warn' : 'info');
  }
  ev(micStart, 'device', t.trigger === 'shake' ? 'mic open (on the shake)' : 'mic open');
  if (set > 0) {
    const capped = num(dev.setc) === 1;
    ev(origin, 'device', `shake noise trimmed: ${s(set)}${capped ? ' (settle cap)' : ''}`, capped ? 'warn' : 'info');
    summary.push({ text: `shake noise trimmed ${s(set)}${capped ? ' (still moving at the cap)' : ''}`, tone: capped ? 'warn' : 'info' });
  }
  // the relay's level strip starts at lv0 (the first frame's ct, ms from the mic start) with lv_ms per byte
  // (100, more once the trace was trimmed); clip indexes are 100 ms chunks from the mic start
  const lv0 = num(dev.lv0) ?? set, lvMs = num(dev.lv_ms) ?? 100;
  const levels = hexBytes(dev.lv).map((b, i) => ({ at: micStart + lv0 + i * lvMs, db: levelDb(b) }));
  const clips = arr(dev.clip).filter((c) => Array.isArray(c) && num(c[0]) !== undefined && num(c[1]) !== undefined)
    .map((c) => ({ at: micStart + c[0] * 100, n: c[1] as number }));
  const clipTotal = num(dev.clip_n) ?? clips.reduce((a, c) => a + c.n, 0);
  if (clipTotal > 0) summary.push({ text: `${clipTotal} sample${clipTotal === 1 ? '' : 's'} clipped${num(dev.gain) !== undefined ? ` (mic gain ${dev.gain})` : ''}`, tone: 'warn' });
  const frames = arr(dev.frames).filter((f) => Array.isArray(f) && num(f[0]) !== undefined && num(f[2]) !== undefined)
    .map((f) => ({ seq: f[0] as number, at: f[2] as number, rt: num(f[3]) ?? 0, ...(num(f[4]) !== undefined ? { wq: f[4] as number } : {}) }));
  for (const f of frames) if (f.rt > 0) ev(f.at, 'device', `frame ${f.seq} resent after ${f.rt} failed send${f.rt === 1 ? '' : 's'}${f.wq !== undefined ? `, waited ${s(f.wq)}` : ''}`, 'warn');
  const lost = arr(dev.lost).filter((r) => Array.isArray(r) && num(r[0]) !== undefined).map((r) => [r[0], num(r[1]) ?? r[0]] as [number, number]);
  for (const [a, b] of lost) ev(frames.find((f) => f.seq > b)?.at ?? num(relay.end), 'relay', `frames ${a === b ? a : `${a}–${b}`} never arrived`, 'err');
  const fail = num(dev.fail) ?? 0, resent = num(dev.resent) ?? frames.filter((f) => f.rt > 0).length;
  let lostMs = num(dev.lost_ms) ?? 0;
  const wall = num(dev.wall), cap = num(dev.cap);
  // the mic was open longer than the audio it captured (a stalled capture): lost too
  const gap = wall !== undefined && cap !== undefined ? wall - set - cap : 0;
  if (gap > 300) lostMs = Math.max(lostMs, gap);
  if (lostMs > 0) summary.push({ text: `lost ${s(lostMs)} of audio${wall !== undefined && cap !== undefined ? ` (captured ${s(cap)} in ${s(wall)})` : ''}`, tone: 'err' });
  else if (lost.length) summary.push({ text: `lost ${lost.length} frame range${lost.length === 1 ? '' : 's'}`, tone: 'err' });
  if (fail > 0) summary.push({ text: `${fail} failed send${fail === 1 ? '' : 's'}, ${resent} frame${resent === 1 ? '' : 's'} resent`, tone: 'warn' });
  const late = num(dev.late) ?? 0;
  if (late > 0) summary.push({ text: `${late} frame${late === 1 ? '' : 's'} came after the end (dropped)`, tone: 'warn' });
  const rssiArr = arr(dev.rssi);
  const rssi: [number | null, number | null] | undefined = rssiArr.length ? [num(rssiArr[0]) ?? null, num(rssiArr[1]) ?? null] : undefined;
  const worst = rssi ? Math.min(...rssi.filter((v): v is number => v !== null)) : Infinity;
  if (worst <= -75) summary.push({ text: `weak Wi-Fi (${worst} dBm)`, tone: 'warn' });

  // ── voice box ──
  const sec = (v: unknown) => (num(v) !== undefined ? origin + (v as number) * 1000 : undefined);
  const speech = arr(box.act).filter((a) => Array.isArray(a) && num(a[0]) !== undefined && num(a[1]) !== undefined)
    .map((a) => [sec(a[0])!, sec(a[1])!] as [number, number]);
  const words = arr(box.words).filter((w) => Array.isArray(w) && typeof w[0] === 'string' && num(w[1]) !== undefined)
    .map((w) => ({ at: sec(w[1])!, ...(num(w[2]) !== undefined ? { end: sec(w[2])! } : {}), text: w[0] as string }));
  // the box records the pause EMA from its FIRST model step (skipped steps included: molty-voice trace.py)
  const every = Math.max(1, num(box.pause_every) ?? 1), step = num(box.step_ms) ?? 80;
  const pause = hexBytes(box.pause).map((b, i) => ({ at: origin + i * every * step, v: b / 255 }));
  if (words.length) ev(words[0].at, 'box', `first word “${words[0].text}”`);
  const end = obj(box.end);
  if (end) {
    const rule = str(end.rule);
    const held = num(end.held_s);
    ev(sec(end.at), 'box', `end of turn: ${rule ? RULES[rule] ?? rule : 'decided'}${str(end.word) ? ` (last word “${end.word}”)` : ''}${held ? `, held ${held.toFixed(1)} s` : ''}`, 'ok');
  }
  const ns = obj(box.ns);
  if (ns) ev(sec(ns.at), 'box', `no speech: ${str(ns.why) ?? `quiet ${num(ns.quiet_s) ?? '?'} s`}`);

  // ── relay ──
  // every relay.* time is ms from t0 already (stt is a duration: end -> transcript)
  ev(num(relay.first_frame), 'relay', 'first audio at the relay');
  ev(num(relay.turn_end), 'relay', 'listen stopped (turn end)');
  const endAt = num(relay.end) ?? num(relay.turn_end);
  ev(endAt, 'relay', `listen ended${str(dev.end) ? `: ${dev.end}` : ''}${num(dev.speech_ms) !== undefined ? `, ${s(dev.speech_ms)} of speech` : ''}`);
  if (num(relay.stt) !== undefined) ev((endAt ?? 0) + relay.stt, 'relay', `transcript ready: ${s(relay.stt)} after the end${num(box.final_ms) !== undefined ? ` (box ${s(box.final_ms)})` : ''}${str(relay.voice) ? ` ⠂ ${relay.voice === 'box' ? 'voice box' : relay.voice}` : ''}`);
  if (str(relay.fallback)) summary.push({ text: `voice box fell back to the cloud: ${relay.fallback}`, tone: 'warn' });
  // while the human spoke (docs/AGENT-LINK.md "While the human speaks"): counts only, never words
  for (const p of Array.isArray(relay.partials) ? relay.partials : []) if (Array.isArray(p)) ev(num(p[1]), 'relay', `partial ${p[0]} to the agents (${p[2]} words so far)`);
  for (const d of Array.isArray(relay.drafts) ? relay.drafts : []) if (Array.isArray(d)) ev(num(d[1]), 'agent', `an opener drafted on partial ${d[0]}`);
  for (const r of Array.isArray(relay.reacts) ? relay.reacts : []) if (Array.isArray(r)) ev(num(r[1]), 'agent', `react ${r[0]}${r[2] === 'ok' ? '' : ` → ${r[2]}`}`, r[2] === 'ok' ? 'ok' : 'warn');
  // Molty's reflex (docs/AGENT-LINK.md "Reflex"): one line per decision; in compare mode every engine's
  for (const r of Array.isArray(relay.reflex) ? relay.reflex : []) {
    const x = obj(r);
    if (!x) continue;
    ev(num(x.at), 'relay', reflexLabel(x), x.fired === 'ok' ? 'ok' : x.err ? 'warn' : 'info');
  }
  const rcfg = obj(relay.reflex_cfg);
  if (rcfg && str(rcfg.fb)) summary.push({ text: `reflex: ${rcfg.mode} unavailable (${rcfg.fb}), keywords instead`, tone: 'warn' });
  const draft = obj(relay.draft);
  if (draft && num(draft.used) !== undefined) ev(num(draft.opener_at), 'relay', `drafted opener played (partial ${draft.used}, ${num(draft.new_words) ?? 0} words after it)`, 'ok');
  if (draft && Array.isArray(draft.dropped) && draft.dropped.length) summary.push({ text: `draft${draft.dropped.length > 1 ? 's' : ''} dropped: ${draft.dropped.join(', ')}`, tone: 'warn' });
  const utter = num(relay.utterance);
  ev(utter, 'relay', 'utterance sent to the agents');

  // ── later: webhook deliveries and the agent's reply ──
  for (const e of extra) {
    if (e.kind !== 'trace') continue;
    const d = e.data ?? {};
    const who = e.agent ?? 'agent';
    if (e.ev === 'webhook') {
      const at = t0 !== undefined ? e.ts - t0 : undefined;
      const status = num(d.status);
      const bad = !!d.error || status === undefined || status < 200 || status > 299;
      ev(at, 'agent', `webhook → ${who}: ${status ?? d.error ?? 'failed'}${num(d.ms) !== undefined ? ` in ${s(d.ms)}` : ''}${num(d.attempt) && d.attempt > 1 ? ` (attempt ${d.attempt})` : ''}`, bad ? 'err' : 'ok');
      if (bad) summary.push({ text: `webhook to ${who} failed: ${d.error ?? status ?? 'unknown'}`, tone: 'err' });
    } else if (e.ev === 'reply') {
      const rep = num(d.reply_ms), first = num(d.first_audio_ms);
      const at = utter !== undefined && rep !== undefined ? utter + rep : t0 !== undefined ? e.ts - t0 : undefined;
      ev(at, 'agent', `${who} answered (${str(d.op) ?? 'reply'}) ${rep !== undefined ? `${s(rep)} after the utterance` : ''}`.trim(), 'ok');
      if (utter !== undefined && first !== undefined) ev(utter + first, 'agent', `first audio of the answer: ${s(first)} after the utterance`, 'ok');
      const ct = obj(d.client_timing);
      if (ct && Object.keys(ct).length) ev(at, 'agent', `${who} reports: ${clientTimingLine(ct)}`);
    }
  }
  if (t.trimmed) summary.push({ text: 'trace trimmed to fit (4 KB)', tone: 'info' });
  if (!summary.some((x) => x.tone === 'warn' || x.tone === 'err')) summary.unshift({ text: 'clean turn: nothing lost, nothing clipped', tone: 'ok' });
  else summary.sort((a, b) => rank(a.tone) - rank(b.tone));

  events.sort((a, b) => a.at - b.at);
  const times = [...events.map((e) => e.at), ...levels.map((l) => l.at + 100), ...speech.flat(), ...frames.map((f) => f.at), ...pause.map((p) => p.at)];
  const from = times.length ? Math.min(0, ...times) : 0;
  const to = times.length ? Math.max(...times, from + 1000) : 1000;
  return {
    ...(str(t.trigger) ? { trigger: t.trigger } : {}),
    origin, levels, clips, clipTotal, speech, words, pause,
    ...(num(box.thr) !== undefined ? { threshold: box.thr } : {}),
    frames, lost, ...(rssi ? { rssi } : {}), events, summary, from, to,
    ...(t.trimmed ? { trimmed: true } : {}),
  };
}

const rank = (t: Tone) => (t === 'err' ? 0 : t === 'warn' ? 1 : t === 'info' ? 2 : 3);

/** {received_ms:120, model_ms:900} → "received 120 ms ⠂ model 900 ms". */
export function clientTimingLine(ct: Record<string, unknown>): string {
  return Object.entries(ct).filter(([, v]) => num(v) !== undefined)
    .map(([k, v]) => `${k.replace(/_ms$/, '').replace(/_/g, ' ')} ${s(v as number)}`).join(' ⠂ ');
}

/** "-140 ms" / "+4.7 s" from t0, for the event list. */
export const fmtAt = (ms: number) => `${ms < 0 ? '−' : '+'}${s(Math.abs(ms))}`;

/** One reflex decision as a timeline line: engine, pick (+ category), latency, and what became of it. */
export function reflexLabel(x: Record<string, any>): string {
  const conf = typeof x.c === 'number' && x.e !== 'keywords' ? ` ${Math.round(x.c * 100)}%` : '';
  const raw = x.raw ? ` (${x.raw} below the gate)` : '';
  const cat = typeof x.cat === 'string' ? ` ⠂ ${x.cat}` : '';
  const head = x.err ? `reflex ${x.e}: ${x.err}${x.late ? ' (late)' : ''}` : `reflex ${x.e}: ${x.pick}${conf}${raw}${cat}${x.late ? ' (late)' : ''}`;
  const size = `${typeof x.ms === 'number' ? ` ⠂ ${Math.round(x.ms)} ms` : ''}${typeof x.tk === 'number' ? ` ⠂ ${x.tk} tok` : ''}`;
  const fired = x.fired === undefined ? '' : x.fired === 'ok' ? ' → shown' : x.fired === 'none' ? '' : ` → not shown: ${x.fired}`;
  const fb = x.fb ? ` (fallback: ${x.fb})` : '';
  return `${head}${size}${fired}${fb} (${x.n} words)`;
}
