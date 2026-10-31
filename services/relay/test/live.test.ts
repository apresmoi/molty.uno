// A live conversation (molty_voice/live.py on the box, runLive in the room): behind the owner's switch.
import { describe, expect, it } from "vitest";
import { setup } from "./turn-harness.js";

const until = async (f: () => boolean, ms = 2000) => {
  const t0 = Date.now();
  while (!f() && Date.now() - t0 < ms) await new Promise((r) => setTimeout(r, 3));
  expect(f()).toBe(true);
};
const SP = { words: ["hello", "there"] };

function liveBox(o: { refuse?: string; silent?: boolean } = {}) {
  const frames: Array<Record<string, any>> = [];
  let say!: (m: unknown) => void;
  let audio!: (id: string, n: number) => void;
  const mic: number[] = [];
  return {
    frames,
    mic,
    send: (m: Record<string, unknown>) => say({ id: frames[0].id, ...m }),
    audio: (n: number) => audio(frames[0].id, n),
    opts: {
      live: (f: Record<string, any>, s: (m: unknown) => void, a: (id: string, n: number) => void) => {
        say = s;
        audio = a;
        frames.push(f);
        if (f.t === "live.start" && !o.silent) setTimeout(() => s(o.refuse ? { t: "live.end", id: f.id, reason: "error", error: o.refuse } : { t: "live.started", id: f.id }), 1);
        if (f.t === "live.stop") setTimeout(() => s({ t: "live.end", id: f.id, reason: "stopped", billed_s: 3 }), 1);
      },
      liveMic: (_id: string, b: number) => void mic.push(b),
    },
  };
}
const on = async (w: Awaited<ReturnType<typeof setup>>) => expect(((await w.room.voiceSettings(w.link, { action: "set", conversation: "live" } as any)) as any).ok).toBe(true);
const traces = async (w: Awaited<ReturnType<typeof setup>>) => ((await w.room.ownerActivity(w.link, { action: "get", query: { kind: "trace", limit: "100" } } as any)) as any).result.entries.filter((e: any) => e.ev === "live");

describe("conversation: live", () => {
  it("off by default: a box that offers it is never asked, the shake is the agents' turn", async () => {
    const b = liveBox();
    const w = await setup({ box: b.opts });
    const view = (await w.room.voiceSettings(w.link, { action: "get" } as any)) as any;
    expect(view.result.conversation).toEqual({ mode: "agent", available: true });
    await w.shake(SP);
    await until(() => w.box.starts.length === 1);
    expect(b.frames).toHaveLength(0);
  });

  it("the setting does nothing on a box that does not offer it", async () => {
    const w = await setup();
    await on(w);
    await w.shake(SP);
    await until(() => w.box.starts.length === 1);
    expect(((await w.room.voiceSettings(w.link, { action: "get" } as any)) as any).result.conversation).toEqual({ mode: "live", available: false });
    expect(await traces(w)).toHaveLength(0); // never tried, not even a fallback
  });

  it("refuses a bad setting; agent turns it off", async () => {
    const w = await setup();
    expect(await w.room.voiceSettings(w.link, { action: "set", conversation: "nope" } as any)).toMatchObject({ ok: false, error: "invalid_args" });
    await on(w);
    await w.room.voiceSettings(w.link, { action: "set", conversation: "agent" } as any);
    expect(((await w.room.voiceSettings(w.link, { action: "get" } as any)) as any).result.conversation.mode).toBe("agent");
  });

  it("turns: mic to the box, its reply to the speaker, the mic again; the box's end closes it all", async () => {
    const b = liveBox();
    const w = await setup({ box: b.opts });
    await on(w);
    const job = await w.shake({ words: Array(50).fill("word") });
    expect(w.box.starts).toHaveLength(0); // no STT in a live conversation
    await until(() => b.mic.length > 0);
    b.send({ t: "live.heard", text: "hello" });
    b.send({ t: "live.reply", gap_ms: 321 });
    await until(() => w.dev.log.some((l) => l.name === "listen_stop"));
    b.audio(4000);
    await until(() => w.dev.log.some((l) => l.name === "say"));
    expect(w.dev.log.find((l) => l.name === "say")!.args.job).toBe(job);
    b.send({ t: "live.said", text: "hi there" });
    b.send({ t: "live.reply_end" });
    await until(() => w.dev.log.filter((l) => l.name === "listen").length === 2);
    expect(b.frames.filter((f) => f.t === "live.listen")).toHaveLength(2);
    b.send({ t: "live.end", reason: "quiet", billed_s: 42 });
    await until(() => w.dev.log.some((l) => l.name === "mood" && l.args.mood === "idle"));
    await until(() => (w.room as any).audioJob === undefined);
    const t = await traces(w);
    expect(t.find((e: any) => e.data.turn === 1).data).toMatchObject({ gap_ms: 321, heard: "hello", said: "hi there" });
    expect(t.find((e: any) => e.data.closed).data).toMatchObject({ closed: "quiet", billed_s: 42, turns: 1 });
  });

  it("a tap ends the conversation and the session is closed", async () => {
    const b = liveBox();
    const w = await setup({ box: b.opts });
    await on(w);
    const job = await w.shake({ words: Array(50).fill("word") });
    await until(() => b.mic.length > 0);
    await w.room.socketMessage(w.sock, new TextEncoder().encode(JSON.stringify({ s: job, seq: 998, kind: "pcm16", rate: 16000, end: true, reason: "tap" }) + "\n") as any);
    await until(() => b.frames.some((f) => f.t === "live.stop"));
    await until(() => (w.room as any).audioJob === undefined);
    expect((await traces(w)).find((e: any) => e.data.closed).data.closed).toBe("tap");
  });

  it("the device going away closes the session too", async () => {
    const b = liveBox();
    const w = await setup({ box: b.opts, timings: { listenGraceMs: 5 } });
    await on(w);
    await w.shake({ words: Array(50).fill("word") });
    await until(() => b.mic.length > 0);
    await w.room.socketClose(w.sock, 1006, "gone");
    await until(() => b.frames.some((f) => f.t === "live.stop"));
  });

  it("the box's socket going ends the conversation, never a stuck turn", async () => {
    const b = liveBox();
    const w = await setup({ box: b.opts });
    await on(w);
    await w.shake({ words: Array(50).fill("word") });
    await until(() => b.mic.length > 0);
    (w.room as any).box.drop(undefined, "voice_box_offline");
    await until(() => (w.room as any).audioJob === undefined && (w.room as any).live === undefined);
    expect((await traces(w)).find((e: any) => e.data.closed).data.closed).toBe("voice_box_offline");
  });

  it("a model that fails to open: the shake is the agents' turn, and the log says why", async () => {
    const b = liveBox({ refuse: "no_key" });
    const w = await setup({ box: b.opts });
    await on(w);
    await w.shake(SP);
    await until(() => w.box.starts.length === 1);
    expect((await traces(w))[0].data).toEqual({ fallback: "no_key" });
  });

  it("a box too slow to open: the same, and the session it may still open is stopped", async () => {
    const b = liveBox({ silent: true });
    const w = await setup({ box: b.opts, timings: { liveStartMs: 10 } });
    await on(w);
    await w.shake(SP);
    await until(() => w.box.starts.length === 1);
    expect(b.frames.map((f) => f.t)).toEqual(["live.start", "live.stop"]);
    expect((await traces(w))[0].data).toEqual({ fallback: "live_start_timeout" });
  });

  it("transcripts off: the words are not logged", async () => {
    const b = liveBox();
    const w = await setup({ box: b.opts });
    await on(w);
    await w.room.ownerActivity(w.link, { action: "set", transcripts: false } as any);
    await w.shake({ words: Array(50).fill("word") });
    await until(() => b.mic.length > 0);
    b.send({ t: "live.heard", text: "secret" });
    b.send({ t: "live.reply" });
    b.audio(4000);
    b.send({ t: "live.reply_end" });
    await until(() => w.dev.log.filter((l) => l.name === "listen").length === 2);
    const data = JSON.stringify((await traces(w)).map((e: any) => e.data));
    expect(data).not.toContain("secret");
  });
});
