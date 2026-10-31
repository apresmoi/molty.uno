// How this Molty's human talks, and what the relay derived from it (docs/AGENT-LINK.md
// "Calibration"): the relay's owner API GET|DELETE <relay>/v1/devices/<id>/speech, authenticated
// with the device's CURRENT link code (header X-Molty-Link-Code), like the Voice card and the
// activity log. Pure: the activity page's "speech habits" card renders what these return.

export interface SpeechStats {
  turns: { sampled: number; total: number; needed: number };
  words: { median: number | null; p25: number | null; p75: number | null };
  seconds: { median: number | null; p25: number | null; p75: number | null };
  wpm: number | null;
  pauses: { per_turn: number | null; max_ms: number | null };
  policy: { source: 'defaults' | 'calibrated'; lead_ms: number; every_words: number; every_ms: number; max_per_turn: number };
  drafts: {
    made: number; used: number; misfires: number; dropped: Record<string, number>;
    use_rate: number | null; misfire_rate: number | null; threshold: number; threshold_bounds: [number, number];
  };
  partials: number;
  reacts: number;
  updated: number | null;
}

export class SpeechApiError extends Error {
  constructor(public code: string, message?: string) { super(message || code); }
}

export interface SpeechTarget { relay: string; deviceId: string; linkCode: string }

async function call(t: SpeechTarget, method: 'GET' | 'DELETE', fetchImpl: typeof fetch, timeoutMs = 8000): Promise<SpeechStats> {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    let res: Response;
    try {
      res = await fetchImpl(`${t.relay.replace(/\/+$/, '')}/v1/devices/${encodeURIComponent(t.deviceId)}/speech`, {
        method, headers: { 'x-molty-link-code': t.linkCode }, signal: ctl.signal, cache: 'no-store',
      });
    } catch {
      throw new SpeechApiError('unreachable', 'can’t reach the relay');
    }
    let b: any;
    try { b = await res.json(); } catch { throw new SpeechApiError(res.status === 404 ? 'unsupported' : `http_${res.status}`); }
    // A relay from before the calibration answers this route not_found.
    if (!b || b.ok !== true) throw new SpeechApiError(b?.error === 'not_found' ? 'unsupported' : String(b?.error ?? `http_${res.status}`), b?.message);
    return b.result as SpeechStats;
  } finally { clearTimeout(timer); }
}

export const getSpeech = (t: SpeechTarget, f: typeof fetch = fetch) => call(t, 'GET', f);
export const resetSpeech = (t: SpeechTarget, f: typeof fetch = fetch) => call(t, 'DELETE', f);

const pct = (v: number | null) => (v === null ? '—' : `${Math.round(v * 100)} %`);
const sec = (ms: number) => `${(ms / 1000).toFixed(ms % 1000 ? 1 : 0)} s`;

/** The card's rows: [label, value], plain words for the owner. */
export function speechRows(s: SpeechStats): Array<[string, string]> {
  const rows: Array<[string, string]> = [];
  rows.push(['turns heard', s.turns.sampled < s.turns.needed ? `${s.turns.sampled} (learning: ${s.turns.needed} needed)` : String(s.turns.sampled)]);
  if (s.words.median !== null) rows.push(['a turn', `${s.words.median} words, ${s.seconds.median ?? '—'} s (usually ${s.words.p25}–${s.words.p75} words)`]);
  if (s.wpm !== null) rows.push(['speech rate', `${s.wpm} words / min`]);
  if (s.pauses.per_turn !== null) rows.push(['pauses', `${s.pauses.per_turn} per turn${s.pauses.max_ms ? `, longest ~${sec(s.pauses.max_ms)}` : ''}`]);
  const p = s.policy;
  rows.push(['partials', `${p.source === 'calibrated' ? 'tuned' : 'defaults'}: every ${p.every_words} words or ${sec(p.every_ms)}, none in the first ${sec(p.lead_ms)}`]);
  const d = s.drafts;
  const drops = Object.entries(d.dropped).filter(([, n]) => n > 0).map(([k, n]) => `${k.replace('_', ' ')} ${n}`).join(', ');
  rows.push(['openers', d.made ? `${d.used} played of ${d.made} drafted (${pct(d.use_rate)})${drops ? ` ⠂ dropped: ${drops}` : ''}${d.misfires ? ` ⠂ cut in: ${d.misfires}` : ''}` : 'none drafted yet']);
  rows.push(['late words allowed', `${d.threshold} (tunes itself within ${d.threshold_bounds[0]}–${d.threshold_bounds[1]})`]);
  if (s.reacts) rows.push(['reactions', String(s.reacts)]);
  return rows;
}
