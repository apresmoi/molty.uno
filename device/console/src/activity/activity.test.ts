import { describe, expect, it } from 'vitest';
import {
  ActivityApiError, agentsOf, entryLine, filterGroups, getActivity, groupEntries, isError, mergeEntries, setTranscripts, timingRows, transcript, visibleRange, voiceLine,
  type ActivityEntry,
} from './activity';

const T = { relay: 'https://relay.example/', deviceId: 'molty-k7m2w9c4ya', linkCode: 'K7QX2MPA' };
const J1 = 'molty-k7m2w9c4ya.j_AAAAAAAAAAAAAAAA';
const J2 = 'molty-k7m2w9c4ya.j_BBBBBBBBBBBBBBBB';

function fakeFetch(reply: { status?: number; body: unknown } | Error) {
  const calls: Array<{ url: string; init: RequestInit }> = [];
  const f = (async (url: string, init: RequestInit) => {
    calls.push({ url, init });
    if (reply instanceof Error) throw reply;
    return new Response(JSON.stringify(reply.body), { status: reply.status ?? 200 });
  }) as unknown as typeof fetch;
  return { f, calls };
}

const E: ActivityEntry[] = [
  { id: 'a1', ts: 1000, kind: 'pair', ev: 'approved', agent: 'Claude' },
  { id: 'a3', ts: 2050, kind: 'device_log', job: J1, data: { text: 'EVT audio.say start ms=1200', level: 'EVT', boot: 7, seq: 10 } },
  { id: 'a4', ts: 5000, kind: 'job', job: J1, agent: 'Claude', op: 'talk', ok: true, ms: 3000, data: { status: 'done', heard: 'hi', said: 'hello', timings: { queue: 2, say: { tts_first_byte: 240, voice: 'box' }, total: 3000 } } },
  { id: 'a5', ts: 2000, kind: 'call', job: J1, agent: 'Claude', op: 'talk', ok: true, ms: 3010 },
  { id: 'a6', ts: 6000, kind: 'device', ev: 'disconnect', data: { code: 1006 } },
  { id: 'a7', ts: 7000, kind: 'job', job: J2, agent: 'Grokbot', op: 'talk', ok: false, error: 'voice_box_offline', ms: 12, data: { status: 'failed', error: 'voice_box_offline' } },
  { id: 'a8', ts: 6990, kind: 'call', job: J2, agent: 'Grokbot', op: 'talk', ok: false, error: 'voice_box_offline', ms: 15 },
  { id: 'a9', ts: 7005, kind: 'device_log', job: J2, data: { text: 'E (7001) agent_link: listen refused', level: 'E', boot: 7, seq: 11 } },
];

describe('activity owner API', () => {
  it('reads with the link code header and the query, never the code in the URL', async () => {
    const { f, calls } = fakeFetch({ body: { ok: true, result: { entries: E, next: 'a9', more: false, transcripts: true } } });
    const p = await getActivity(T, { since: 'a3', limit: 50, agent: 'Claude', kind: 'job', errors: true }, f);
    expect(p.next).toBe('a9');
    expect(p.entries).toHaveLength(E.length);
    expect(calls[0].url).toBe('https://relay.example/v1/devices/molty-k7m2w9c4ya/activity?since=a3&limit=50&agent=Claude&kind=job&errors=1');
    expect(calls[0].init.method).toBe('GET');
    expect((calls[0].init.headers as Record<string, string>)['x-molty-link-code']).toBe('K7QX2MPA');
    expect(calls[0].url).not.toContain('K7QX2MPA');
  });
  it('sets the transcripts setting with a POST', async () => {
    const { f, calls } = fakeFetch({ body: { ok: true, result: { entries: [], next: 'a0', more: false, transcripts: false } } });
    expect((await setTranscripts(T, false, f)).transcripts).toBe(false);
    expect(calls[0].init.method).toBe('POST');
    expect(JSON.parse(String(calls[0].init.body))).toEqual({ transcripts: false });
  });
  it('maps failures: no route = unsupported, unreachable, others keep their code', async () => {
    await expect(getActivity(T, {}, fakeFetch({ status: 404, body: { ok: false, error: 'not_found' } }).f)).rejects.toMatchObject({ code: 'unsupported' });
    await expect(getActivity(T, {}, fakeFetch({ status: 401, body: { ok: false, error: 'unauthorized' } }).f)).rejects.toMatchObject({ code: 'unauthorized' });
    await expect(getActivity(T, {}, fakeFetch(new TypeError('net')).f)).rejects.toMatchObject({ code: 'unreachable' });
    await expect(getActivity(T, {}, fakeFetch(new TypeError('net')).f)).rejects.toBeInstanceOf(ActivityApiError);
  });
});

describe('merge', () => {
  it('dedupes by id, sorts by time then id, keeps the newest under the cap', () => {
    const m = mergeEntries(E.slice(0, 4), E.slice(2));
    expect(m.map((e) => e.id)).toEqual(['a1', 'a5', 'a3', 'a4', 'a6', 'a8', 'a7', 'a9']);
    expect(mergeEntries(E, [{ ...E[0], ev: 'revoked' }]).find((e) => e.id === 'a1')?.ev).toBe('revoked');
    expect(mergeEntries([], E, 3).map((e) => e.id)).toEqual(['a8', 'a7', 'a9']);
  });
});

describe('grouping into turns', () => {
  const g = groupEntries(E);
  it('gathers a job’s call, job end and device lines into one group', () => {
    expect(g.map((x) => x.key)).toEqual(['a1', J1, 'a6', J2]);
    const t = g[1];
    expect(t.call?.id).toBe('a5');
    expect(t.jobEntry?.id).toBe('a4');
    expect(t.lines.map((l) => l.id)).toEqual(['a3']);
    expect(t).toMatchObject({ agent: 'Claude', op: 'talk', outcome: 'done', ms: 3000, ts: 2000 });
  });
  it('a failed job is a failed group with its error', () => {
    expect(g[3]).toMatchObject({ agent: 'Grokbot', outcome: 'failed', error: 'voice_box_offline' });
  });
  it('a call with no job entry yet is pending; a failed call alone is failed', () => {
    expect(groupEntries([E[3]])[0].outcome).toBe('pending');
    expect(groupEntries([E[6]])[0].outcome).toBe('failed');
  });
  it('filters by agent, kind and errors only', () => {
    expect(filterGroups(g, { agent: 'Grokbot' }).map((x) => x.key)).toEqual([J2]);
    expect(filterGroups(g, { kind: 'device' }).map((x) => x.key)).toEqual(['a6']);
    expect(filterGroups(g, { errors: true }).map((x) => x.key)).toEqual([J2]);
    expect(filterGroups(g, {})).toHaveLength(4);
  });
  it('lists agents and tells errors', () => {
    expect(agentsOf(E)).toEqual(['Claude', 'Grokbot']);
    expect(isError(E[7])).toBe(true);
    expect(isError(E[1])).toBe(false);
    expect(isError({ id: 'a', ts: 0, kind: 'device', ev: 'refused' })).toBe(true);
  });
});

describe('turn details', () => {
  it('flattens nested stage timings', () => {
    expect(timingRows(E[2].data!.timings)).toEqual([['queue', '2 ms'], ['say · tts_first_byte', '240 ms'], ['say · voice', 'box'], ['total', '3.0 s']]);
  });
  it('names the voice provider and a fallback', () => {
    expect(voiceLine(E[2])).toBe('speaks with voice box');
    expect(voiceLine({ id: 'x', ts: 0, kind: 'job', op: 'talk', data: { voice: { tts: 'box', stt: 'workers-ai', fallback: 'stt_timeout' } } }))
      .toBe('speaks with voice box ⠂ listens with cloud (workers-ai) ⠂ fell back to the cloud: stt_timeout');
  });
  it('shows heard/said, or redacted', () => {
    expect(transcript(E[2], 'heard')).toBe('hi');
    expect(transcript({ id: 'x', ts: 0, kind: 'job', redacted: true, data: { heard: '[redacted]' } }, 'heard')).toBe('redacted');
    expect(transcript({ id: 'x', ts: 0, kind: 'job', data: {} }, 'said')).toBeNull();
  });
  it('summarises standalone entries', () => {
    expect(entryLine(E[4])).toBe('molty disconnected ⠂ close 1006');
    expect(entryLine(E[0])).toBe('paired');
  });
});

describe('search and the visible window', () => {
  const gs = groupEntries([
    { id: 'a1', ts: 1, kind: 'job', job: 'j1', op: 'talk', data: { heard: 'Green tea please' } },
    { id: 'a2', ts: 2, kind: 'device_log', job: 'j2', data: { text: 'wifi lost' } },
  ] as never);
  it('needs every word, anywhere in the turn, in any case', () => {
    expect(filterGroups(gs, { q: 'GREEN please' }).map((g) => g.job)).toEqual(['j1']);
    expect(filterGroups(gs, { q: 'green wifi' })).toEqual([]);
    expect(filterGroups(gs, { q: '  ' })).toHaveLength(2);
  });
  it('draws only what is on screen', () => {
    expect(visibleRange(0, 240, 24, 3000, 0)).toEqual([0, 10]);
    expect(visibleRange(2400, 240, 24, 3000, 5)).toEqual([95, 115]);
    expect(visibleRange(71760, 240, 24, 3000, 5)).toEqual([2985, 3000]);
  });
});
