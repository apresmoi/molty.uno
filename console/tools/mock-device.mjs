// Mock Molty: serves dist/index.html at "/" plus the device API the console uses,
// so write flows (Wi-Fi connect/forget, creature upload, relay change) can be
// exercised without touching a real device.
//
//   node tools/mock-device.mjs [port=8170]
//   GET  /__mock/log            -> recorded non-GET requests (path + body)
//   POST /__mock/config {link,file,reset,wifi:'none'}  -> toggle features (false = route 404s like old
//        firmware); reset restores Wi-Fi + link; wifi:'none' starts with no saved network
//   POST /__mock/pair {agent_name, code}   -> an agent asks to pair: GET /api/link shows `pair`
//   POST /__mock/decide {ok, reason}       -> the human tapped (ok) / it failed: pair → last_pair (seq+1)
//   POST /__mock/legacy {on}               -> GET /api/link omits pair/last_pair (pre-contract firmware)
//   GET  /__mock/release?board=            -> what the relay's public GET /v1/releases/latest would answer
//        (a release one minor above VERSION); e2e routes the console's relay call here.
//        config {update:true} = firmware that can update itself (GET/POST /api/update);
//        {rollback:true} = its new image does not stick: it restarts on the old firmware.
//        {fail:true} = the download fails (state failed, error tls); Molty keeps its firmware.
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';

const port = +(process.argv[2] ?? 8170);
const dist = new URL('../dist/index.html', import.meta.url);
const VERSION = readFileSync(new URL('../../VERSION', import.meta.url), 'utf8').trim();
const W = 412;

const cfg = { link: true, file: true, update: false, rollback: false, fail: false };
const BOARD = 'waveshare-esp32-s3-touch-lcd-1.46';
const NEXT = VERSION.replace(/^(\d+)\.(\d+)\..*$/, (_, a, b) => `${a}.${+b + 1}.0`);
let fw = VERSION;
let upd = { state: 'idle' };
/** The device's boot as GET /api/info reports it (a restart bumps the count, resets the uptime). */
let boots = 74, bootAt = Date.now(), upBase = 3_725_000;
const restart = () => { boots++; bootAt = Date.now(); upBase = 0; };
const release = (board) => {
  const tag = `v${NEXT}`, name = `molty-${NEXT}-${BOARD}-ota.bin`;
  return { ok: true, release_check: 'fresh',
    latest: { version: NEXT, tag, prerelease: false, published_at: '2026-11-01T10:00:00Z', notes_url: `https://github.com/apresmoi/molty.uno/releases/tag/${tag}`, notes: `## ${tag} — 2026-11-01\n- Molty asks you things: tap an answer\n- Molty can update itself` },
    board: board === BOARD ? { name: BOARD, ota: { url: `https://github.com/apresmoi/molty.uno/releases/download/${tag}/${name}`, size: 1500000, sha256: 'd'.repeat(64) } } : null,
    features: [{ feature: 'molty_ask', cap: 'ask', needs: NEXT }] };
};
const log = [];
const WIFI0 = { state: 'connected', ssid: 'home-wifi', ip: '192.168.1.32', rssi: -61 };
const wifi = { ...WIFI0 };
const fluid = {
  ok: true, creature: 'fluid', state: 'blob', autoplay: false, text: 'hello, i am molty. i breathe in ascii.', wpm: 300,
  palette: 'pastel', ring_color: 'accent', listen_ms: 30000, ack: 'chirp', shake_talk: true, word: '', word_idx: 0, nwords: 7, fps: 33.1,
};
let current = 'fluid';
const LINK0 = { device_id: 'molty-k7m2w9c4ya', link_code: 'K7QX2MPA', relay_url: 'wss://molty-relay.example.workers.dev', connected: true, auth_failed: false, close_code: 0,
  pair: null, last_pair: { seq: 3, agent_name: 'an-older-agent', ok: true } };
const link = structuredClone(LINK0);
let legacy = false;
const linkBody = () => { if (!legacy) return link; const { pair, last_pair, ...rest } = link; return rest; };
let t0 = Date.now();

const PAL = { green: [124, 242, 154], amber: [255, 179, 71], pastel: [201, 182, 255] };
function frame() {
  const t = (Date.now() - t0) / 1000;
  const [r, g, b] = PAL[fluid.palette] ?? PAL.pastel;
  const hdr = Buffer.from(`P6 ${W} ${W} 255\n`);
  const px = Buffer.alloc(W * W * 3);
  const cell = 8;
  for (let cy = 0; cy < W / cell; cy++) for (let cx = 0; cx < W / cell; cx++) {
    const x = (cx * cell - W / 2) / (W / 2), y = (cy * cell - W / 2) / (W / 2);
    const d = Math.hypot(x, y);
    const v = Math.max(0, 1 - d * 1.6 + 0.15 * Math.sin(8 * d - t * 2) + 0.1 * Math.sin(5 * x + t));
    if (v < 0.05 || d > 1) continue;
    for (let yy = 1; yy < cell - 1; yy++) for (let xx = 2; xx < cell - 2; xx++) {
      if (((xx + yy + cx + cy) & 1) === 0) continue; // dotted, braille-ish
      const o = ((cy * cell + yy) * W + cx * cell + xx) * 3;
      px[o] = r * v; px[o + 1] = g * v; px[o + 2] = b * v;
    }
  }
  return Buffer.concat([hdr, px]);
}

const json = (res, obj, status = 200) => {
  res.writeHead(status, { 'content-type': 'application/json', 'access-control-allow-origin': '*', 'cache-control': 'no-store' });
  res.end(JSON.stringify(obj));
};
const notFound = (res) => json(res, { ok: false, error: 'not_found' }); // same as the firmware's 404 handler
const body = (req) => new Promise((ok) => { const c = []; req.on('data', (d) => c.push(d)); req.on('end', () => ok(Buffer.concat(c))); });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const p = url.pathname;
  const raw = req.method === 'GET' ? null : await body(req);
  if (raw && !p.startsWith('/__mock')) log.push({ method: req.method, path: p + url.search, ctype: req.headers['content-type'], body: raw.toString('utf8').slice(0, 400), head: raw.subarray(0, 4).toString('latin1'), bytes: raw.length });
  const b = () => { try { return JSON.parse(raw.toString()); } catch { return {}; } };

  if (p === '/' || p === '/index.html') {
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
    return res.end(readFileSync(dist));
  }
  if (p === '/__mock/log') return json(res, log);
  if (p === '/__mock/config') {
    const c = b();
    if (c.reset) { fw = VERSION; upd = { state: 'idle' }; cfg.update = false; cfg.rollback = false; cfg.fail = false; Object.assign(wifi, WIFI0); Object.assign(link, structuredClone(LINK0)); legacy = false; current = 'fluid'; delete c.reset; }
    if (c.wifi === 'none') Object.assign(wifi, { state: 'setup_needed', ssid: undefined, ip: undefined, rssi: undefined });
    delete c.wifi;
    Object.assign(cfg, c); log.length = 0; return json(res, { ok: true, cfg });
  }
  if (p === '/__mock/pair') { const { agent_name = 'Grok', code = '4821' } = b(); link.pair = { agent_name, code, expires_in_s: 60 }; return json(res, { ok: true, link }); }
  if (p === '/__mock/decide') {
    const { ok = true, reason } = b();
    link.last_pair = { seq: (link.last_pair?.seq ?? 0) + 1, agent_name: link.pair?.agent_name ?? 'Grok', ok, ...(ok ? {} : { reason: reason ?? 'denied' }) };
    link.pair = null;
    return json(res, { ok: true, link });
  }
  if (p === '/__mock/legacy') { legacy = !!b().on; return json(res, { ok: true }); }
  if (p === '/__mock/release') return json(res, release(url.searchParams.get('board')));

  switch (`${req.method} ${p}`) {
    case 'GET /api/info':
      return json(res, { ok: true, chip: 'esp32-s3', mac: '30:ed:a0:2a:3b:b0', flash_mb: 16, psram_mb: 8, free_heap: 3451664, free_psram: 5300000,
        uptime_ms: Date.now() - bootAt + upBase, reset_reason: 'sw', boot_count: boots, idf_version: 'v5.5', board: BOARD, fw, fw_build: 'Oct 25 2026T04:51:38' });
    case 'GET /api/status': return json(res, { ok: true, ...wifi });
    case 'GET /api/scan':
      await sleep(900);
      return json(res, { ok: true, networks: [
        { ssid: 'home-wifi', rssi: -58, open: false }, { ssid: 'FRITZ!Box 7590 QX', rssi: -71, open: false },
        { ssid: 'cafe-libre', rssi: -80, open: true }, { ssid: 'home-wifi', rssi: -88, open: false } ] });
    case 'POST /api/set_creds': {
      const { ssid, password } = b();
      await sleep(1200);
      if (password === 'wrong') return json(res, { ok: false, error: 'connect_failed', wifi_status: 4 });
      Object.assign(wifi, { state: 'connected', ssid, ip: '192.168.1.40', rssi: -60 });
      return json(res, { ok: true, ip: wifi.ip, rssi: wifi.rssi });
    }
    case 'POST /api/forget': Object.assign(wifi, { state: 'setup_needed', ssid: undefined, ip: undefined, rssi: undefined }); return json(res, { ok: true });
    case 'GET /api/creature': return json(res, { ok: true, current, available: ['eye', 'claude', 'glyph', 'reader', 'fluid', 'ui'] });
    case 'POST /api/creature': { const n = b().name; if (!['eye', 'claude', 'glyph', 'reader', 'fluid', 'ui'].includes(n)) return json(res, { ok: false, error: 'not_found' }); current = n; return json(res, { ok: true, current, available: [] }); }
    case 'GET /api/creature/state': return json(res, fluid);
    case 'POST /api/creature/state': {
      const o = b();
      if (o.state === 'autoplay') { fluid.autoplay = true; } else if (o.state) { fluid.autoplay = false; fluid.state = o.state; }
      if (o.ring_color !== undefined && !(o.ring_color === 'accent' || /^#[0-9a-fA-F]{6}$/.test(o.ring_color))) return json(res, { ok: false, error: 'invalid_args' }, 400);
      if (o.listen_ms !== undefined && !(Number.isInteger(o.listen_ms) && (o.listen_ms === 0 || (o.listen_ms >= 5000 && o.listen_ms <= 30000)))) return json(res, { ok: false, error: 'invalid_args' }, 400);
      if (o.ack !== undefined && !['chirp', 'thinking', 'off'].includes(o.ack)) return json(res, { ok: false, error: 'invalid_args' }, 400);
      if (o.shake_talk !== undefined && typeof o.shake_talk !== 'boolean') return json(res, { ok: false, error: 'invalid_args' }, 400);
      for (const k of ['text', 'wpm', 'palette', 'ring_color', 'listen_ms', 'ack', 'shake_talk']) if (o[k] !== undefined) fluid[k] = o[k];
      if (o.text) fluid.nwords = o.text.split(/\s+/).filter(Boolean).length;
      return json(res, fluid);
    }
    case 'GET /api/framebuffer':
      res.writeHead(200, { 'content-type': 'image/x-portable-pixmap', 'access-control-allow-origin': '*', 'cache-control': 'no-store' });
      return res.end(frame());
    case 'POST /api/creature/file':
      if (!cfg.file) return notFound(res);
      if (raw.length > 16384) return json(res, { ok: false, error: 'bad_size_max_16kb' });
      if (raw.subarray(0, 4).toString('latin1') !== 'MLTY') return json(res, { ok: false, error: 'bad_magic' });
      return json(res, { ok: true, bytes: raw.length });
    case 'GET /api/update': return cfg.update ? json(res, { ok: true, ...upd }) : notFound(res);
    case 'POST /api/update': {
      if (!cfg.update) return notFound(res);
      const { version, url: u } = b();
      if (!/^https:\/\/github\.com\/apresmoi\/molty\.uno\/releases\/download\//.test(u ?? '')) return json(res, { ok: false, error: 'bad_url' }, 400);
      upd = { state: 'downloading', version, progress: 0 };
      if (cfg.fail) { setTimeout(() => (upd = { state: 'failed', version, error: 'tls' }), 600); return json(res, { ok: true, state: 'downloading' }, 202); }
      setTimeout(() => (upd = { state: 'flashing', version }), 600);
      setTimeout(() => (upd = { state: 'rebooting', version }), 900);
      setTimeout(() => { upd = { state: 'idle' }; restart(); if (!cfg.rollback) fw = version; }, 1200);
      return json(res, { ok: true, state: 'downloading' }, 202);
    }
    case 'POST /api/ota':
      if (raw.length < 32) return json(res, { ok: false, error: 'empty' });
      return json(res, { ok: true, written: raw.length, slot: 'ota_1', reboot: true });
    case 'GET /api/link': return cfg.link ? json(res, linkBody()) : notFound(res);
    case 'POST /api/link': if (!cfg.link) return notFound(res); link.relay_url = b().relay_url ?? link.relay_url; link.connected = false; setTimeout(() => (link.connected = true), 1500); return json(res, linkBody());
  }
  return notFound(res);
}).listen(port, () => console.log(`mock molty on http://localhost:${port}`));
