import { describe, expect, it } from 'vitest';
import { audioFileName, getAudio, groupEntries, setKeepAudio, type ActivityEntry } from './activity';
import { clientTimingLine, fmtAt, hexBytes, levelDb, traceModel } from './trace';

const T = { relay: 'https://relay.example/', deviceId: 'molty-k7m2w9c4ya', linkCode: 'K7QX2MPA' };
const J = 'molty-k7m2w9c4ya.j_Qm9vZ2llV29vZ2ll';
const T0 = 1_800_000_000_000;

// A shake turn: hard shake, trimmed noise, a Wi-Fi stall (resends, a lost range), clipping, the box's words and a held turn end.
const trace = {
  v: 1, trigger: 'shake', t0: T0,
  dev: {
    shake: { at: -140, peak_g: 8.2, rev: 4, ms: 420 }, set: 340, setc: 0, wall: 8970, cap: 5400, gain: 6, pk: 998,
    lv: '5a6488a0c8b4', lv0: 340, clip: [[7, 18]], clip_n: 18,
    frames: [[0, 340, 520, 0, 40], [1, 640, 3900, 2, 3100], [3, 1240, 4000, 0, 2600]],
    sent: 3, fail: 2, resent: 1, lost: [[2, 2]], lost_ms: 300, rssi: [-69, -79], end: 'turn_end', speech_ms: 1200,
  },
  box: {
    v: 1, step_ms: 80, skip: 12, thr: 0.6, hold_s: 1.2, audio_s: 5.36,
    act: [[0.88, 2.4]], pause: 'ff80202020c0ff', pause_every: 2,
    words: [['I', 1.04, 1.1], ['measured', 1.1, null], ['from', 2.2, 2.4]],
    end: { at: 4.72, rule: 'hold', word: 'from', held_s: 1.2, ema: 0.71 }, final_ms: 212,
  },
  relay: { voice: 'box', cmd: 40, first_frame: 480, turn_end: 4500, end: 4540, stt: 210, utterance: 4900 },
};
const job: ActivityEntry = { id: 'a5', ts: T0, kind: 'job', job: J, op: 'listen', ok: true, ms: 5000, data: { status: 'done', trace, audio: true } };
const hook: ActivityEntry = { id: 'a6', ts: T0 + 5080, kind: 'trace', ev: 'webhook', job: J, agent: 'Claude', data: { status: 200, ms: 180, attempt: 1 } };
const reply: ActivityEntry = { id: 'a7', ts: T0 + 7000, kind: 'trace', ev: 'reply', job: J, agent: 'Claude', data: { op: 'talk', reply_ms: 2100, first_audio_ms: 2600, client_timing: { received_ms: 120, model_ms: 900 } } };

describe('trace helpers', () => {
  it('decodes the level hex (byte = 2·(dBFS+100))', () => {
    expect(hexBytes('00c8zz64')).toEqual([0, 200, 100]);
    expect(levelDb(200)).toBe(0);
    expect(levelDb(100)).toBe(-50);
    expect(levelDb(0)).toBe(-100);
  });
  it('formats client_timing and times', () => {
    expect(clientTimingLine({ received_ms: 120, model_ms: 900, bad: 'x' })).toBe('received 120 ms ⠂ model 900 ms');
    expect(fmtAt(-140)).toBe('−140 ms');
    expect(fmtAt(4720)).toBe('+4.7 s');
  });
});

describe('traceModel', () => {
  it('places a trimmed level strip by the relay\'s lv0 and lv_ms', () => {
    const t = { ...trace, dev: { ...trace.dev, lv: '5a64', lv0: 300, lv_ms: 200 } };
    const m = traceModel({ ...job, data: { ...job.data, trace: t } })!;
    expect(m.levels.map((l) => l.at)).toEqual([-140 + 300, -140 + 500]);
  });
  it('is null without a trace (old relay)', () => {
    expect(traceModel({ ...job, data: { status: 'done' } })).toBeNull();
    expect(traceModel(undefined)).toBeNull();
  });
  it('builds the timeline: audio starts after the shake noise, box times map onto it', () => {
    const m = traceModel(job, [hook, reply])!;
    expect(m.origin).toBe(-140 + 340);
    expect(m.levels).toHaveLength(6);
    expect(m.levels[0]).toEqual({ at: 200, db: levelDb(0x5a) });
    expect(m.clips).toEqual([{ at: 560, n: 18 }]);   // chunk 7 from the mic start (relay: floor(ct / 100))
    expect(m.speech).toEqual([[1080, 2600]]);
    expect(m.words.map((w) => w.text)).toEqual(['I', 'measured', 'from']);
    expect(m.words[1].end).toBeUndefined();
    expect(m.pause[0]).toEqual({ at: 200, v: 1 });   // from the box's first step (skipped steps are recorded too)
    expect(m.pause[1].at - m.pause[0].at).toBe(160);
    expect(m.threshold).toBe(0.6);
    expect(m.rssi).toEqual([-69, -79]);
    const labels = m.events.map((e) => e.label);
    expect(labels).toContain('shake 8.2 g, 4 reversals');
    expect(labels).toContain('shake noise trimmed: 340 ms');
    expect(labels.some((l) => l.startsWith('end of turn: a mid-sentence pause held (last word “from”), held 1.2 s'))).toBe(true);
    expect(labels).toContain('frame 1 resent after 2 failed sends, waited 3.1 s');
    expect(labels).toContain('frames 2 never arrived');
    expect(labels).toContain('webhook → Claude: 200 in 180 ms');
    expect(labels).toContain('Claude answered (talk) 2.1 s after the utterance');
    expect(labels).toContain('Claude reports: received 120 ms ⠂ model 900 ms');
    expect(m.events.find((e) => e.label.startsWith('webhook'))!.at).toBe(5080);
    // relay times are ms from t0 as they stand (not offset by cmd); stt is a duration after the end
    expect(m.events.find((e) => e.label === 'first audio at the relay')!.at).toBe(480);
    expect(m.events.find((e) => e.label === 'listen stopped (turn end)')!.at).toBe(4500);
    expect(m.events.find((e) => e.label.startsWith('transcript ready'))!.at).toBe(4540 + 210);
    expect(m.events.find((e) => e.label.startsWith('first audio of the answer'))!.at).toBe(4900 + 2600);
    // events in time order, from the shake on
    expect(m.events.map((e) => e.at)).toEqual([...m.events.map((e) => e.at)].sort((a, b) => a - b));
    expect(m.from).toBe(-140);
  });
  it('summarises what needs attention first', () => {
    const m = traceModel(job, [hook, reply])!;
    const sum = m.summary.map((s) => s.text);
    expect(sum[0]).toBe('lost 3.2 s of audio (captured 5.4 s in 9.0 s)');   // the mic gap beats lost_ms
    expect(sum).toContain('18 samples clipped (mic gain 6)');
    expect(sum).toContain('2 failed sends, 1 frame resent');
    expect(sum).toContain('weak Wi-Fi (-79 dBm)');
    expect(sum).toContain('shake noise trimmed 340 ms');
    expect(sum).not.toContain('clean turn: nothing lost, nothing clipped');
  });
  it('a partial trace (old firmware, no box) shows what is there and calls a clean turn clean', () => {
    const m = traceModel({ ...job, data: { trace: { v: 1, trigger: 'agent', t0: T0, relay: { voice: 'workers-ai', cmd: 30, end: 3000, stt: 400 } } } })!;
    expect(m.levels).toEqual([]);
    expect(m.words).toEqual([]);
    expect(m.summary[0]).toEqual({ text: 'clean turn: nothing lost, nothing clipped', tone: 'ok' });
    expect(m.events.map((e) => e.label)).toEqual(['mic open', 'listen ended', 'transcript ready: 400 ms after the end ⠂ workers-ai']);
  });
  it('a failed webhook and a no-speech decision', () => {
    const m = traceModel({ ...job, data: { trace: { t0: T0, box: { ns: { at: 8, quiet_s: 8, why: 'no word, no voice activity for 8.0 s' } }, relay: { cmd: 0 } } } },
      [{ ...hook, data: { status: 503, ms: 90, attempt: 2, error: 'http_503' } }])!;
    expect(m.events.map((e) => e.label)).toContain('no speech: no word, no voice activity for 8.0 s');
    expect(m.events.map((e) => e.label)).toContain('webhook → Claude: 503 in 90 ms (attempt 2)');
    expect(m.summary[0]).toEqual({ text: 'webhook to Claude failed: http_503', tone: 'err' });
  });
});

describe('grouping trace entries', () => {
  it('joins them to their turn, without making a webhook’s agent the turn’s agent', () => {
    const [g] = groupEntries([job, hook, reply]);
    expect(g.traces.map((e) => e.id)).toEqual(['a6', 'a7']);
    expect(g.agent).toBeUndefined();
    expect(g.op).toBe('listen');
  });
});

describe('keep audio + download', () => {
  it('posts keep_audio and reads it back', async () => {
    const calls: RequestInit[] = [];
    const f = (async (_u: string, init: RequestInit) => { calls.push(init); return new Response(JSON.stringify({ ok: true, result: { entries: [], next: 'a0', transcripts: true, keep_audio: true, audio: { turns: 5, max_s: 30, max_age_h: 24 } } })); }) as unknown as typeof fetch;
    const p = await setKeepAudio(T, true, f);
    expect(JSON.parse(String(calls[0].body))).toEqual({ keep_audio: true });
    expect(p.keep_audio).toBe(true);
    expect(p.audio?.turns).toBe(5);
  });
  it('downloads a turn’s wav with the link code header (never in the URL)', async () => {
    const calls: Array<{ url: string; init: RequestInit }> = [];
    const f = (async (url: string, init: RequestInit) => { calls.push({ url, init }); return new Response(new Uint8Array([82, 73, 70, 70]), { headers: { 'content-type': 'audio/wav' } }); }) as unknown as typeof fetch;
    const b = await getAudio(T, J, f);
    expect(b.size).toBe(4);
    expect(calls[0].url).toBe(`https://relay.example/v1/devices/molty-k7m2w9c4ya/activity/audio/${encodeURIComponent(J)}`);
    expect((calls[0].init.headers as Record<string, string>)['x-molty-link-code']).toBe('K7QX2MPA');
    expect(audioFileName(J)).toBe('molty-j_Qm9vZ2llV29vZ2ll.wav');
  });
  it('maps an expired turn to not_found', async () => {
    const f = (async () => new Response(JSON.stringify({ ok: false, error: 'not_found' }), { status: 404 })) as unknown as typeof fetch;
    await expect(getAudio(T, J, f)).rejects.toMatchObject({ code: 'not_found' });
  });
});
