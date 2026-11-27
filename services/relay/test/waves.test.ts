// The tone player (docs/AGENT-LINK.md "Waves"): wave.play / set / stop are validated here and forwarded to a device that
// lists the `rhythm` cap (the speaker's players share it); its mutes reach the agent as `tap` events.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { CommandError, MAX_WAVE_BYTES, MAX_WAVE_KINDS, validateCommand } from "../src/core/commands.js";
import { agentEventData, eventSentence } from "../src/core/events.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, LINK, connect, makeRoom } from "./helpers.js";

const MAJOR = { id: "major", label: "MAJOR", semis: [0, 4, 7], ratio: [4, 5, 6], spell: "major" };
const PLAY = { chords: [MAJOR], pick: 0, note: 0, midi: 60 };
const code = (name: string, args: unknown) => {
  try {
    validateCommand(name, args);
  } catch (e) {
    return (e as CommandError).code;
  }
  return "ok";
};

describe("wave commands: validation", () => {
  it("wave.play: 1..32 kinds (8 KB at most), pick within them, a note, midi or hz, tuning, paused, mute; forwarded as given", () => {
    expect(validateCommand("wave.play", { ...PLAY, tuning: "equal", paused: true, mute: [2] }).args).toEqual({ ...PLAY, tuning: "equal", paused: true, mute: [2] });
    expect(validateCommand("wave.play", { chords: [MAJOR], pick: 0, hz: 3 }).args).toEqual({ chords: [MAJOR], pick: 0, hz: 3 });
    for (const bad of [{}, { ...PLAY, chords: [] }, { ...PLAY, chords: [1] }, { ...PLAY, pick: 1 }, { ...PLAY, note: 12 }, { ...PLAY, midi: -37 }, { ...PLAY, hz: 3 },
      { ...PLAY, tuning: "piano" }, { ...PLAY, mute: [6] }, { ...PLAY, octave: 4 }, { ...PLAY, chords: Array.from({ length: MAX_WAVE_KINDS + 1 }, () => MAJOR) },
      { ...PLAY, chords: [{ ...MAJOR, about: "x".repeat(MAX_WAVE_BYTES) }] }])
      expect(code("wave.play", bad), JSON.stringify(bad).slice(0, 80)).toBe("invalid_args");
  });
  it("wave.set: one pitch (midi, hz, octave or count: true), a note, a pick, tuning, mute, paused; wave.stop takes nothing", () => {
    expect(validateCommand("wave.set", { count: true }).args).toEqual({ count: true });
    expect(validateCommand("wave.set", { octave: 4, pick: 3, mute: [] }).args).toEqual({ octave: 4, pick: 3, mute: [] });
    for (const bad of [{}, { count: false }, { midi: 60, hz: 440 }, { octave: 9 }, { volume: 3 }, { hz: 6000 }]) expect(code("wave.set", bad)).toBe("invalid_args");
    expect(validateCommand("wave.stop", {}).args).toEqual({});
    expect(code("wave.stop", { now: true })).toBe("invalid_args");
  });
  it("a muting tap on a voice keeps its shape and reads as the chord's", () => {
    expect(agentEventData("tap", { x: 0.5, y: 0.1, spot: "voice-2", muted: true })).toEqual({ x: 0.5, y: 0.1, spot: "voice-2", muted: true });
    expect(eventSentence({ id: "e1", type: "tap", ts: 0, data: { x: 0.5, y: 0.1, spot: "voice-2", muted: true } })).toBe("The human muted voice-2 of Molty's chord");
  });
});

async function setup(caps: string[]) {
  const ctx = makeRoom({ timings: { cmdTimeoutMs: 300, pairWindowMs: 400 } });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps });
  const cmds: Array<{ name: string; args: any }> = [];
  const orig = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args });
    setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: f.name === "wave.stop" ? {} : { chord: "major", note: "C", midi: 60, hz: 261.63 } })), 1);
  };
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "waves" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  const backend: RelayBackend = { device: () => ctx.room };
  const token = out.result.agent_token as string;
  const cmd = async (name: string, args: unknown) => {
    const r = await handleRest(new Request(`https://relay.test/v1/devices/${DEVICE}/cmd`, { method: "POST", body: JSON.stringify({ name, args }), headers: { authorization: `Bearer ${token}`, "content-type": "application/json" } }), backend);
    return { status: r.status, body: (await r.json()) as any };
  };
  return { cmds, cmd };
}

describe("wave commands: forwarding and caps", () => {
  it("a Molty with a speaker's players (the rhythm cap) gets them as validated", async () => {
    const w = await setup(["say", "pkg", "scene", "rhythm"]);
    expect((await w.cmd("wave.play", PLAY)).body).toMatchObject({ ok: true, result: { chord: "major", hz: 261.63 } });
    expect((await w.cmd("wave.set", { count: true })).body).toMatchObject({ ok: true });
    expect((await w.cmd("wave.stop", {})).body).toMatchObject({ ok: true });
    expect(w.cmds).toEqual([{ name: "wave.play", args: PLAY }, { name: "wave.set", args: { count: true } }, { name: "wave.stop", args: {} }]);
  });
  it("no speaker (no rhythm cap): a clean 409 unsupported, nothing sent", async () => {
    const w = await setup(["say", "pkg", "scene"]);
    for (const [name, args] of [["wave.play", PLAY], ["wave.set", { midi: 60 }], ["wave.stop", {}]] as const) {
      const r = await w.cmd(name, args);
      expect(r.status, name).toBe(409);
      expect(r.body).toMatchObject({ ok: false, error: "unsupported" });
    }
    expect(w.cmds).toEqual([]);
  });
});
