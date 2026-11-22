// A press by accident: Molty listened, nobody spoke. The relay's voice check (core/voiced.ts) on the
// recorded audio, and the transcript as a second gate, end it as no_speech — no STT, no utterance to
// any agent, no first words, no filler, no thinking — however the listen ended (a held button's
// release here: the device VAD is off for it, so its end says nothing about speech).
import { describe, expect, it } from "vitest";
import type { AgentEvent } from "../src/core/events.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { DEVICE, type FakeSocket, LINK, connect, flush, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300 };
const CAPS = ["say", "listen", "listen_stop", "listen_window", "ack", "shake_talk"];
const RATE = 16000;

/** 1.5 s of a quiet room (seeded noise, about −62 dBFS), with a voiced vowel at 0.5 s when `voice`. */
function recording(voice: boolean): Int16Array {
  let s = 7;
  const rnd = () => ((s = (s * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff) * 2 - 1;
  const out = new Int16Array(RATE * 1.5);
  for (let i = 0; i < out.length; i++) {
    let v = rnd() * 45;
    const t = i - RATE / 2;
    if (voice && t >= 0 && t < RATE * 0.4) {
      const ph = (2 * Math.PI * 140 * t) / RATE;
      let h = 0;
      for (let k = 1; k <= 10; k++) h += Math.sin(k * ph) / k;
      v += 1500 * h;
    }
    out[i] = Math.round(v);
  }
  return out;
}

/** A device that streams `pcm` for the adopted listen in 100 ms frames, then the held button's release (reason tap). */
function device(room: DeviceRoom, sock: FakeSocket, pcm: Int16Array) {
  const cmds: Array<{ name: string; args: any }> = [];
  const text = sock.sendText.bind(sock);
  const bin = sock.sendBinary.bind(sock);
  sock.sendText = (t: string) => {
    text(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args });
    const result = f.name === "listen" ? { job: f.args.job, ms: 30000, ack: "chirp", filler_ms: 400 } : {};
    setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result })), 1);
    if (f.name !== "listen") return;
    setTimeout(() => {
      let seq = 0;
      for (let at = 0; at < pcm.length; at += 1600) void room.socketMessage(sock, encodeAudioFrame({ s: f.args.job, seq: seq++, kind: "pcm16", rate: RATE, end: false }, new Uint8Array(pcm.slice(at, at + 1600).buffer)));
      void room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: f.args.job, seq, kind: "pcm16", rate: RATE, end: true, reason: "tap" })));
    }, 15);
  };
  sock.sendBinary = (b: Uint8Array) => {
    bin(b);
    const fr = decodeAudioFrame(b)!;
    if (fr.header.end) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } })), 5);
  };
  return cmds;
}

async function heldPress(voice: boolean, sttText = "what's on my calendar today?") {
  const ctx = makeRoom({ timings: FAST, voiced: undefined }); // the real voice check
  ctx.voice.ttsSamples = 1600;
  ctx.voice.sttText = sttText;
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: CAPS });
  const cmds = device(ctx.room, sock, recording(voice));
  const paired = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "agent" });
  if (!paired.ok) throw new Error(JSON.stringify(paired));
  const token = paired.result.agent_token as string;
  await flush();
  await ctx.room.socketMessage(sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 0, reversals: 0, ms: 0, listen: "shake-1", input: "button_accept", hold: true } }));
  const listen = () => cmds.find((c) => c.name === "listen");
  const done = async () => {
    const job = listen() && (await ctx.kv.get<any>(`job:${listen()!.args.job}`));
    return job?.status === "done";
  };
  for (let end = Date.now() + 1500; !(await done()) && Date.now() < end; ) await new Promise((r) => setTimeout(r, 5));
  await new Promise((r) => setTimeout(r, 600)); // past the 400 ms filler
  const job = await ctx.kv.get<any>(`job:${listen()!.args.job}`);
  const events = ((await ctx.room.events(token, { wait_ms: 0 })) as any).result.events as AgentEvent[];
  return { ...ctx, cmds, job, events };
}

describe("an accidental press (nothing said)", () => {
  it("a held press of silence ends as no_speech: no STT, no utterance, no filler, idle (not thinking)", async () => {
    const w = await heldPress(false);
    expect(w.job).toMatchObject({ status: "done", result: { text: "", reason: "no_speech" } });
    expect(w.voice.sttCalls).toHaveLength(0);
    expect(w.events.filter((e) => e.type === "utterance")).toHaveLength(0);
    expect(w.cmds.filter((c) => c.name === "say" || c.name === "quick")).toHaveLength(0);
    const moods = w.cmds.filter((c) => c.name === "mood").map((c) => c.args.mood);
    expect(moods).not.toContain("thinking");
    expect(moods).toContain("idle"); // the device acknowledged the release itself: the relay takes it down
  });

  it("the same press with a voice in it is a turn: STT, one utterance", async () => {
    const w = await heldPress(true);
    expect(w.voice.sttCalls).toHaveLength(1);
    expect(w.job).toMatchObject({ status: "done", result: { text: "what's on my calendar today?", reason: "tap" } });
    expect(w.events.filter((e) => e.type === "utterance")).toHaveLength(1);
  });

  it("a voiced press whose transcript has no words ([BLANK_AUDIO]) is no_speech too: no utterance, no filler", async () => {
    const w = await heldPress(true, "[BLANK_AUDIO]");
    expect(w.voice.sttCalls).toHaveLength(1);
    expect(w.job).toMatchObject({ status: "done", result: { text: "", reason: "no_speech" } });
    expect(w.events.filter((e) => e.type === "utterance")).toHaveLength(0);
    expect(w.cmds.filter((c) => c.name === "say" || c.name === "quick")).toHaveLength(0);
  });
});
