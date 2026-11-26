// The beat player (docs/AGENT-LINK.md "Rhythm"): rhythm.play / set / stop are validated here and forwarded to a device
// that lists the `rhythm` cap; the device's own `ring`, `rhythm` and muting `tap` events reach the agent, by pull and
// ride-along, and a webhook only when it names them.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { CommandError, MAX_RHYTHM_BYTES, MAX_RHYTHMS, validateCommand } from "../src/core/commands.js";
import { DEFAULT_WEBHOOK_EVENTS, SAMPLE_EVENTS, agentEventData, eventSentence, webhookWants } from "../src/core/events.js";
import type { DeviceRoom } from "../src/core/room.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeSocket, LINK, connect, makeRoom } from "./helpers.js";

const CHACARERA = { id: "chacarera", label: "CHACARERA", steps: 6, per: 3, beats: [1, 4], count_in: 1, layers: [{ id: "aro", sound: "rim", color: "#9fc0ff", hits: [1, 4] }, { id: "parche", sound: "head", color: "#ff9a7a", hits: [3, 5], accent: [5] }] };
const PLAY = { rhythms: [CHACARERA], pick: 0, bpm: 104 };
const code = (name: string, args: unknown) => {
  try {
    validateCommand(name, args);
  } catch (e) {
    return (e as CommandError).code;
  }
  return "ok";
};

describe("rhythm commands: validation", () => {
  it("rhythm.play: 1..96 patterns (32 KB at most) in up to 16 regions, pick within them, bpm 40..220, mute, buzz, paused, sync; forwarded as given", () => {
    const REG = [{ label: "ARGENTINA", color: "#8cc4ff" }];
    expect(validateCommand("rhythm.play", { ...PLAY, mute: ["aro"], buzz: true, paused: false, regions: REG, sync: -20 }).args).toEqual({ ...PLAY, mute: ["aro"], buzz: true, paused: false, regions: REG, sync: -20 });
    expect(validateCommand("rhythm.play", { ...PLAY, rhythms: Array.from({ length: 70 }, () => CHACARERA) }).args.rhythms).toHaveLength(70);   // (the skill's whole library)
    for (const bad of [{}, { ...PLAY, rhythms: [] }, { ...PLAY, rhythms: [1] }, { ...PLAY, pick: 1 }, { ...PLAY, bpm: 39 }, { ...PLAY, bpm: 221 }, { ...PLAY, mute: "aro" }, { ...PLAY, buzz: 1 }, { ...PLAY, tempo: 3 },
      { ...PLAY, rhythms: Array.from({ length: MAX_RHYTHMS + 1 }, () => CHACARERA) }, { ...PLAY, rhythms: [{ ...CHACARERA, source: "x".repeat(MAX_RHYTHM_BYTES) }] },
      { ...PLAY, regions: [] }, { ...PLAY, regions: Array.from({ length: 17 }, () => REG[0]) }, { ...PLAY, sync: 251 }, { ...PLAY, sync: 2.5 }])
      expect(code("rhythm.play", bad), JSON.stringify(bad).slice(0, 80)).toBe("invalid_args");
  });
  it("rhythm.set: at least one of bpm, pick, mute, next (true), buzz, paused; rhythm.stop takes nothing", () => {
    expect(validateCommand("rhythm.set", { bpm: 90, next: true, mute: [] }).args).toEqual({ bpm: 90, next: true, mute: [] });
    expect(validateCommand("rhythm.set", { sync: 35 }).args).toEqual({ sync: 35 });
    for (const bad of [{}, { next: false }, { bpm: "fast" }, { pick: -1 }, { volume: 3 }, { sync: -300 }]) expect(code("rhythm.set", bad)).toBe("invalid_args");
    expect(validateCommand("rhythm.stop", {}).args).toEqual({});
    expect(code("rhythm.stop", { now: true })).toBe("invalid_args");
  });
});

function device(room: DeviceRoom, sock: FakeSocket) {
  const cmds: Array<{ name: string; args: any }> = [];
  const orig = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args });
    setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: f.name === "rhythm.stop" ? {} : { rhythm: "chacarera", bpm: 104, muted: [], paused: false, buzz: false } })), 1);
  };
  return cmds;
}
async function setup(caps: string[]) {
  const ctx = makeRoom({ timings: { cmdTimeoutMs: 300, pairWindowMs: 400 } });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps });
  const cmds = device(ctx.room, sock);
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "rhythm" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  const backend: RelayBackend = { device: () => ctx.room };
  const token = out.result.agent_token as string;
  const rest = async (path: string, init: RequestInit = {}) => {
    const r = await handleRest(new Request(`https://relay.test${path}`, { ...init, headers: { authorization: `Bearer ${token}`, "content-type": "application/json" } }), backend);
    return { status: r.status, body: (await r.json()) as any };
  };
  const cmd = (name: string, args: unknown) => rest(`/v1/devices/${DEVICE}/cmd`, { method: "POST", body: JSON.stringify({ name, args }) });
  return { ...ctx, sock, cmds, cmd, rest };
}

describe("rhythm commands: forwarding and caps", () => {
  it("a Molty with the rhythm cap gets them as validated", async () => {
    const w = await setup(["say", "pkg", "scene", "rhythm"]);
    expect((await w.cmd("rhythm.play", PLAY)).body).toMatchObject({ ok: true, result: { rhythm: "chacarera", bpm: 104 } });
    expect((await w.cmd("rhythm.set", { bpm: 90 })).body).toMatchObject({ ok: true });
    expect((await w.cmd("rhythm.stop", {})).body).toMatchObject({ ok: true });
    expect(w.cmds).toEqual([{ name: "rhythm.play", args: PLAY }, { name: "rhythm.set", args: { bpm: 90 } }, { name: "rhythm.stop", args: {} }]);
  });
  it("firmware without the cap (or no speaker): a clean 409 unsupported, nothing sent", async () => {
    const w = await setup(["say", "pkg", "scene"]);
    for (const [name, args] of [["rhythm.play", PLAY], ["rhythm.set", { bpm: 90 }], ["rhythm.stop", {}]] as const) {
      const r = await w.cmd(name, args);
      expect(r.status, name).toBe(409);
      expect(r.body).toMatchObject({ ok: false, error: "unsupported" });
    }
    expect(w.cmds).toEqual([]);
  });
});

describe("rhythm events", () => {
  it("ring, rhythm and a muting tap keep their shape; anything else of theirs is dropped", () => {
    expect(agentEventData("ring", { spot: "tempo", value: 112 })).toEqual({ spot: "tempo", value: 112 });
    expect(agentEventData("ring", { spot: "rhythm", index: 6, label: "SON CLAVE", id: "son-clave", extra: 1 })).toEqual({ spot: "rhythm", index: 6, label: "SON CLAVE", id: "son-clave" });
    expect(agentEventData("ring", { spot: "tempo" })).toBeUndefined();
    expect(agentEventData("ring", { spot: "bad spot!", value: 1 })).toBeUndefined();
    expect(agentEventData("rhythm", { state: "paused", rhythm: "chacarera", bpm: 104 })).toEqual({ state: "paused", rhythm: "chacarera", bpm: 104 });
    expect(agentEventData("rhythm", { state: "louder", bpm: 104 })).toBeUndefined();
    expect(agentEventData("tap", { x: 0.4, y: 0.6, spot: "parche", muted: true })).toEqual({ x: 0.4, y: 0.6, spot: "parche", muted: true });
    for (const t of ["ring", "rhythm"]) expect(agentEventData(t, SAMPLE_EVENTS[t].data)).toEqual(SAMPLE_EVENTS[t].data);
    expect(eventSentence(SAMPLE_EVENTS.ring)).toBe("The human turned Molty's tempo wheel to 112");
    expect(eventSentence({ id: "e1", type: "ring", ts: 0, data: { spot: "rhythm", index: 6, label: "SON CLAVE", id: "son-clave" } })).toBe("The human chose son-clave on Molty's wheel: it waits, stopped, for a tap in the middle");
    expect(eventSentence(SAMPLE_EVENTS.rhythm)).toBe("The human stopped the rhythm on Molty: it waits on the clock, choosing");
    expect(eventSentence({ id: "e1", type: "rhythm", ts: 0, data: { state: "playing", rhythm: "milonga", bpm: 90 } })).toBe("The human started milonga on Molty");
    expect(eventSentence({ id: "e1", type: "rhythm", ts: 0, data: { state: "stopped", rhythm: "milonga", bpm: 90 } })).toBe("The human stopped the rhythm on Molty and left it");
    expect(eventSentence({ id: "e1", type: "ring", ts: 0, data: { spot: "region", index: 2, label: "CARIBBEAN" } })).toBe("The human is browsing CARIBBEAN on Molty's wheel");
    // the human closed a screen by hand (the reject button, a finger held on the glass): any skill's
    expect(agentEventData("scene", { why: "close" })).toEqual({ why: "close" });
    expect(agentEventData("scene", { why: "close", pkg: "astrology", layout: "chart" })).toEqual({ why: "close", pkg: "astrology", layout: "chart" });
    expect(eventSentence({ id: "e1", type: "scene", ts: 0, data: { why: "close" } })).toBe("The human closed Molty's screen: the creature is back, your screen is gone");
    expect(eventSentence({ id: "e1", type: "tap", ts: 0, data: { x: 0.5, y: 0.5, spot: "aro", muted: true } })).toBe("The human muted aro on Molty's rhythm");
  });
  it("a webhook gets them only when it names them: the human acts on Molty itself", () => {
    for (const t of ["ring", "rhythm"]) {
      expect(DEFAULT_WEBHOOK_EVENTS).not.toContain(t);
      expect(webhookWants(undefined, t)).toBe(false);
      expect(webhookWants([t], t)).toBe(true);
    }
  });
  it("the device's events reach the agent's inbox", async () => {
    const w = await setup(["say", "pkg", "scene", "rhythm"]);
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "ring", data: { spot: "tempo", value: 112 } }));
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "rhythm", data: { state: "stopped", rhythm: "chacarera", bpm: 112 } }));
    const got = await w.rest(`/v1/devices/${DEVICE}/events`);
    expect(got.body.events.map((e: any) => [e.type, e.data])).toEqual([["ring", { spot: "tempo", value: 112 }], ["rhythm", { state: "stopped", rhythm: "chacarera", bpm: 112 }]]);
  });
});
