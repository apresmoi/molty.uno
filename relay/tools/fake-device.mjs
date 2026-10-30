#!/usr/bin/env node
// Fake Molty device for local e2e runs. Speaks the device side of
// docs/AGENT-LINK.md: WSS /v1/device?id=..., hello with device_key, auto-
// accepts pairing, answers commands, plays (counts) say audio and streams a
// synthetic mic tone for listen. Emits one JSON line per notable event on stdout.
//
//   node tools/fake-device.mjs --relay ws://127.0.0.1:8787 --id molty-k7m2q9xa4c \
//        --key-file /tmp/k --link-file /tmp/link [--reject] [--once]
import { randomBytes } from "node:crypto";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { parseArgs } from "node:util";

const { values: o } = parseArgs({
  options: {
    relay: { type: "string", default: "ws://127.0.0.1:8787" },
    id: { type: "string", default: "molty-k7m2q9xa4c" },
    "key-file": { type: "string" },
    key: { type: "string" },
    "link-file": { type: "string" },
    reject: { type: "boolean", default: false },
    once: { type: "boolean", default: false },
  },
});

const LINK_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ23456789";
const newLink = () => Array.from(randomBytes(8), (b) => LINK_ALPHABET[b % LINK_ALPHABET.length]).join("");
const out = (event, data = {}) => process.stdout.write(JSON.stringify({ event, at: Date.now(), ...data }) + "\n");

let key = o.key;
if (!key) {
  if (o["key-file"] && existsSync(o["key-file"])) key = readFileSync(o["key-file"], "utf8").trim();
  else {
    key = randomBytes(32).toString("base64");
    if (o["key-file"]) writeFileSync(o["key-file"], key, { mode: 0o600 });
  }
}
let linkCode = newLink();
const publishLink = () => {
  if (o["link-file"]) writeFileSync(o["link-file"], linkCode);
  out("link_code", { link_code: linkCode });
};
publishLink();

const started = Date.now();
const state = { creature: "fluid", state: "idle", mood: "idle", volume: 12, blink: 0, color: "#70d8a0" };
const sayStreams = new Map(); // job -> {frames, bytes, first, ms, ended}

function connect() {
  const ws = new WebSocket(`${o.relay.replace(/\/$/, "")}/v1/device?id=${o.id}`);
  ws.binaryType = "arraybuffer";
  current = ws;
  let pinger;
  const send = (obj) => ws.readyState === 1 && ws.send(JSON.stringify(obj));

  ws.addEventListener("open", () => {
    out("open");
    send({ t: "hello", id: o.id, key, fw: "fake-0.1", caps: ["fluid", "say", "listen", "show", "mood", "creature"], link_code: linkCode });
    pinger = setInterval(() => send({ t: "ping" }), 20000);
  });

  ws.addEventListener("message", (ev) => {
    if (typeof ev.data !== "string") return onAudio(new Uint8Array(ev.data), send);
    const f = JSON.parse(ev.data);
    if (f.t === "welcome") return out("welcome");
    if (f.t === "pong") return;
    if (f.t === "pair.request") {
      out("pair.request", { agent_name: f.agent_name, code: f.code });
      setTimeout(() => {
        const ok = !o.reject;
        if (ok) linkCode = newLink();
        send({ t: "pair.result", req: f.req, ok, ...(ok ? { link_code: linkCode } : {}) });
        if (ok) publishLink();
        out("pair.result", { ok });
      }, 300);
      return;
    }
    if (f.t === "cmd") return onCmd(f, send);
  });

  ws.addEventListener("close", (ev) => {
    clearInterval(pinger);
    out("close", { code: ev.code, reason: ev.reason });
    if (!o.once) setTimeout(connect, 1000);
    else process.exit(0);
  });
  ws.addEventListener("error", () => out("error"));
}

function onCmd(f, send) {
  const reply = (ok, result = {}, error) => send(ok ? { t: "res", id: f.id, ok: true, result } : { t: "res", id: f.id, ok: false, error });
  out("cmd", { name: f.name, args: f.args });
  const a = f.args ?? {};
  switch (f.name) {
    case "status":
      return reply(true, { fw: "fake-0.1", uptime: Math.round((Date.now() - started) / 1000), creature: state.creature, state: state.state, mood: state.mood, fps: 30, rssi: -52 });
    case "state":
      Object.assign(state, a);
      return reply(true, { ...state });
    case "mood":
      state.mood = a.mood;
      return reply(true, { mood: a.mood });
    case "show": {
      // Test hook: show "evt:<type>" makes the fake device emit that agent event (as if touched).
      const sample = { shake: { peak_g: 2.4, reversals: 5, ms: 900 }, tap: { x: 0.5, y: 0.5 }, button: { id: "accept" } };
      const m = /^evt:(\w+)$/.exec(String(a.text));
      if (m && sample[m[1]]) setTimeout(() => send({ t: "evt", name: m[1], data: sample[m[1]] }), 10);
      return reply(true, { reader: "playing", words: String(a.text).split(/\s+/).length, wpm: a.wpm });
    }
    case "volume":
      state.volume = a.volume;
      return reply(true, { volume: a.volume });
    case "diag":
      return reply(true, { uptime_ms: Date.now() - started, frames: { sim: { fps: 30, p95_ms: 34 }, panel: { fps: 30 } }, heap: { internal: { free: 50000 } }, ...(a.timeline ? { timeline: [] } : {}) });
    case "log":
      return reply(true, { next: (a.since ?? 0) + 1, text: "0 0.001 EVT boot fake\n" });
    case "imu":
      return reply(true, { ax: 0.01, ay: -0.02, az: 0.99, gx: 0.1, gy: 0, gz: -0.1, ms: Date.now() - started });
    case "say":
      sayStreams.set(a.job, { frames: 0, bytes: 0, first: 0, ms: a.ms, text: a.text, lastSeq: -1 });
      state.mood = "speaking";
      return reply(true, {});
    case "listen":
      reply(true, {});
      return streamMic(a.job, a.ms, send);
    default:
      return reply(false, {}, "unknown_command");
  }
}

function onAudio(bytes, send) {
  const nl = bytes.indexOf(10);
  const header = JSON.parse(new TextDecoder().decode(nl < 0 ? bytes : bytes.subarray(0, nl)));
  const payload = nl < 0 ? new Uint8Array(0) : bytes.subarray(nl + 1);
  const st = sayStreams.get(header.s);
  if (!st) return out("audio.unexpected", { s: header.s });
  if (header.kind !== "pcm16" || header.rate !== 16000 || payload.byteLength > 16384 || nl > 512) out("audio.bad_frame", { header });
  if (header.seq !== st.lastSeq + 1) out("audio.seq_gap", { expected: st.lastSeq + 1, got: header.seq });
  st.lastSeq = header.seq;
  if (!st.first) st.first = Date.now();
  st.frames += 1;
  st.bytes += payload.byteLength;
  if (header.end) {
    const playedMs = Math.round((st.bytes / 2 / 16000) * 1000);
    out("say.audio", { job: header.s, frames: st.frames, bytes: st.bytes, played_ms: playedMs, announced_ms: st.ms, stream_ms: Date.now() - st.first });
    // "Playback" finishes when the ring buffer drains.
    const left = Math.max(0, st.first + playedMs - Date.now());
    setTimeout(() => {
      send({ t: "evt", name: "say.done", data: { job: header.s } });
      state.mood = "idle";
      sayStreams.delete(header.s);
    }, left);
  }
}

function streamMic(job, ms, send) {
  // 440 Hz tone at 0.3 amplitude, 320 ms frames, paced in real time.
  const total = Math.round((ms * 16000) / 1000);
  const chunk = 5120;
  let seq = 0;
  let sent = 0;
  const tick = () => {
    const n = Math.min(chunk, total - sent);
    const pcm = new Int16Array(n);
    for (let i = 0; i < n; i++) pcm[i] = Math.round(0.3 * 32767 * Math.sin((2 * Math.PI * 440 * (sent + i)) / 16000));
    sent += n;
    const end = sent >= total;
    const head = new TextEncoder().encode(JSON.stringify({ s: job, seq: seq++, kind: "pcm16", rate: 16000, end }));
    const frame = new Uint8Array(head.length + 1 + pcm.byteLength);
    frame.set(head);
    frame[head.length] = 10;
    frame.set(new Uint8Array(pcm.buffer), head.length + 1);
    wsSend(frame);
    if (end) out("listen.audio", { job, frames: seq, samples: sent });
    else setTimeout(tick, (chunk * 1000) / 16000);
  };
  const wsSend = (frame) => current?.readyState === 1 && current.send(frame);
  setTimeout(tick, 50);
  void send;
}

let current; // the live socket, used by the mic streamer
connect();
process.on("SIGTERM", () => process.exit(0));
