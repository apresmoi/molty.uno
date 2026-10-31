import { describe, expect, it } from 'vitest';
import { gapOptions, reflexOptions, voiceOptions, VoiceApiError, boxLine, getVoice, latencyLine, pairCommand, servingLine, setVoice, unpairBox, voiceChoices, voiceLabel, type VoiceStatus } from './voice';

const T = { relay: 'https://relay.example', deviceId: 'molty-k7m2w9c4ya', linkCode: 'K7QX2MPA' };

const status = (over: Partial<VoiceStatus> = {}, box: Partial<VoiceStatus['box']> = {}): VoiceStatus => ({
  mode: 'auto', voice: null, cloud: 'workers-ai', serving: { tts: 'box', stt: 'box' },
  box: { paired: true, name: 'gpu box', paired_at: 1, connected: true, connected_since: 2, reported_name: 'gpu box', version: '0.1.0', voices: ['expresso/ex03-ex01_happy_001_channel1_334s.wav'], stt: true, tts: true, last: {}, ...box },
  ...over,
});

function fakeFetch(reply: { status?: number; body: unknown } | Error) {
  const calls: Array<{ url: string; init: RequestInit }> = [];
  const f = (async (url: string, init: RequestInit) => {
    calls.push({ url, init });
    if (reply instanceof Error) throw reply;
    return new Response(JSON.stringify(reply.body), { status: reply.status ?? 200 });
  }) as unknown as typeof fetch;
  return { f, calls };
}

describe('voice box owner API', () => {
  it('reads with the current link code as a header, never in the URL', async () => {
    const { f, calls } = fakeFetch({ body: { ok: true, result: status() } });
    expect((await getVoice(T, f)).mode).toBe('auto');
    expect(calls[0].url).toBe('https://relay.example/v1/devices/molty-k7m2w9c4ya/voice');
    expect(calls[0].init.method).toBe('GET');
    expect((calls[0].init.headers as Record<string, string>)['x-molty-link-code']).toBe('K7QX2MPA');
  });
  it('sets mode / voice (POST JSON) and unpairs (DELETE)', async () => {
    const { f, calls } = fakeFetch({ body: { ok: true, result: status({ mode: 'box' }) } });
    await setVoice(T, { mode: 'box', voice: null }, f);
    expect(calls[0].init.method).toBe('POST');
    expect(JSON.parse(String(calls[0].init.body))).toEqual({ mode: 'box', voice: null });
    await unpairBox(T, f);
    expect(calls[1].init.method).toBe('DELETE');
  });
  it('maps failures: a relay without the route is unsupported, others keep their code', async () => {
    await expect(getVoice(T, fakeFetch({ status: 404, body: { ok: false, error: 'not_found' } }).f)).rejects.toMatchObject({ code: 'unsupported' });
    await expect(getVoice(T, fakeFetch({ status: 401, body: { ok: false, error: 'unauthorized' } }).f)).rejects.toMatchObject({ code: 'unauthorized' });
    await expect(getVoice(T, fakeFetch(new TypeError('net')).f)).rejects.toBeInstanceOf(VoiceApiError);
  });
});

describe('voice card text', () => {
  it('says whether a box is paired / connected', () => {
    expect(boxLine(status())).toEqual({ text: 'gpu box ⠂ connected ⠂ listens + speaks ⠂ v0.1.0', kind: 'ok' });
    expect(boxLine(status({}, { connected: false })).kind).toBe('warn');
    expect(boxLine(status({}, { paired: false })).text).toBe('no voice box paired');
  });
  it('says who serves now', () => {
    expect(servingLine(status())).toBe('Molty uses: voice box');
    expect(servingLine(status({ serving: { tts: 'box', stt: 'workers-ai' } }))).toBe('speaks with voice box ⠂ listens with cloud (workers-ai)');
    expect(servingLine(status({ serving: { tts: 'voice_box_offline', stt: 'voice_box_offline' } }))).toBe('Molty uses: nobody (box offline)');
  });
  it('shows the last latencies only once measured', () => {
    expect(latencyLine(status())).toBe('');
    expect(latencyLine(status({}, { last: { stt_ms: 320, tts_first_audio_ms: 1450 } }))).toBe('last turn: transcript 0.32 s after you stop ⠂ first word in 1.4 s');
  });
  it('voices: the box list plus a saved default it no longer lists; short labels', () => {
    expect(voiceChoices(status({ voice: 'old/x.wav' }))).toEqual(['old/x.wav', 'expresso/ex03-ex01_happy_001_channel1_334s.wav']);
    expect(voiceLabel('expresso/ex03-ex01_happy_001_channel1_334s.wav')).toBe('ex03 happy');
    expect(voiceLabel('unmute-prod-website/default_voice.wav')).toBe('default voice');
  });
  it('pair command is filled with this Molty', () => {
    expect(pairCommand(T)).toBe('molty-voice pair --relay https://relay.example --device molty-k7m2w9c4ya --link-code K7QX2MPA');
  });
});

describe('reflex setting', () => {
  const rx = (o: Partial<import('./voice').ReflexStatus> = {}) => ({ mode: 'jev' as const, compare: false, engines: { local: true, jev: true, openai: false, keywords: true }, engine: 'jev' as const, timeout_ms: 600, ...o });
  it('posts the engine and compare mode', async () => {
    const { f, calls } = fakeFetch({ body: { ok: true, result: status({ reflex: rx() }) } });
    await setVoice(T, { reflex: 'jev', reflex_compare: true, liveliness: 'lively' }, f);
    expect(JSON.parse(String(calls[0].init.body))).toEqual({ reflex: 'jev', reflex_compare: true, liveliness: 'lively' });
  });
  it('says who reacts, and why not the chosen engine', async () => {
    const { reflexLine } = await import('./voice');
    expect(reflexLine(undefined)).toBe('');
    expect(reflexLine(rx({ mode: 'off', engine: null }))).toBe('');
    expect(reflexLine(rx())).toBe('reacts with Jev');
    expect(reflexLine(rx({ mode: 'openai', engine: 'keywords', fallback: 'no_key', compare: true }))).toBe('reacts with keywords (no key on the relay for OpenAI) ⠂ comparing every engine in the activity log');
    expect(reflexLine(rx({ mode: 'auto', engine: 'local' }))).toBe('reacts with the voice box');
  });
});

describe('setup hides what the owner\'s tools can\'t do', () => {
  const ids = (o: Array<{ id: string }>) => o.map((x) => x.id);
  const engines = { local: false, jev: false, openai: false, keywords: true };
  const rx = (e = engines) => ({ reflex: { mode: 'off' as const, compare: false, engines: e, engine: null, timeout_ms: 1 } });

  it('voice: no box, no box choice; no cloud provider, no cloud choice; text only always', () => {
    expect(ids(voiceOptions(status({}, { paired: false })))).toEqual(['cloud', 'off']);
    expect(ids(voiceOptions(status({ cloud: 'none' })))).toEqual(['box', 'off']);
  });
  it('gap: first words only with a box that offers them', () => {
    expect(ids(gapOptions(status()))).toEqual(['phrases', 'off']);
    expect(ids(gapOptions(status({ first_words: { mode: 'off', available: true } })))).toEqual(['local', 'phrases', 'off']);
  });
  it('reflex: only engines available now, keywords and off always', () => {
    expect(ids(reflexOptions(status(rx())))).toEqual(['off', 'keywords']);
    expect(ids(reflexOptions(status(rx({ ...engines, local: true, jev: true }))))).toEqual(['off', 'local', 'jev', 'keywords']);
  });
});
