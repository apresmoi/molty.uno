// Who Molty is (core/events.ts creatureData, docs/AGENT-LINK.md "Device events"): the device reports the
// creature on its screen on each welcome and each change; the relay keeps it and hands it to agents as
// `device.creature` on every utterance, `relay.creature` in status, and a `creature` event when it changes.
import { describe, expect, it } from "vitest";
import { type AgentEvent, creatureData } from "../src/core/events.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import { DEVICE, LINK, connect, flush, makeRoom } from "./helpers.js";

const DUCK = {
  id: "duck",
  name: "Duck",
  about: {
    look: "A round yellow duckling, mostly head, with a small orange bill.",
    gestures: { nod: "bobs its head forward two or three times", wiggle: "waggles its tail end fast" },
    moods: { idle: "keeps busy with small things", happy: "flaps its wings in bursts" },
  },
  gestures: ["wiggle", "nod"],
  moods: ["idle", "happy"],
};
const CAT = { ...DUCK, id: "cat", name: "Cat", about: { ...DUCK.about, look: "A slim lavender cat sitting up straight." } };

async function setup() {
  const ctx = makeRoom({ timings: { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300 } });
  ctx.voice.sttText = "who are you?";
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: ["say", "listen", "listen_stop", "shake_talk"] });
  const orig = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t !== "cmd") return;
    const result = f.name === "listen" ? { job: f.args.job, ms: 30000 } : f.name === "status" ? { fw: "test-fw", creature: "fluid" } : {};
    setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result })), 1);
    if (f.name === "listen") {
      setTimeout(() => {
        void ctx.room.socketMessage(sock, encodeAudioFrame({ s: f.args.job, seq: 0, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(3200).fill(8000).buffer)));
        void ctx.room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: f.args.job, seq: 1, kind: "pcm16", rate: 16000, end: true, reason: "silence", speech_ms: 900 })));
      }, 15);
    }
  };
  sock.sendBinary = (b: Uint8Array) => {
    const fr = decodeAudioFrame(b)!;
    if (fr.header.end) setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } })), 5);
  };
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "zoo" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  await flush();
  const token = out.result.agent_token as string;
  const emit = async (name: string, data: unknown) => {
    await ctx.room.socketMessage(sock, JSON.stringify({ t: "evt", name, data }));
    await flush();
  };
  const inbox = async () => ((await ctx.room.events(token, { wait_ms: 0 })) as any).result.events as AgentEvent[];
  const status = async () => ((await ctx.room.command(token, "status", {})) as any).result;
  return { ...ctx, token, emit, inbox, status };
}

describe("device creature", () => {
  it("kept per device: in status, on every utterance, and an event only when it changes", async () => {
    const w = await setup();
    expect((await w.status()).relay.creature).toBeNull(); // the firmware said nothing yet
    await w.emit("creature", DUCK);
    expect((await w.inbox()).map((e) => [e.type, e.data])).toEqual([["creature", DUCK]]);
    await w.emit("creature", DUCK); // the next welcome: the same creature, no event
    expect(await w.inbox()).toEqual([]);
    expect((await w.status()).relay.creature).toEqual(DUCK);

    await w.emit("shake", { peak_g: 2.4, reversals: 5, ms: 900, listen: "shake-1" });
    let seen: AgentEvent[] = [];
    for (let i = 0; i < 300 && !seen.some((e) => e.type === "utterance"); i++) {
      seen.push(...(await w.inbox()));
      await new Promise((r) => setTimeout(r, 5));
    }
    expect(seen.find((e) => e.type === "utterance")!.data.device).toMatchObject({ mood: "thinking", creature: DUCK });

    await w.emit("creature", CAT);
    expect((await w.inbox()).map((e) => [e.type, e.data])).toEqual([["creature", CAT]]);
    expect((await w.status()).relay.creature).toEqual(CAT);
  });

  it("a malformed frame is dropped and the last good creature stays", async () => {
    const w = await setup();
    await w.emit("creature", DUCK);
    await w.inbox();
    for (const bad of [{ ...DUCK, about: { look: 3 } }, { ...DUCK, gestures: "nod" }, { ...DUCK, id: "Duck!" }, { ...DUCK, about: { ...DUCK.about, moods: { idle: "x".repeat(101) } } }]) await w.emit("creature", bad);
    expect(await w.inbox()).toEqual([]);
    expect((await w.status()).relay.creature).toEqual(DUCK);
  });
});

describe("creatureData", () => {
  it("takes what the firmware sends and nothing else", () => {
    expect(creatureData(DUCK)).toEqual(DUCK);
    expect(creatureData({ id: "", name: "Blob" })).toEqual({ id: "", name: "Blob" }); // a .molty: its name only
    expect(creatureData({ ...DUCK, extra: 1, about: { ...DUCK.about, extra: 2 } })).toEqual(DUCK);
    const many = Object.fromEntries(Array.from({ length: 33 }, (_, i) => [`g${i}`, "does a thing"]));
    for (const bad of [null, {}, { id: "duck" }, { ...DUCK, name: "" }, { ...DUCK, about: { ...DUCK.about, look: "x".repeat(241) } }, { ...DUCK, about: { ...DUCK.about, gestures: many } }, { ...DUCK, moods: ["idle", "very happy"] }]) {
      expect(creatureData(bad), JSON.stringify(bad)).toBeUndefined();
    }
  });
});
