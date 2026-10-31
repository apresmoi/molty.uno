// Voice off (text only): what agents say goes on the screen, and nothing listens.
import { describe, expect, it } from "vitest";
import { CAPS, SHAKE, setup, until } from "./turn-harness.js";

async function off() {
  const w = await setup({ caps: [...CAPS, "working"] });
  expect(((await w.room.voiceSettings(w.link, { action: "set", mode: "off" })) as any).ok).toBe(true);
  return w;
}
const named = (w: any, n: string) => w.dev.log.filter((l: any) => l.name === n);

describe("voice off", () => {
  it("a talk shows its text, does not speak or listen, and says so", async () => {
    const w = await off();
    const r = (await w.room.talk(w.a, { say: "Hi there." })) as any;
    expect(r).toMatchObject({ ok: true, result: { status: "done", reason: "voice_off", said: "Hi there." } });
    expect(named(w, "show").map((l: any) => l.args.text)).toEqual(["Hi there."]);
    expect(named(w, "say")).toHaveLength(0);
    expect(named(w, "listen")).toHaveLength(0);
  });

  it("a [working] cue still starts working, and listen and quick don't run", async () => {
    const w = await off();
    await w.room.talk(w.a, { say: "Searching. [working]" });
    await until(() => named(w, "mood").some((l: any) => l.args.mood === "working"), 500);
    expect(named(w, "mood").map((l: any) => l.args.mood)).toContain("working");
    expect(await w.room.command(w.a, "listen", {})).toMatchObject({ ok: false, error: "voice_off" });
    expect(await w.room.command(w.a, "quick", { id: "hmm" })).toMatchObject({ ok: true, result: { skipped: "voice_off" } });
    expect(named(w, "say")).toHaveLength(0);
  });

  it("a shake is declined, so nothing records, fills or reacts", async () => {
    const w = await off();
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { ...SHAKE, listen: "shake-1" } }));
    await until(() => named(w, "listen_stop").length > 0, 500);
    expect(named(w, "listen_stop")[0].args).toMatchObject({ job: "shake-1", reason: "no_speech" });
    expect(named(w, "listen")).toHaveLength(0);
  });

  it("status tells the agent what setup chose", async () => {
    const w = await off();
    const r = (await w.room.command(w.a, "status", {})) as any;
    expect(r.result.relay.voice).toBe("voice_off");
    expect(r.result.relay.setup).toEqual({ voice: "off", first_words: "off", reflex: "off" });
  });

  it("voice back on: a talk speaks and listens again", async () => {
    const w = await off();
    await w.room.voiceSettings(w.link, { action: "set", mode: "auto" });
    await w.room.talk(w.a, { say: "Hi there." });
    expect(named(w, "say")).toHaveLength(1);
    expect(named(w, "listen")).toHaveLength(1);
  });
});
