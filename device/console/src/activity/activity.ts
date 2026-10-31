// The activity log (docs/AGENT-LINK.md "Activity log"): everything that happened on this Molty,
// with which agent did it — agent calls, jobs (stage timings, voice, heard/said), device
// events and the device's own log lines, joined by job id. The relay's owner API
// GET|POST <relay>/v1/devices/<id>/activity, authenticated with the device's CURRENT link
// code (header X-Molty-Link-Code), like the Voice card (voice/voice.ts).
// Pure: the Activity page (pages/activity.ts) renders what these return.

export type ActivityKind = 'call' | 'job' | 'device' | 'device_log' | 'pair' | 'voice_box' | 'settings' | 'trace';
export const ACTIVITY_KINDS: ReadonlyArray<{ id: ActivityKind; label: string }> = [
  { id: 'call', label: 'agent calls' },
  { id: 'job', label: 'turns' },
  { id: 'device', label: 'device events' },
  { id: 'device_log', label: 'device log' },
  { id: 'pair', label: 'pairing' },
  { id: 'voice_box', label: 'voice box' },
  { id: 'settings', label: 'settings' },
  { id: 'trace', label: 'turn traces' },
];

export interface ActivityEntry {
  id: string;
  ts: number;
  kind: ActivityKind | (string & {});
  ev?: string;
  agent?: string;
  job?: string;
  op?: string;
  args?: Record<string, unknown>;
  ok?: boolean;
  error?: string;
  message?: string;
  ms?: number;
  data?: Record<string, any>;
  redacted?: true;
}

export interface ActivityPage {
  entries: ActivityEntry[];
  next: string;
  more: boolean;
  transcripts: boolean;
  /** "keep audio for debugging" (owner setting); undefined: this relay has no such setting. */
  keep_audio?: boolean;
  audio?: { turns: number; max_s: number; max_age_h: number };
  limits?: { max_age_h: number; max_entries: number };
}

export interface ActivityQuery { since?: string; limit?: number; agent?: string; kind?: string; job?: string; errors?: boolean }

export class ActivityApiError extends Error {
  constructor(public code: string, message?: string) { super(message || code); }
}

export interface ActivityTarget { relay: string; deviceId: string; linkCode: string }

async function call(t: ActivityTarget, method: 'GET' | 'POST', q: ActivityQuery, body: unknown, fetchImpl: typeof fetch, timeoutMs = 8000): Promise<ActivityPage> {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  const params = new URLSearchParams();
  if (q.since) params.set('since', q.since);
  if (q.limit) params.set('limit', String(q.limit));
  if (q.agent) params.set('agent', q.agent);
  if (q.kind) params.set('kind', q.kind);
  if (q.job) params.set('job', q.job);
  if (q.errors) params.set('errors', '1');
  const qs = params.toString();
  try {
    let res: Response;
    try {
      res = await fetchImpl(`${t.relay.replace(/\/+$/, '')}/v1/devices/${encodeURIComponent(t.deviceId)}/activity${qs ? `?${qs}` : ''}`, {
        method,
        headers: { 'x-molty-link-code': t.linkCode, ...(body === undefined ? {} : { 'content-type': 'application/json' }) },
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: ctl.signal,
        cache: 'no-store',
      });
    } catch {
      throw new ActivityApiError('unreachable', 'can’t reach the relay');
    }
    let b: any;
    try { b = await res.json(); } catch { throw new ActivityApiError(res.status === 404 ? 'unsupported' : `http_${res.status}`); }
    if (!b || b.ok !== true) {
      // A relay from before the activity log answers this route not_found.
      throw new ActivityApiError(b?.error === 'not_found' ? 'unsupported' : String(b?.error ?? `http_${res.status}`), b?.message);
    }
    const r = b.result ?? {};
    return { entries: Array.isArray(r.entries) ? r.entries : [], next: String(r.next ?? q.since ?? ''), more: !!r.more, transcripts: r.transcripts !== false,
      ...(typeof r.keep_audio === 'boolean' ? { keep_audio: r.keep_audio } : {}), ...(r.audio && typeof r.audio === 'object' ? { audio: r.audio } : {}), limits: r.limits };
  } finally { clearTimeout(timer); }
}

export const getActivity = (t: ActivityTarget, q: ActivityQuery = {}, f: typeof fetch = fetch) => call(t, 'GET', q, undefined, f);
export const setTranscripts = (t: ActivityTarget, on: boolean, f: typeof fetch = fetch) => call(t, 'POST', {}, { transcripts: on }, f);
export const setKeepAudio = (t: ActivityTarget, on: boolean, f: typeof fetch = fetch) => call(t, 'POST', {}, { keep_audio: on }, f);

/**
 * A kept turn's audio (owner only): GET <relay>/v1/devices/<id>/activity/audio/<job> with the
 * link code header → the WAV blob. Errors: not_found (expired / never kept), unauthorized, unreachable.
 */
export async function getAudio(t: ActivityTarget, job: string, fetchImpl: typeof fetch = fetch, timeoutMs = 20000): Promise<Blob> {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    let res: Response;
    try {
      res = await fetchImpl(`${t.relay.replace(/\/+$/, '')}/v1/devices/${encodeURIComponent(t.deviceId)}/activity/audio/${encodeURIComponent(job)}`, {
        method: 'GET', headers: { 'x-molty-link-code': t.linkCode }, signal: ctl.signal, cache: 'no-store',
      });
    } catch {
      throw new ActivityApiError('unreachable', 'can’t reach the relay');
    }
    if (!res.ok) {
      let b: any = null;
      try { b = await res.json(); } catch { /* not JSON */ }
      throw new ActivityApiError(String(b?.error ?? (res.status === 404 ? 'not_found' : `http_${res.status}`)), b?.message);
    }
    return await res.blob();
  } finally { clearTimeout(timer); }
}

/** File name for a turn's audio: molty-<job tag>.wav. */
export const audioFileName = (job: string) => `molty-${job.split('.').pop() ?? 'turn'}.wav`;

const seqOf = (id: string) => { const m = /^a(\d+)$/.exec(id); return m ? Number(m[1]) : NaN; };

/** Merges a polled page into what is shown: deduped by id, ts ascending (id breaks ties), at most `cap` (newest kept). */
export function mergeEntries(have: ActivityEntry[], add: ActivityEntry[], cap = 1000): ActivityEntry[] {
  const byId = new Map(have.map((e) => [e.id, e]));
  for (const e of add) if (e && typeof e.id === 'string') byId.set(e.id, e);
  const all = [...byId.values()].sort((a, b) => a.ts - b.ts || seqOf(a.id) - seqOf(b.id));
  return all.length > cap ? all.slice(all.length - cap) : all;
}

/** An entry that went wrong: a failed call / job, a refusal, an E-level device line. */
export function isError(e: ActivityEntry): boolean {
  if (e.ok === false || !!e.error) return true;
  if (e.kind === 'job' && (e.data?.status === 'failed')) return true;
  if (e.kind === 'device' && e.ev === 'refused') return true;
  if (e.kind === 'device_log' && e.data?.level === 'E') return true;
  return false;
}

export interface Filters { agent?: string; kind?: string; errors?: boolean; q?: string }

/** One turn (a job's call + job end + its device lines) or one standalone entry. */
export interface Group {
  key: string;
  job?: string;
  /** Earliest entry's time. */
  ts: number;
  agent?: string;
  /** The job kind (talk, say, …) or the entry's op / ev / kind. */
  op: string;
  /** done | failed | pending (a job group), ok | error (a single entry). */
  outcome: 'done' | 'failed' | 'pending' | 'ok' | 'error';
  error?: string;
  /** Job total (or call duration). */
  ms?: number;
  call?: ActivityEntry;
  jobEntry?: ActivityEntry;
  lines: ActivityEntry[];
  /** The turn's later trace facts (webhook deliveries, the agent's reply). */
  traces: ActivityEntry[];
  entries: ActivityEntry[];
}

/** Entries → groups, oldest first: everything with the same job id is one turn. */
export function groupEntries(entries: ActivityEntry[]): Group[] {
  const groups: Group[] = [];
  const byJob = new Map<string, Group>();
  for (const e of [...entries].sort((a, b) => a.ts - b.ts || seqOf(a.id) - seqOf(b.id))) {
    if (e.job) {
      let g = byJob.get(e.job);
      if (!g) {
        g = { key: e.job, job: e.job, ts: e.ts, op: 'job', outcome: 'pending', lines: [], traces: [], entries: [] };
        byJob.set(e.job, g);
        groups.push(g);
      }
      g.entries.push(e);
      g.ts = Math.min(g.ts, e.ts);
      if (e.kind === 'call') g.call = e;
      else if (e.kind === 'job') g.jobEntry = e;
      else if (e.kind === 'device_log') g.lines.push(e);
      else if (e.kind === 'trace') g.traces.push(e);
      continue;
    }
    groups.push({ key: e.id, ts: e.ts, agent: e.agent, op: e.op ?? e.ev ?? e.kind, outcome: isError(e) ? 'error' : 'ok', error: e.error, ms: e.ms, lines: e.kind === 'device_log' ? [e] : [], traces: e.kind === 'trace' ? [e] : [], entries: [e] });
  }
  for (const g of byJob.values()) {
    const j = g.jobEntry, c = g.call;
    // a shake's listen has no agent: the agents its trace entries name (webhooks, replies) are not its owner
    g.agent = j?.agent ?? c?.agent ?? g.entries.find((e) => e.agent && e.kind !== 'trace')?.agent;
    g.op = j?.op ?? c?.op ?? 'job';
    if (j) {
      const failed = j.ok === false || j.data?.status === 'failed' || !!j.error;
      g.outcome = failed ? 'failed' : 'done';
      g.error = j.error ?? j.data?.error;
      g.ms = j.ms ?? j.data?.ms;
    } else if (c && (c.ok === false || c.error)) {
      g.outcome = 'failed'; g.error = c.error; g.ms = c.ms;
    } else g.ms = c?.ms;
  }
  return groups.sort((a, b) => a.ts - b.ts);
}

/** Filters groups: agent (a group's agent), kind (any entry of that kind), errors only (any error in it). */
export function filterGroups(groups: Group[], f: Filters): Group[] {
  const words = (f.q ?? '').toLowerCase().split(/\s+/).filter(Boolean);
  return groups.filter((g) =>
    (!words.length || matches(g, words))
    && (!f.agent || g.agent === f.agent)
    && (!f.kind || g.entries.some((e) => e.kind === f.kind))
    && (!f.errors || g.outcome === 'failed' || g.outcome === 'error' || g.entries.some(isError)));
}

/** Every word appears somewhere in the turn: its line, the words said, its device log, any field. */
function matches(g: Group, words: string[]): boolean {
  const text = `${g.agent ?? 'molty'} ${g.op} ${entryLine(g.entries[0])} ${JSON.stringify(g.entries)}`.toLowerCase();
  return words.every((w) => text.includes(w));
}

/** The rows to draw for a scroll position: [first, end) of n fixed-height rows, plus a margin. */
export function visibleRange(scrollTop: number, height: number, rowH: number, n: number, margin = 10): [number, number] {
  const first = Math.max(0, Math.floor(scrollTop / rowH) - margin);
  return [first, Math.min(n, Math.ceil((scrollTop + height) / rowH) + margin)];
}

/** The agent names seen, sorted. */
export function agentsOf(entries: ActivityEntry[]): string[] {
  return [...new Set(entries.map((e) => e.agent).filter((a): a is string => !!a))].sort((a, b) => a.localeCompare(b));
}

const fmtMs = (ms: number) => (ms >= 1000 ? `${(ms / 1000).toFixed(ms >= 10000 ? 0 : 1)} s` : `${Math.round(ms)} ms`);
export { fmtMs };

/** Stage timings → rows ["say · tts_first_byte", "240 ms"], nested say/listen flattened one level. */
export function timingRows(t: Record<string, unknown> | undefined): Array<[string, string]> {
  if (!t) return [];
  const rows: Array<[string, string]> = [];
  for (const [k, v] of Object.entries(t)) {
    if (typeof v === 'number') rows.push([k, fmtMs(v)]);
    else if (typeof v === 'string') rows.push([k, v]);
    else if (v && typeof v === 'object') {
      for (const [k2, v2] of Object.entries(v as Record<string, unknown>)) {
        if (typeof v2 === 'number') rows.push([`${k} · ${k2}`, fmtMs(v2)]);
        else if (typeof v2 === 'string') rows.push([`${k} · ${k2}`, v2]);
      }
    }
  }
  return rows;
}

const providerName = (v: string) => (v === 'box' ? 'voice box' : `cloud (${v})`);

/** "speaks with voice box ⠂ listens with cloud (workers-ai) ⠂ fell back: box_timeout", or '' when unknown. */
export function voiceLine(j: ActivityEntry | undefined): string {
  const t = j?.data?.timings ?? {};
  const v = j?.data?.voice ?? {};
  const tts: string | undefined = v.tts ?? t.say?.voice ?? (j?.op === 'say' ? t.voice : undefined);
  const stt: string | undefined = v.stt ?? t.listen?.voice ?? (j?.op === 'listen' ? t.voice : undefined);
  const fb: string | undefined = v.fallback ?? t.listen?.voice_fallback ?? t.voice_fallback;
  const parts: string[] = [];
  if (tts) parts.push(`speaks with ${providerName(tts)}`);
  if (stt) parts.push(`listens with ${providerName(stt)}`);
  if (fb) parts.push(`fell back to the cloud: ${fb}`);
  return parts.join(' ⠂ ');
}

/** heard / said for display: the text, "redacted" when the relay removed it, or null when absent. */
export function transcript(j: ActivityEntry | undefined, field: 'heard' | 'said'): string | null {
  if (!j?.data || !(field in j.data)) return null;
  const v = j.data[field];
  if (j.redacted && (typeof v !== 'string' || /^\[?redacted/i.test(v) || v === '')) return 'redacted';
  return typeof v === 'string' ? v : null;
}

/** One-line summary of a standalone entry. */
export function entryLine(e: ActivityEntry): string {
  const d = e.data ?? {};
  switch (e.kind) {
    case 'device':
      if (e.ev === 'connect') return `molty connected${d.fw ? ` ⠂ fw ${d.fw}` : ''}${typeof d.rssi === 'number' ? ` ⠂ ${d.rssi} dBm` : ''}`;
      if (e.ev === 'disconnect') return `molty disconnected${d.code ? ` ⠂ close ${d.code}` : ''}${d.reason ? ` ${d.reason}` : ''}`;
      if (e.ev === 'refused') return `relay refused a connection${d.code ? ` ⠂ close ${d.code}` : ''}`;
      return `${e.ev ?? 'event'}${Object.keys(d).length ? ` ${JSON.stringify(d)}` : ''}`;
    case 'device_log': return String(d.text ?? '');
    // the agent's name is the row's own column: not repeated here
    case 'pair': return `${e.ev === 'approved' ? 'paired' : e.ev === 'revoked' ? 'unpaired' : `pairing ${e.ev ?? ''}`}${d.role === 'voice' ? ' (voice box)' : ''}`;
    case 'voice_box': return `voice box ${e.ev ?? ''}${d.name ? ` ⠂ ${d.name}` : ''}${d.code ? ` ⠂ close ${d.code}` : ''}`;
    case 'settings': return `settings: ${e.ev ?? ''} ${JSON.stringify(d)}`;
    case 'trace': return `${e.ev === 'webhook' ? 'webhook' : e.ev === 'reply' ? 'reply' : e.ev === 'filler' ? 'filler' : `trace ${e.ev ?? ''}`}${typeof d.status === 'number' ? ` ⠂ ${d.status}` : ''}${typeof d.ms === 'number' ? ` ⠂ ${fmtMs(d.ms)}` : ''}${d.error ? ` ⠂ ${d.error}` : ''}`;
    case 'call': return `${e.op ?? 'call'}${e.ok === false ? ` → ${e.error ?? 'error'}` : ''}${e.args && Object.keys(e.args).length ? ` ${JSON.stringify(e.args)}` : ''}`;
    default: return `${e.kind}${e.ev ? ` ${e.ev}` : ''}`;
  }
}
