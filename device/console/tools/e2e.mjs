// Headless Chromium walk-through against the mock device (tools/mock-device.mjs on
// :8170 — start it with `pnpm mock` after `pnpm build`), at phone width (390 px):
//   1. the 5-step setup flow (Wi-Fi → Molty → Agent → Tap → Voice → Done), screenshot per step
//   2. the same Molty step with the three.js CDN blocked → live device preview fallback
//   3. the settings pages (agent single-message design, molty picker, degraded firmware)
// Then an optional READ-ONLY pass against a real device via ?device= (GETs only, plus
// harmless fluid orders that it restores). It never scans, connects, forgets or uploads there.
//
//   node tools/e2e.mjs [outDir] [realDevice]
import { chromium } from 'playwright';
import { mkdirSync, readFileSync } from 'node:fs';

const OUT = process.argv[2] ?? './e2e-out';
const REAL = process.argv[3] ?? '';
const MOCK = 'http://localhost:8170';
const PHONE = { width: 390, height: 844 };
mkdirSync(OUT, { recursive: true });

const errors = [];
// Known-benign: headless-GPU readback notes from the viewer engine (three → canvas), our
// own canvas sampling, and the CDN request the offline pass blocks on purpose.
const BENIGN = [/GL Driver Message/, /willReadFrequently/];
const checks = [];
const ok = (cond, what) => { checks.push(`${cond ? 'PASS' : 'FAIL'} ${what}`); if (!cond) process.exitCode = 1; };
const mockLog = async () => (await fetch(`${MOCK}/__mock/log`)).json();
const mock = (path, c = {}) => fetch(`${MOCK}/__mock/${path}`, { method: 'POST', body: JSON.stringify(c) });
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

// The relay's owner voice-box API (GET|POST|DELETE /v1/devices/<id>/voice, X-Molty-Link-Code), faked here.
const VOICE_CORS = { 'access-control-allow-origin': '*', 'access-control-allow-methods': 'GET, POST, DELETE, OPTIONS', 'access-control-allow-headers': 'content-type, x-molty-link-code' };
const voiceRelay = {
  calls: [],
  state: { mode: 'auto', voice: null, cloud: 'workers-ai', serving: { tts: 'box', stt: 'box' },
    reflex: { mode: 'off', compare: false, engines: { local: true, jev: true, openai: false, keywords: true }, engine: null, timeout_ms: 600, liveliness: 'normal' },
    box: { paired: true, name: 'voice box', paired_at: 1, connected: true, connected_since: 2, reported_name: 'voice box', version: '0.1.0',
      voices: ['expresso/ex03-ex01_happy_001_channel1_334s.wav', 'expresso/ex03-ex01_calm_001_channel2_1081s.wav'], stt: true, tts: true, last: { stt_ms: 310, tts_first_audio_ms: 240 } } },
  async route(r) {
    const req = r.request();
    if (req.method() === 'OPTIONS') return r.fulfill({ status: 204, headers: VOICE_CORS });
    const code = req.headers()['x-molty-link-code'];
    this.calls.push({ method: req.method(), code, body: req.postData() });
    if (code !== 'K7QX2MPA') return r.fulfill({ status: 401, headers: VOICE_CORS, contentType: 'application/json', body: '{"ok":false,"error":"unauthorized"}' });
    const st = this.state;
    if (req.method() === 'POST') { const b = JSON.parse(req.postData() || '{}'); if (b.mode) st.mode = b.mode; if (b.voice !== undefined) st.voice = b.voice;
      if (b.reflex) { st.reflex.mode = b.reflex; st.reflex.engine = b.reflex === 'off' ? null : b.reflex === 'openai' ? 'keywords' : b.reflex === 'auto' ? 'local' : b.reflex; st.reflex.fallback = b.reflex === 'openai' ? 'no_key' : undefined; }
      if (b.reflex_compare !== undefined) st.reflex.compare = b.reflex_compare;
      if (b.liveliness !== undefined) st.reflex.liveliness = b.liveliness;
      if (st.mode === 'cloud') st.serving = { tts: 'workers-ai', stt: 'workers-ai' }; }
    if (req.method() === 'DELETE') { st.box = { ...st.box, paired: false, name: null, connected: false, voices: [] }; st.serving = { tts: 'workers-ai', stt: 'workers-ai' }; }
    return r.fulfill({ status: 200, headers: VOICE_CORS, contentType: 'application/json', body: JSON.stringify({ ok: true, result: st }) });
  },
};

// The relay's owner activity API (GET|POST /v1/devices/<id>/activity, X-Molty-Link-Code), faked here:
// two agents, a done talk (timings + device lines), a failed talk (voice_box_offline), a disconnect, a pairing.
const JA = 'molty-k7m2w9c4ya.j_Qm9vZ2llV29vZ2ll', JB = 'molty-k7m2w9c4ya.j_R3Jva2JvdEZhaWxz', JC = 'molty-k7m2w9c4ya.j_U2hha2VUdXJuVHJj';
// A shake turn with its trace (docs/AGENT-LINK.md "Turn trace"): a hard shake (noise trimmed), a
// Wi-Fi stall (failed sends, a resent frame, a lost one), clipping, the box's words and a turn end
// by the mid-sentence hold, the webhook delivered (200 in 180 ms) and the agent's reply with client_timing.
const SHAKE_TRACE = {
  v: 1, trigger: 'shake', t0: 0,
  dev: { shake: { at: -140, peak_g: 8.2, rev: 4, ms: 420 }, set: 340, setc: 0, wall: 8970, cap: 5400, gain: 6, pk: 998,
    lv: '2a3c6e8ca0b4b4a8a0948c8078706c6460', lv0: 340, clip: [[7, 18]], clip_n: 18,
    frames: [[0, 340, 520, 0, 40], [1, 640, 820, 0, 40], [2, 940, 3900, 2, 2700], [4, 1540, 4000, 0, 2200], [5, 1840, 4100, 0, 1900]],
    sent: 5, fail: 2, resent: 1, lost: [[3, 3]], lost_ms: 300, rssi: [-69, -79], end: 'turn_end', speech_ms: 1200 },
  box: { v: 1, step_ms: 80, skip: 12, thr: 0.6, hold_s: 1.2, audio_s: 5.36, act: [[0.4, 1.9]], pause: 'ffc0402010102040a0d0e0f0', pause_every: 2,
    words: [['I', 0.5, 0.6], ['measured', 0.6, 1.0], ['it,', 1.0, 1.2], ['from', 1.6, 1.9]],
    end: { at: 4.32, rule: 'hold', word: 'from', held_s: 1.2, ema: 0.71 }, final_ms: 212 },
  relay: { voice: 'box', cmd: 40, first_frame: 480, turn_end: 4500, end: 4540, stt: 210, utterance: 4900,
    reflex_cfg: { mode: 'jev', engine: 'jev', compare: ['jev', 'local', 'keywords'] },
    reflex: [{ n: 4, at: 1500, e: 'jev', ms: 212, ch: 19, tk: 670, pick: 'surprised', c: 0.91, cat: 'other', cc: 0.8, fired: 'ok' },
      { n: 4, at: 1330, e: 'local', ms: 34, ch: 19, tk: 9, pick: 'none', raw: 'happy', c: 0.3, cat: 'other' },
      { n: 4, at: 1290, e: 'keywords', ms: 0, ch: 19, pick: 'none', cat: 'other' }] },
};
const T0 = Date.parse('2026-10-30T19:00:00Z');
const activityRelay = {
  calls: [],
  transcripts: true,
  entries: [
    { id: 'a1', ts: T0, kind: 'pair', ev: 'approved', agent: 'Claude' },
    { id: 'a2', ts: T0 + 60000, kind: 'call', job: JA, agent: 'Claude', op: 'talk', ok: true, ms: 4210, args: { say: 'Want some tea?', listen: true } },
    { id: 'a3', ts: T0 + 60120, kind: 'device_log', job: JA, data: { text: 'EVT audio.say start ms=1400 int_free=61234', level: 'EVT', boot: 74, seq: 801 } },
    { id: 'a4', ts: T0 + 62900, kind: 'device_log', job: JA, data: { text: 'W (62900) audio: mic gain clipped twice', level: 'W', boot: 74, seq: 805 } },
    { id: 'a5', ts: T0 + 64200, kind: 'job', job: JA, agent: 'Claude', op: 'talk', ok: true, ms: 4200,
      data: { status: 'done', reason: 'turn_end', said: 'Want some tea?', heard: 'yes please, green tea',
        voice: { tts: 'box', stt: 'box' }, timings: { queue: 3, say: { tts_first_byte: 240, first_audio: 260, play: 1500, voice: 'box' }, listen: { cmd: 40, record: 2100, stt: 310, voice: 'box' }, total: 4200 } } },
    { id: 'a6', ts: T0 + 120000, kind: 'device', ev: 'disconnect', data: { code: 1006 } },
    { id: 'a7', ts: T0 + 125000, kind: 'device', ev: 'connect', data: { fw: '0.4.0', rssi: -61 } },
    { id: 'a8', ts: T0 + 180000, kind: 'pair', ev: 'approved', agent: 'Grokbot' },
    { id: 'a9', ts: T0 + 240000, kind: 'call', job: JB, agent: 'Grokbot', op: 'talk', ok: false, error: 'voice_box_offline', ms: 18, args: { say: 'Hello there', listen: true } },
    { id: 'a10', ts: T0 + 240010, kind: 'job', job: JB, agent: 'Grokbot', op: 'talk', ok: false, error: 'voice_box_offline', ms: 12, data: { status: 'failed', error: 'voice_box_offline' } },
    { id: 'a11', ts: T0 + 240015, kind: 'device_log', job: JB, data: { text: 'E (240015) agent_link: say refused: no stream', level: 'E', boot: 74, seq: 990 } },
    { id: 'a12', ts: T0 + 300000, kind: 'job', job: JC, op: 'listen', ok: true, ms: 5100,
      data: { status: 'done', reason: 'turn_end', heard: 'I measured it, from', audio: true, voice: { stt: 'box' }, timings: { cmd: 40, record: 4460, stt: 210, voice: 'box' }, trace: { ...SHAKE_TRACE, t0: T0 + 300000 } } },
    { id: 'a13', ts: T0 + 305080, kind: 'trace', ev: 'webhook', job: JC, agent: 'Claude', data: { status: 200, ms: 180, attempt: 1 } },
    { id: 'a14', ts: T0 + 307000, kind: 'trace', ev: 'reply', job: JC, agent: 'Claude', data: { op: 'talk', reply_ms: 2100, first_audio_ms: 2600, client_timing: { received_ms: 120, model_ms: 900 } } },
  ],
  keepAudio: false,
  audioCalls: [],
  async audio(r) {
    const req = r.request();
    if (req.method() === 'OPTIONS') return r.fulfill({ status: 204, headers: VOICE_CORS });
    const code = req.headers()['x-molty-link-code'];
    this.audioCalls.push({ url: req.url(), code });
    if (code !== 'K7QX2MPA') return r.fulfill({ status: 401, headers: VOICE_CORS, contentType: 'application/json', body: '{"ok":false,"error":"unauthorized"}' });
    const wav = Buffer.concat([Buffer.from('RIFF'), Buffer.alloc(40)]);
    return r.fulfill({ status: 200, headers: VOICE_CORS, contentType: 'audio/wav', body: wav });
  },
  async route(r) {
    const req = r.request();
    if (req.method() === 'OPTIONS') return r.fulfill({ status: 204, headers: VOICE_CORS });
    const code = req.headers()['x-molty-link-code'];
    const u = new URL(req.url());
    this.calls.push({ method: req.method(), code, since: u.searchParams.get('since'), body: req.postData() });
    if (code !== 'K7QX2MPA') return r.fulfill({ status: 401, headers: VOICE_CORS, contentType: 'application/json', body: '{"ok":false,"error":"unauthorized"}' });
    if (req.method() === 'POST') {
      const b = JSON.parse(req.postData() || '{}');
      if (typeof b.transcripts === 'boolean') this.transcripts = b.transcripts;
      if (typeof b.keep_audio === 'boolean') this.keepAudio = b.keep_audio;
    }
    const since = u.searchParams.get('since');
    const after = since && req.method() === 'GET' ? Number(since.slice(1)) : 0;
    const entries = this.entries.filter((e) => Number(e.id.slice(1)) > after);
    const next = `a${this.entries.length}`;
    return r.fulfill({ status: 200, headers: VOICE_CORS, contentType: 'application/json',
      body: JSON.stringify({ ok: true, result: { entries: req.method() === 'POST' ? [] : entries, next, more: false, transcripts: this.transcripts, keep_audio: this.keepAudio, audio: { turns: 5, max_s: 30, max_age_h: 24 }, limits: { max_age_h: 24, max_entries: 3000 } } }) });
  },
};

// The relay's owner speech API (GET|DELETE /v1/devices/<id>/speech, X-Molty-Link-Code): a calibrated human.
const speechRelay = {
  calls: [],
  stats: {
    turns: { sampled: 12, total: 30, needed: 8 }, words: { median: 14, p25: 8, p75: 22 }, seconds: { median: 4.6, p25: 2.9, p75: 7.1 },
    wpm: 182, pauses: { per_turn: 1, max_ms: 900 }, policy: { source: 'calibrated', lead_ms: 3000, every_words: 5, every_ms: 2000, max_per_turn: 8 },
    drafts: { made: 10, used: 6, misfires: 1, dropped: { new_words: 2, meaning: 1 }, use_rate: 0.7, misfire_rate: 0.14, threshold: 5, threshold_bounds: [1, 10] },
    partials: 20, reacts: 3, updated: T0,
  },
  async route(r) {
    const req = r.request();
    if (req.method() === 'OPTIONS') return r.fulfill({ status: 204, headers: VOICE_CORS });
    const code = req.headers()['x-molty-link-code'];
    this.calls.push({ method: req.method(), code });
    if (code !== 'K7QX2MPA') return r.fulfill({ status: 401, headers: VOICE_CORS, contentType: 'application/json', body: '{"ok":false,"error":"unauthorized"}' });
    if (req.method() === 'DELETE') this.stats = { ...this.stats, turns: { sampled: 0, total: 0, needed: 8 }, words: { median: null, p25: null, p75: null }, seconds: { median: null, p25: null, p75: null }, wpm: null, pauses: { per_turn: null, max_ms: null }, policy: { ...this.stats.policy, source: 'defaults', every_words: 8, every_ms: 4000 }, drafts: { ...this.stats.drafts, made: 0, used: 0, misfires: 0, dropped: {}, use_rate: null, misfire_rate: null, threshold: 4 }, partials: 0, reacts: 0 };
    return r.fulfill({ status: 200, headers: VOICE_CORS, contentType: 'application/json', body: JSON.stringify({ ok: true, result: this.stats }) });
  },
};

const browser = await chromium.launch();
async function newPage(tag, { offline = false, viewport = PHONE } = {}) {
  const ctx = await browser.newContext({ viewport, deviceScaleFactor: 2 });
  await ctx.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: MOCK });
  if (offline) await ctx.route('https://cdn.jsdelivr.net/**', (r) => r.abort('internetdisconnected'));
  // The status page asks the device's relay for the latest release: answer from the mock.
  await ctx.route('**/v1/releases/latest*', async (r) => r.fulfill({ response: await ctx.request.get(`${MOCK}/__mock/release?${new URL(r.request().url()).searchParams}`), headers: { 'access-control-allow-origin': '*' } }));
  await ctx.route('**/v1/devices/*/voice', (r) => voiceRelay.route(r));
  await ctx.route('**/v1/devices/*/activity*', (r) => activityRelay.route(r));
  await ctx.route('**/v1/devices/*/activity/audio/*', (r) => activityRelay.audio(r));
  await ctx.route('**/v1/devices/*/speech', (r) => speechRelay.route(r));
  const page = await ctx.newPage();
  page.on('console', (m) => {
    if (m.type() !== 'error' && m.type() !== 'warning') return;
    if (BENIGN.some((r) => r.test(m.text())) || (offline && /ERR_INTERNET_DISCONNECTED/.test(m.text()))) return;
    errors.push(`[${tag}] console.${m.type()}: ${m.text()}`);
  });
  page.on('pageerror', (e) => errors.push(`[${tag}] pageerror: ${e.message}`));
  // net::ERR_ABORTED is the browser cancelling an in-flight request (a poll cut short by a
  // navigation or a reload): not a failure of the page, and timing-dependent on CI.
  page.on('requestfailed', (r) => {
    if (r.failure()?.errorText === 'net::ERR_ABORTED') return;
    if (!(offline && r.url().includes('cdn.jsdelivr.net'))) errors.push(`[${tag}] requestfailed: ${r.url()} ${r.failure()?.errorText}`);
  });
  return page;
}
const shot = (page, name) => page.screenshot({ path: `${OUT}/${name}.png`, fullPage: true });
const bubble = (page) => page.locator('.setup .bubble').innerText();
const noOverflow = async (page, what) =>
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth && [...document.querySelectorAll('.setup *')].every((e) => e.getBoundingClientRect().right <= innerWidth + 0.5)),
    `${what}: no horizontal overflow at 390px`);
/** Count lit + orange-accent pixels on the fluid canvas (the code must not have an ORP-orange digit). */
const canvasStats = (page) => page.evaluate(() => {
  const c = document.querySelector('.fstage canvas'); const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
  let lit = 0, orange = 0;
  for (let i = 0; i < d.length; i += 4) { const [r, g, b] = [d[i], d[i + 1], d[i + 2]]; if (r + g + b > 90) lit++; if (r > 200 && g > 90 && g < 180 && b < 90) orange++; }
  return { lit, orange };
});

// ── 1. setup flow ─────────────────────────────────────────────────────────────
{
  await mock('config', { link: true, file: true, reset: true, wifi: 'none' });
  const page = await newPage('setup');
  await page.goto(`${MOCK}/`);
  await page.waitForURL(/#\/setup\/wifi/);
  ok(true, 'first run lands on #/setup/wifi');
  await page.locator('.setup .opt', { hasText: 'FRITZ!Box' }).waitFor();
  ok((await bubble(page)).startsWith('Hi! Which Wi-Fi should I use?'), 'wifi: bubble asks one question');
  ok(await page.locator('.setup .opt.net').count() === 3, 'wifi: scanned list deduped (3 networks)');
  ok(await page.locator('.prog').innerText() === '● Wi-Fi   ○ Molty   ○ Agent   ○ Voice', 'wifi: progress line');
  await page.locator('.fstage canvas').first().waitFor({ timeout: 15000 });
  await wait(1200);
  await noOverflow(page, 'wifi');
  await shot(page, '01-wifi');

  await page.locator('.setup .opt', { hasText: 'FRITZ!Box' }).click();
  ok((await bubble(page)).includes('password for FRITZ!Box 7590 QX'), 'pass: asks for the password');
  await page.getByLabel('password').fill('wrong');
  await page.getByRole('button', { name: 'Connect' }).click();
  await page.locator('.setup .bubble', { hasText: 'that didn’t work' }).waitFor();
  ok(true, 'pass: wrong password → bubble says so, stays on password');
  await noOverflow(page, 'pass');
  await shot(page, '02-wifi-wrong-password');
  await page.getByLabel('password').fill('hunter2hunter2');
  await page.getByRole('button', { name: 'Connect' }).click();
  await page.locator('.setup .bubble', { hasText: 'Connecting' }).waitFor();
  await page.waitForURL(/#\/setup\/molty/);
  const sc = (await mockLog()).filter((l) => l.path === '/api/set_creds').map((l) => JSON.parse(l.body));
  ok(sc.at(-1)?.ssid === 'FRITZ!Box 7590 QX' && sc.at(-1)?.password === 'hunter2hunter2', 'set_creds body {ssid,password}');
  ok((await bubble(page)).startsWith('Connected! Now, who should I be?'), 'wifi → molty auto-advance on success');

  // Molty
  await page.locator('.setup .tile img').first().waitFor();
  const tiles = await page.locator('.setup .tile').count();
  ok(tiles === 29, `molty: grid has all 29 Moltys (${tiles})`);
  const thumbsOk = await page.evaluate(() => [...document.querySelectorAll('.setup .tile img')].every((i) => i.complete && i.naturalWidth === 160));
  ok(thumbsOk, 'molty: 29 bundled thumbnails decoded');
  ok(!/creature/i.test(await page.locator('.setup').innerText()), 'molty: no "creature" in the copy');
  ok(await page.getByRole('button', { name: 'Pick one' }).isDisabled(), 'molty: button disabled until a pick');
  await page.locator('.setup .tile', { hasText: 'Frog' }).click();
  await page.locator('.setup .bubble small', { hasText: 'On your Molty now' }).waitFor();
  const log = await mockLog();
  const up = log.filter((l) => l.path.startsWith('/api/creature/file'));
  const frog = readFileSync(new URL('../../creatures/molty/frog.molty', import.meta.url));
  ok(up.length === 1 && up[0].path === '/api/creature/file?name=cute-frog' && up[0].bytes === frog.length && up[0].head === 'MLTY', `molty: frog.molty uploaded raw (${up[0]?.bytes} B, magic ${up[0]?.head})`);
  ok(up[0]?.ctype?.startsWith('text/plain'), 'molty: upload is a CORS-simple request');
  ok(log.some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).state === 'creature'), 'molty: device told to show the creature state');
  ok(await page.getByRole('button', { name: 'Choose Frog' }).isEnabled(), 'molty: "Choose Frog"');
  ok((await page.locator('.fstage').getAttribute('data-mode')) === 'fluid', 'molty: round screen runs the real fluid engine (three from CDN)');
  await wait(3500); // let it emerge and start dancing
  const st = await canvasStats(page);
  ok(st.lit > 2000, `molty: Frog drawn on the round screen (${st.lit} lit px)`);
  await noOverflow(page, 'molty');
  await shot(page, '03-molty');
  await page.getByRole('button', { name: 'Choose Frog' }).click();

  // Agent
  await page.waitForURL(/#\/setup\/agent/);
  await page.locator('.msg pre', { hasText: 'molty-k7m2w9c4ya' }).waitFor();
  const msg = await page.locator('.msg pre').innerText();
  // The mock device reports a private relay: the message names it; the skill comes from GitHub.
  const MSG = "Hi! Please connect yourself to my Molty.\n\nSkill: https://github.com/apresmoi/molty.uno/tree/main/skills/molty\nDevice: molty-k7m2w9c4ya\nCode: K7QX2MPA\nRelay: https://molty-relay.example.workers.dev\n\nI'll tap Molty to accept you.";
  ok(msg === MSG, 'agent: the one message has the skill on GitHub, real device id, code, and the (private) relay');
  ok(await page.getByLabel('Claude Code install command').innerText() === 'claude plugin marketplace add apresmoi/molty.uno && claude plugin install molty@molty', 'agent: Claude Code plugin install line');
  ok(await page.getByLabel('Codex install command').innerText() === 'codex plugin marketplace add apresmoi/molty.uno && codex plugin add molty@molty', 'agent: Codex plugin install line');
  ok((await bubble(page)).startsWith('Who will be my brain?'), 'agent: bubble');
  await page.getByRole('button', { name: 'Copy message' }).click();
  ok(await page.evaluate(() => navigator.clipboard.readText()) === MSG, 'agent: Copy message → clipboard has the message');
  ok(await page.getByRole('button', { name: 'Copied ✓' }).isVisible(), 'agent: button confirms the copy');
  await page.locator('.wait', { hasText: 'Waiting for your agent' }).waitFor();
  await wait(2500);
  ok(!/#\/setup\/done/.test(page.url()), 'agent: an older last_pair does not skip ahead');
  await noOverflow(page, 'agent');
  await shot(page, '04-agent');
  await page.locator('summary', { hasText: 'Advanced' }).click();
  const adv = await page.locator('.adv').innerText();
  ok(adv.includes('molty-k7m2w9c4ya') && adv.includes('K7QX2MPA') && adv.includes('molty pair --relay https://molty-relay.example.workers.dev') && adv.includes('https://molty-relay.example.workers.dev/mcp'),
    'agent: Advanced has device id, code, CLI pair command, MCP fallback');
  await page.getByLabel('relay url').fill('https://relay.example.org/');
  await page.getByRole('button', { name: 'Save' }).click();
  await page.locator('.adv-msg', { hasText: 'Saved' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/link' && JSON.parse(l.body).relay_url === 'wss://relay.example.org'), 'agent: relay POSTed as ws origin');
  ok((await page.locator('.msg pre').innerText()).includes('\nRelay: https://relay.example.org\n'), 'agent: message follows the new relay');
  await noOverflow(page, 'agent advanced');
  await shot(page, '05-agent-advanced');

  // Tap
  await mock('pair', { agent_name: 'Grok', code: '4821' });
  await page.locator('.setup .bubble', { hasText: 'Tap me if you see this code' }).waitFor({ timeout: 5000 });
  ok((await bubble(page)).includes('Grok wants to connect'), 'tap: auto-advance on pair, names the agent');
  await wait(2500);
  const tap = await canvasStats(page);
  ok(tap.lit > 800 && tap.orange < 40, `tap: code condensed in the fluid, uniform (lit ${tap.lit}, orange ${tap.orange})`);
  await noOverflow(page, 'tap');
  await shot(page, '06-tap');

  // a rejected request → back to the agent step
  await mock('decide', { ok: false, reason: 'denied' });
  await page.locator('.setup .bubble', { hasText: 'That didn’t work (denied). Send the message again?' }).waitFor({ timeout: 5000 });
  ok(/#\/setup\/agent/.test(page.url()), 'failed pair → back to the agent step');
  await shot(page, '07-agent-failed');

  // Done
  await mock('pair', { agent_name: 'Grok', code: '1937' });
  await page.locator('.setup .bubble', { hasText: 'Tap me' }).waitFor({ timeout: 5000 });
  await mock('decide', { ok: true });
  // Voice, the gap, reflex: only what the owner's tools can do
  await page.locator('.setup .bubble', { hasText: 'Should I talk?' }).waitFor({ timeout: 5000 });
  await page.locator('.setup .opt', { hasText: 'Text only' }).waitFor();
  ok(await page.locator('.setup .opt', { hasText: 'My voice box' }).count() === 1, 'voice: a paired box is offered');
  await shot(page, '07b-voice');
  await page.locator('.setup .opt', { hasText: 'My voice box' }).click();
  await page.locator('.setup .bubble', { hasText: 'What should I do meanwhile?' }).waitFor();
  ok(await page.locator('.setup .opt', { hasText: 'First words' }).count() === 0, 'gap: no local model on offer, no choice');
  await shot(page, '07c-gap');
  await page.locator('.setup .opt', { hasText: 'Filler phrases' }).click();
  await page.locator('.setup .bubble', { hasText: 'react while you talk' }).waitFor();
  ok(await page.locator('.setup .opt', { hasText: 'OpenAI' }).count() === 0, 'reflex: an engine with no key is hidden');
  await shot(page, '07d-reflex');
  await page.locator('.setup .opt', { hasText: 'Off' }).click();
  ok(voiceRelay.calls.some((c) => c.body?.includes('"mode":"box"')) && voiceRelay.calls.some((c) => c.body?.includes('"reflex":"off"')), 'setup saved its choices on the relay');
  await page.locator('.setup .bubble', { hasText: 'I’m Frog, and Grok is my brain.' }).waitFor({ timeout: 5000 });
  ok(true, 'done: "I’m Frog, and Grok is my brain."');
  ok(await page.locator('.prog').innerText() === '✓ Wi-Fi   ✓ Molty   ✓ Agent   ✓ Voice', 'done: progress all ticked');
  await wait(2500);
  ok((await canvasStats(page)).lit > 2000, 'done: Molty keeps dancing on the screen');
  await noOverflow(page, 'done');
  await shot(page, '08-done');
  // text only: no gap or reflex questions
  await page.evaluate(() => { location.hash = '#/setup/voice'; });
  await page.locator('.setup .opt', { hasText: 'Text only' }).click();
  await page.locator('.setup .bubble', { hasText: 'I’m Frog' }).waitFor({ timeout: 5000 });
  ok(voiceRelay.calls.some((c) => c.body?.includes('"mode":"off"')), 'text only: saved, straight to done');
  voiceRelay.state.mode = 'auto'; voiceRelay.state.serving = { tts: 'box', stt: 'box' };
  await page.getByRole('button', { name: 'Open Molty' }).click();
  await page.waitForURL(/#\/status/);
  await page.locator('.facts dd.hi', { hasText: 'molty-k7m2w9c4ya' }).waitFor();
  await page.goto(`${MOCK}/`);
  await page.waitForURL(/#\/status/);
  ok(true, 'after setup, / opens the status page');
  await shot(page, '09-status');
  await page.context().close();
}

// ── 2. offline: no three.js → live device preview on the round screen ──────
{
  await mock('config', { reset: true });
  const page = await newPage('offline', { offline: true });
  await page.goto(`${MOCK}/#/setup/molty`);
  await page.locator('.fstage[data-mode="device"] .pv-status', { hasText: 'live' }).waitFor({ timeout: 20000 });
  ok(true, 'offline: round screen falls back to the live device preview');
  await page.locator('.setup .tile', { hasText: 'Duck' }).click();
  await page.locator('.setup .bubble small', { hasText: 'On your Molty now' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/file?name=cute-duck' && l.head === 'MLTY'), 'offline: picking still uploads the bundled .molty');
  await wait(800);
  await noOverflow(page, 'offline molty');
  await shot(page, '10-molty-offline');
  await page.context().close();
}

// ── 3. settings pages + degraded firmware ────────────────────────────────────
{
  await mock('config', { reset: true });
  const page = await newPage('settings');
  await page.goto(`${MOCK}/#/agent`);
  await page.locator('.msg pre', { hasText: 'molty-k7m2w9c4ya' }).waitFor();
  ok(await page.getByRole('button', { name: 'Copy message' }).isVisible(), 'settings agent: same single-message design');
  await mock('pair', { agent_name: 'Claude', code: '5150' });
  await page.locator('.link-state', { hasText: 'tap molty to accept Claude' }).waitFor({ timeout: 5000 });
  ok(true, 'settings agent: shows a pending pair');
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'settings agent: no horizontal scroll at 390px');
  await shot(page, '11-settings-agent');
  await mock('decide', { ok: true });
  await page.goto(`${MOCK}/#/voice`);
  // Voice card: the owner's voice box, through the relay with the link code from GET /api/link
  const vc = page.locator('.voice.card');
  await vc.locator('p', { hasText: 'voice box ⠂ connected ⠂ listens + speaks' }).waitFor({ timeout: 5000 });
  ok(voiceRelay.calls.every((c) => c.code === 'K7QX2MPA'), 'settings voice: relay calls carry the current link code');
  ok(await vc.getByText('last turn: transcript 0.31 s after you stop').isVisible(), 'settings voice: last latencies');
  const modes = vc.getByRole('group', { name: 'voice mode' });
  ok(await modes.getByRole('button', { name: 'auto' }).getAttribute('aria-pressed') === 'true', 'settings voice: mode auto by default');
  await modes.getByRole('button', { name: 'cloud' }).click();
  await modes.locator('button[aria-pressed="true"]', { hasText: 'cloud' }).waitFor();
  ok(voiceRelay.calls.some((c) => c.method === 'POST' && JSON.parse(c.body).mode === 'cloud'), 'settings voice: mode sent to the relay');
  await vc.getByLabel('box voice').selectOption('expresso/ex03-ex01_calm_001_channel2_1081s.wav');
  await page.waitForFunction(() => document.querySelector('.voice select')?.value === 'expresso/ex03-ex01_calm_001_channel2_1081s.wav');
  ok(voiceRelay.calls.some((c) => c.method === 'POST' && JSON.parse(c.body).voice === 'expresso/ex03-ex01_calm_001_channel2_1081s.wav'), 'settings voice: default box voice sent');
  // the reflex: Molty's own reactions while you talk, its engine, compare mode
  const rx = vc.getByRole('group', { name: 'reflex' });
  ok(await rx.getByRole('button', { name: 'off' }).getAttribute('aria-pressed') === 'true', 'settings reflex: off by default');
  ok((await rx.getByRole('button', { name: 'OpenAI' }).getAttribute('class')).includes('dim'), 'settings reflex: an engine without its key is dimmed');
  await rx.getByRole('button', { name: 'Jev' }).click();
  await rx.locator('button[aria-pressed="true"]', { hasText: 'Jev' }).waitFor();
  ok(voiceRelay.calls.some((c) => c.method === 'POST' && JSON.parse(c.body).reflex === 'jev'), 'settings reflex: engine sent to the relay');
  await vc.getByText('reacts with Jev', { exact: true }).waitFor();
  await vc.getByLabel('compare reflex engines').check();
  await vc.getByText('reacts with Jev ⠂ comparing every engine in the activity log').waitFor();
  ok(voiceRelay.calls.some((c) => c.method === 'POST' && JSON.parse(c.body).reflex_compare === true), 'settings reflex: compare sent to the relay');
  // liveliness: normal by default, lively sent to the relay and shown pressed
  const lv = vc.getByRole('group', { name: 'liveliness' });
  ok(await lv.getByRole('button', { name: 'normal' }).getAttribute('aria-pressed') === 'true', 'settings reflex: liveliness normal by default');
  await lv.getByRole('button', { name: 'lively' }).click();
  await lv.locator('button[aria-pressed="true"]', { hasText: 'lively' }).waitFor();
  ok(voiceRelay.calls.some((c) => c.method === 'POST' && JSON.parse(c.body).liveliness === 'lively'), 'settings reflex: liveliness sent to the relay');
  await rx.getByRole('button', { name: 'OpenAI' }).click();
  await vc.getByText('reacts with keywords (no key on the relay for OpenAI) ⠂ comparing every engine in the activity log').waitFor();
  ok(true, 'settings reflex: a keyless engine says it falls back to keywords');
  await vc.locator('summary', { hasText: 'Pair another voice box' }).click();
  ok((await vc.locator('code').innerText()) === 'molty-voice pair --relay https://molty-relay.example.workers.dev --device molty-k7m2w9c4ya --link-code K7QX2MPA', 'settings voice: pair command for the box');
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'settings voice: no horizontal scroll at 390px');
  await shot(page, '11b-settings-voice');
  await vc.getByRole('button', { name: 'Unpair voice box' }).click();
  await vc.getByRole('button', { name: 'Sure? Unpair' }).click();
  await vc.locator('p', { hasText: 'no voice box paired' }).waitFor();
  ok(voiceRelay.calls.some((c) => c.method === 'DELETE'), 'settings voice: unpair');

  // Activity: the relay's merged timeline, grouped by turn, through the link code from GET /api/link
  await page.goto(`${MOCK}/#/activity`);
  const turns = page.locator('.act-turn');
  await turns.first().waitFor({ timeout: 5000 });
  ok(activityRelay.calls.every((c) => c.code === 'K7QX2MPA'), 'activity: relay calls carry the current link code');
  ok(await turns.count() === 3, 'activity: three turns (one per job)');
  ok(await page.locator('.act-item', { hasText: 'molty disconnected ⠂ close 1006' }).count() === 1, 'activity: device disconnect with its close code');
  // the agent's name is the row's own column, the line says what happened (not the name twice)
  const pairRow = page.locator('.act-item', { hasText: 'Grokbot' }).filter({ hasText: 'paired' });
  ok(await pairRow.count() === 1 && !(await pairRow.innerText()).includes('Grokbot Grokbot'), 'activity: pairing shown');
  ok((await turns.first().locator(':scope > summary').innerText()).includes('listen'), 'activity: newest turn first');
  ok((await turns.nth(1).locator(':scope > summary').innerText()).includes('Grokbot'), 'activity: then the older ones');
  // the shake turn: its trace as a timeline, the webhook and the reply joined to it, its audio
  const shakeTurn = turns.first();
  ok((await shakeTurn.locator(':scope > summary').innerText()).includes('molty'), 'activity: a shake listen is molty’s own (not the webhook’s agent)');
  await shakeTurn.locator(':scope > summary').click();
  const tl = shakeTurn.locator('details.act-trace');
  ok((await tl.locator('summary').innerText()).includes('lost 3.2 s of audio (captured 5.4 s in 9.0 s)'), 'trace: summary leads with the lost audio');
  await tl.locator('summary').click();
  ok(await tl.getByRole('img', { name: 'turn timeline' }).isVisible(), 'trace: timeline picture');
  const sum = tl.getByLabel('trace summary');
  for (const t of ['18 samples clipped (mic gain 6)', '2 failed sends, 1 frame resent', 'shake noise trimmed 340 ms', 'weak Wi-Fi (-79 dBm)']) ok(await sum.getByText(t).isVisible(), `trace: summary "${t}"`);
  const evs = tl.getByLabel('turn events');
  for (const t of ['shake 8.2 g, 4 reversals', 'shake noise trimmed: 340 ms', 'frame 2 resent after 2 failed sends, waited 2.7 s', 'frames 3 never arrived',
    'end of turn: a mid-sentence pause held (last word “from”), held 1.2 s', 'utterance sent to the agents', 'webhook → Claude: 200 in 180 ms',
    'Claude answered (talk) 2.1 s after the utterance', 'Claude reports: received 120 ms ⠂ model 900 ms',
    'reflex jev: surprised 91% ⠂ other ⠂ 212 ms ⠂ 670 tok → shown (4 words)', 'reflex local: none 30% (happy below the gate) ⠂ other ⠂ 34 ms ⠂ 9 tok (4 words)']) ok(await evs.getByText(t, { exact: true }).isVisible(), `trace: event "${t}"`);
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'trace: no horizontal scroll at 390px');
  await shot(page, '11e-activity-trace');
  const dl = page.waitForEvent('download');
  await shakeTurn.getByRole('button', { name: 'download audio (wav)' }).click();
  const file = await dl;
  ok(file.suggestedFilename() === 'molty-j_U2hha2VUdXJuVHJj.wav', 'audio: saved as molty-<job>.wav');
  ok(activityRelay.audioCalls.length === 1 && activityRelay.audioCalls[0].code === 'K7QX2MPA' && activityRelay.audioCalls[0].url.endsWith(`/activity/audio/${encodeURIComponent(JC)}`) && !activityRelay.audioCalls[0].url.includes('K7QX2MPA'),
    'audio: fetched with the link code header, not in the URL');
  const ka = page.getByRole('group', { name: 'keep audio for debugging' });
  ok(await ka.getByRole('button', { name: 'off' }).getAttribute('aria-pressed') === 'true', 'keep audio: off by default');
  await ka.getByRole('button', { name: 'on' }).click();
  await ka.locator('button[aria-pressed="true"]', { hasText: 'on' }).waitFor();
  ok(activityRelay.calls.some((c) => c.method === 'POST' && JSON.parse(c.body).keep_audio === true), 'keep audio: on sent to the relay');
  await shakeTurn.locator(':scope > summary').click();   // fold it back: the rest of the walk-through reads the others
  const done = turns.filter({ has: page.locator('summary', { hasText: 'Claude' }) });
  await done.locator(':scope > summary').click();
  ok(await done.getByLabel('stage timings').getByText('say · tts_first_byte').isVisible(), 'activity: expanded turn shows stage timings');
  ok(await done.getByText('speaks with voice box ⠂ listens with voice box').isVisible(), 'activity: voice provider');
  ok(await done.getByText('“yes please, green tea”').isVisible(), 'activity: heard text');
  ok(await done.getByLabel('device log').getByText('mic gain clipped twice').isVisible(), 'activity: the turn’s device line');
  ok(await done.locator('.act-line.warn').count() === 1, 'activity: W device line highlighted');
  await wait(3500);   // one live poll (since=next) must keep the expanded turn open
  ok(activityRelay.calls.some((c) => c.method === 'GET' && c.since === 'a14'), 'activity: live poll asks since the last id');
  ok(await done.getAttribute('open') !== null, 'activity: an expanded turn survives the live poll');
  await shot(page, '11c-activity');
  await page.getByRole('button', { name: 'errors only' }).click();
  await page.locator('.act-turn', { hasText: 'Claude' }).waitFor({ state: 'detached' });
  ok(await page.locator('.act-turn').count() === 1 && (await page.locator('.act-turn').innerText()).includes('voice_box_offline'), 'activity: errors only → just the failed turn');
  ok(await page.locator('.act-item').count() === 0, 'activity: errors only hides the ok events');
  await page.getByRole('button', { name: 'errors only' }).click();
  await page.getByLabel('agent', { exact: true }).selectOption('Grokbot');
  ok(await page.locator('.act-turn').count() === 1 && await page.locator('.act-item', { hasText: 'Claude' }).count() === 0, 'activity: agent filter');
  await page.getByLabel('agent', { exact: true }).selectOption('');
  const tr = page.getByRole('group', { name: 'log transcripts' });
  ok(await tr.getByRole('button', { name: 'on' }).getAttribute('aria-pressed') === 'true', 'activity: transcripts on by default');
  await tr.getByRole('button', { name: 'off' }).click();
  await tr.locator('button[aria-pressed="true"]', { hasText: 'off' }).waitFor();
  ok(activityRelay.calls.some((c) => c.method === 'POST' && JSON.parse(c.body).transcripts === false), 'activity: transcripts off sent to the relay');
  ok(await page.getByRole('group', { name: 'keep audio for debugging' }).getByRole('button', { name: 'on' }).isDisabled(), 'keep audio: can’t be turned on while transcripts are off');
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'activity: no horizontal scroll at 390px');
  await shot(page, '11d-activity-errors');
  // speech habits: the relay's calibration, read with the link code; "start over" resets it
  const habits = page.getByLabel('speech habits');
  await habits.waitFor();
  ok(speechRelay.calls.every((c) => c.code === 'K7QX2MPA'), 'speech habits: relay calls carry the current link code');
  ok(await habits.getByText('14 words, 4.6 s (usually 8–22 words)').isVisible(), 'speech habits: a typical turn');
  ok(await habits.getByText('tuned: every 5 words or 2 s, none in the first 3 s').isVisible(), 'speech habits: the partial policy');
  ok(await habits.getByText(/6 played of 10 drafted \(70 %\)/).isVisible(), 'speech habits: the draft record');
  await page.getByRole('button', { name: 'start over' }).click();
  await habits.getByText('none drafted yet').waitFor();
  ok(speechRelay.calls.some((c) => c.method === 'DELETE'), 'speech habits: start over resets on the relay');
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'speech habits: no horizontal scroll at 390px');

  await page.goto(`${MOCK}/#/molty`);
  await page.locator('.creature img').first().waitFor();
  ok(await page.locator('.creature').count() === 29, 'settings molty: 29 Moltys with thumbnails');
  await page.locator('.creature', { hasText: 'Owl' }).click();
  await page.getByRole('button', { name: 'send Owl to molty' }).click();
  await page.locator('.note.ok', { hasText: 'moving in' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/file?name=cute-owl' && l.head === 'MLTY'), 'settings molty: upload');
  const ringRow = page.getByRole('group', { name: 'listening ring colour' });
  ok(await ringRow.getByRole('button', { name: 'accent (default)' }).getAttribute('aria-pressed') === 'true', 'settings molty: listening ring defaults to the accent');
  await ringRow.getByRole('button', { name: 'violet' }).click();
  await ringRow.locator('button[aria-pressed="true"]', { hasText: 'violet' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).ring_color === '#b197fc'), 'settings molty: ring colour sent to the device');
  await page.goto(`${MOCK}/#/voice`);
  const listenRow = page.getByRole('group', { name: 'listen window' });
  ok(await listenRow.getByRole('button', { name: '30 s (default)' }).getAttribute('aria-pressed') === 'true', 'settings molty: listen window defaults to 30 s');
  await listenRow.getByRole('button', { name: 'unlimited' }).click();
  await listenRow.locator('button[aria-pressed="true"]', { hasText: 'unlimited' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).listen_ms === 0), 'settings molty: unlimited listen window sent to the device (listen_ms 0)');
  await listenRow.getByRole('button', { name: '10 s' }).click();
  await listenRow.locator('button[aria-pressed="true"]', { hasText: '10 s' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).listen_ms === 10000), 'settings molty: 10 s listen window sent to the device');
  const ackRow = page.getByRole('group', { name: 'acknowledge' });
  ok(await ackRow.getByRole('button', { name: 'chirp + thinking (default)' }).getAttribute('aria-pressed') === 'true', 'settings molty: acknowledge defaults to chirp + thinking');
  await ackRow.getByRole('button', { name: 'thinking only' }).click();
  await ackRow.locator('button[aria-pressed="true"]', { hasText: 'thinking only' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).ack === 'thinking'), 'settings molty: "thinking only" acknowledgement sent to the device (ack thinking)');
  await ackRow.getByRole('button', { name: 'off' }).click();
  await ackRow.locator('button[aria-pressed="true"]', { hasText: 'off' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).ack === 'off'), 'settings molty: acknowledgement off sent to the device (ack off)');
  await ackRow.getByRole('button', { name: 'chirp + thinking (default)' }).click();
  await ackRow.locator('button[aria-pressed="true"]', { hasText: 'chirp + thinking' }).waitFor();
  const fillerRow = page.getByRole('group', { name: 'filler' });
  ok(await fillerRow.getByRole('button', { name: 'after 2.5 s (default)' }).getAttribute('aria-pressed') === 'true', 'settings molty: filler defaults to after 2.5 s');
  await fillerRow.getByRole('button', { name: 'off' }).click();
  await fillerRow.locator('button[aria-pressed="true"]', { hasText: 'off' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).filler_ms === 0), 'settings molty: filler off sent to the device (filler_ms 0)');
  await fillerRow.getByRole('button', { name: 'after 1.5 s' }).click();
  await fillerRow.locator('button[aria-pressed="true"]', { hasText: 'after 1.5 s' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).filler_ms === 1500), 'settings molty: 1.5 s filler delay sent to the device (filler_ms 1500)');
  ok(JSON.stringify(await fillerRow.getByRole('button').allInnerTexts()) === JSON.stringify(['off', 'after 0.5 s', 'after 0.8 s', 'after 1.5 s', 'after 2.5 s (default)', 'after 3.5 s']), 'settings molty: filler chips are off, 0.5, 0.8, 1.5, 2.5 (default), 3.5 s');
  for (const [label, ms] of [['after 0.5 s', 500], ['after 0.8 s', 800]]) {
    await fillerRow.getByRole('button', { name: label, exact: true }).click();
    await fillerRow.locator('button[aria-pressed="true"]', { hasText: label }).waitFor();
    ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).filler_ms === ms), `settings molty: ${label} filler delay sent to the device (filler_ms ${ms})`);
    const st = await (await fetch(`${MOCK}/api/creature/state`)).json();
    ok(st.filler_ms === ms, `settings molty: the device keeps filler_ms ${ms}`);
  }
  await fillerRow.getByRole('button', { name: 'after 2.5 s (default)' }).click();
  await fillerRow.locator('button[aria-pressed="true"]', { hasText: 'after 2.5 s' }).waitFor();
  await page.goto(`${MOCK}/#/molty`);
  const shakeRow = page.getByRole('group', { name: 'shake to talk' });
  ok(await shakeRow.getByRole('button', { name: 'on (default)' }).getAttribute('aria-pressed') === 'true', 'settings molty: shake to talk defaults to on');
  await shakeRow.getByRole('button', { name: 'off' }).click();
  await shakeRow.locator('button[aria-pressed="true"]', { hasText: 'off' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).shake_talk === false), 'settings molty: shake to talk off sent to the device (shake_talk false)');
  await shakeRow.getByRole('button', { name: 'on (default)' }).click();
  await shakeRow.locator('button[aria-pressed="true"]', { hasText: 'on' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).shake_talk === true), 'settings molty: shake to talk back on (shake_talk true)');
  const idleRow = page.getByRole('group', { name: 'idle life' });
  ok(await idleRow.getByRole('button', { name: 'lively (default)' }).getAttribute('aria-pressed') === 'true', 'settings molty: idle life defaults to lively');
  await idleRow.getByRole('button', { name: 'calm' }).click();
  await idleRow.locator('button[aria-pressed="true"]', { hasText: 'calm' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).idle_life === 'calm'), 'settings molty: calm idle life sent to the device (idle_life calm)');
  await idleRow.getByRole('button', { name: 'off' }).click();
  await idleRow.locator('button[aria-pressed="true"]', { hasText: 'off' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).idle_life === 'off'), 'settings molty: idle life off sent to the device (idle_life off)');
  await idleRow.getByRole('button', { name: 'lively (default)' }).click();
  await idleRow.locator('button[aria-pressed="true"]', { hasText: 'lively' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).idle_life === 'lively'), 'settings molty: idle life back to lively (idle_life lively)');
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'settings molty: no horizontal scroll at 390px');
  await shot(page, '12-settings-molty');

  await mock('config', { file: false });
  await page.getByRole('button', { name: 'send Owl to molty' }).click();
  await page.locator('.note.warn', { hasText: "can't take new moltys yet" }).waitFor();
  ok(true, 'settings molty: creature-file 404 → graceful notice');

  // pre-contract firmware: link without pair/last_pair still waits; no link at all → skip offered
  await mock('legacy', { on: true });
  await page.goto(`${MOCK}/#/setup/agent`);
  await page.locator('.wait', { hasText: 'Waiting for your agent' }).waitFor();
  await wait(1500);
  ok(/#\/setup\/agent/.test(page.url()), 'legacy link (no pair fields): agent step waits, no crash');
  await mock('config', { link: false });
  await page.reload();
  await page.locator('.setup .bubble', { hasText: 'firmware update' }).waitFor({ timeout: 5000 });
  await page.getByRole('button', { name: 'Skip for now' }).click();
  await page.locator('.setup .bubble', { hasText: '!' }).waitFor();
  ok((await bubble(page)).includes('Give me a brain any time'), 'no /api/link → "Skip for now" → done');
  await shot(page, '13-link-unsupported-done');
  await mock('config', { reset: true, link: true, file: true });
  await page.context().close();
}

// ── firmware update on the status page ──────────────────────────────────────
{
  await mock('config', { reset: true });
  const page = await newPage('update');
  await page.goto(`${MOCK}/#/status`);
  const card = page.locator('.update .card');
  await card.locator('b', { hasText: /^update available: \d+\.\d+\.\d+$/ }).waitFor({ timeout: 10000 });
  ok(await card.locator('a', { hasText: /^molty-.*-ota\.bin$/ }).count() === 1, 'update: older firmware gets the manual path (download link + install)');
  await card.locator('summary', { hasText: 'what’s new' }).click();
  ok((await card.locator('pre').innerText()).includes('tap an answer'), 'update: what’s new shows the release notes');
  await page.setInputFiles('input[type=file]', { name: 'molty-ota.bin', mimeType: 'application/octet-stream', buffer: Buffer.alloc(4096, 0xe9) });
  await card.getByRole('button', { name: 'Install' }).click();
  await card.getByRole('button', { name: 'Sure? Molty restarts' }).click();
  await card.locator('.note', { hasText: 'restarting' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/ota' && l.bytes === 4096), 'update: the chosen image is uploaded to /api/ota');
  await noOverflow(page, 'update manual');
  await shot(page, '14-update-manual');
  await mock('config', { reset: true, update: true });
  await page.reload();
  await card.getByRole('button', { name: 'Update' }).click();
  await card.getByRole('button', { name: 'Sure? Update now' }).click();
  await page.locator('.update .note', { hasText: /updated to \d+\.\d+\.\d+ ✓/ }).waitFor({ timeout: 15000 });
  const post = (await mockLog()).find((l) => l.path === '/api/update');
  ok(!!post && /releases\/download\/v.*-ota\.bin/.test(JSON.parse(post.body).url) && JSON.parse(post.body).sha256 === 'd'.repeat(64), 'update: self-updating firmware gets Update, which posts the relay’s official image + sha256');
  ok((await page.locator('.facts dd').nth(1).innerText()).startsWith((await (await fetch(`${MOCK}/__mock/release`)).json()).latest.version), 'update: status shows the new firmware afterwards');
  await shot(page, '15-update-self');
  // the new image does not stick: Molty comes back on the old firmware -> rolled back, Update offered again
  await mock('config', { reset: true, update: true, rollback: true });
  await page.reload();
  await card.getByRole('button', { name: 'Update' }).click();
  await card.getByRole('button', { name: 'Sure? Update now' }).click();
  await card.locator('.note', { hasText: /did not stick/ }).waitFor({ timeout: 15000 });
  ok(await card.getByRole('button', { name: 'Update' }).count() === 1, 'update: back on the old firmware ends the install as rolled back and offers Update again');
  await shot(page, '16-update-rolled-back');
  // the download fails (a transient TLS error): the failure shows and Update is offered again
  await mock('config', { reset: true, update: true, fail: true });
  await page.reload();
  await card.getByRole('button', { name: 'Update' }).click();
  await card.getByRole('button', { name: 'Sure? Update now' }).click();
  await card.locator('.note', { hasText: /update failed: tls/ }).waitFor({ timeout: 15000 });
  ok(await card.getByRole('button', { name: 'Update' }).count() === 1, 'update: a failed download shows why and offers Update again');
  await mock('config', { reset: true });
  await page.context().close();
}

// ── real device: read-only (+ harmless, restored fluid orders) ───────────────
if (REAL) {
  const page = await newPage('real', { viewport: { width: 1100, height: 900 } });
  await page.goto(`${MOCK}/?device=${encodeURIComponent(REAL)}#/status`);
  await page.locator('.facts dd.hi', { hasText: /^molty-/ }).waitFor({ timeout: 15000 });
  await page.locator('.pv-status', { hasText: 'live' }).waitFor({ timeout: 15000 });
  await shot(page, '20-status-real');
  await page.goto(`${MOCK}/?device=${encodeURIComponent(REAL)}#/agent`);
  await page.locator('.msg pre', { hasText: 'molty-' }).waitFor({ timeout: 10000 });
  await shot(page, '21-agent-real');
  await page.goto(`${MOCK}/?device=#/status`); // clear the remembered override
  await page.context().close();
}

await browser.close();
console.log(checks.join('\n'));
console.log(errors.length ? `CONSOLE ERRORS (${errors.length}):\n` + errors.join('\n') : 'console errors: 0');
if (errors.length) process.exitCode = 1;
