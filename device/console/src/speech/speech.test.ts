import { describe, expect, it } from 'vitest';
import { SpeechApiError, getSpeech, resetSpeech, speechRows, type SpeechStats } from './speech';

const T = { relay: 'https://relay.example/', deviceId: 'molty-k7m2w9c4ya', linkCode: 'K7QX2MPA' };
const S: SpeechStats = {
  turns: { sampled: 12, total: 30, needed: 8 },
  words: { median: 14, p25: 8, p75: 22 },
  seconds: { median: 4.6, p25: 2.9, p75: 7.1 },
  wpm: 182,
  pauses: { per_turn: 1, max_ms: 900 },
  policy: { source: 'calibrated', lead_ms: 3000, every_words: 5, every_ms: 2000, max_per_turn: 8 },
  drafts: { made: 10, used: 6, misfires: 1, dropped: { new_words: 2, meaning: 1 }, use_rate: 0.7, misfire_rate: 0.14, threshold: 5, threshold_bounds: [1, 10] },
  partials: 20,
  reacts: 3,
  updated: 1,
};

function fakeFetch(reply: { status?: number; body: unknown } | Error) {
  const calls: Array<{ url: string; init: RequestInit }> = [];
  const f = (async (url: string, init: RequestInit) => {
    calls.push({ url, init });
    if (reply instanceof Error) throw reply;
    return new Response(JSON.stringify(reply.body), { status: reply.status ?? 200 });
  }) as unknown as typeof fetch;
  return { f, calls };
}

describe('speech habits (owner API)', () => {
  it('reads and resets with the link code', async () => {
    const { f, calls } = fakeFetch({ body: { ok: true, result: S } });
    expect(await getSpeech(T, f)).toEqual(S);
    await resetSpeech(T, f);
    expect(calls.map((c) => [c.url, c.init.method, (c.init.headers as Record<string, string>)['x-molty-link-code']])).toEqual([
      ['https://relay.example/v1/devices/molty-k7m2w9c4ya/speech', 'GET', 'K7QX2MPA'],
      ['https://relay.example/v1/devices/molty-k7m2w9c4ya/speech', 'DELETE', 'K7QX2MPA'],
    ]);
  });

  it('an older relay is unsupported; errors keep their code', async () => {
    await expect(getSpeech(T, fakeFetch({ status: 404, body: { ok: false, error: 'not_found' } }).f)).rejects.toMatchObject({ code: 'unsupported' });
    await expect(getSpeech(T, fakeFetch({ status: 401, body: { ok: false, error: 'unauthorized' } }).f)).rejects.toMatchObject({ code: 'unauthorized' });
    await expect(getSpeech(T, fakeFetch(new Error('net')).f)).rejects.toBeInstanceOf(SpeechApiError);
  });

  it('rows in plain words: learning until enough turns, tuned partials, the draft record', () => {
    const rows = Object.fromEntries(speechRows(S));
    expect(rows).toMatchObject({
      'turns heard': '12',
      'a turn': '14 words, 4.6 s (usually 8–22 words)',
      'speech rate': '182 words / min',
      partials: 'tuned: every 5 words or 2 s, none in the first 3 s',
      openers: '6 played of 10 drafted (70 %) ⠂ dropped: new words 2, meaning 1 ⠂ cut in: 1',
      'late words allowed': '5 (tunes itself within 1–10)',
      reactions: '3',
    });
    const fresh = Object.fromEntries(speechRows({ ...S, turns: { sampled: 2, total: 2, needed: 8 }, words: { median: null, p25: null, p75: null }, wpm: null, pauses: { per_turn: null, max_ms: null }, policy: { ...S.policy, source: 'defaults', every_words: 8, every_ms: 4000 }, drafts: { ...S.drafts, made: 0, used: 0, misfires: 0, dropped: {}, use_rate: null }, reacts: 0 }));
    expect(fresh).toEqual({ 'turns heard': '2 (learning: 8 needed)', partials: 'defaults: every 8 words or 4 s, none in the first 3 s', openers: 'none drafted yet', 'late words allowed': '5 (tunes itself within 1–10)' });
  });
});
