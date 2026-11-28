// The sky atlas (docs/AGENT-LINK.md "Atlas"): atlas.load / atlas.show are validated here for their shape and forwarded
// to a device that lists the `atlas` cap (the device checks each object strictly); its `ring` events name the object.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { CommandError, MAX_ATLAS_BYTES, MAX_ATLAS_OBJECTS, validateCommand } from "../src/core/commands.js";
import { agentEventData, eventSentence } from "../src/core/events.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, LINK, connect, makeRoom } from "./helpers.js";

const ORION = { id: "con-ori", name: "Orion", facts: "2 HANDS WIDE", see: "eye", d: { k: "stars", p: "0AZv", l: "" } };
const LOAD = { kinds: [{ id: "constellation", name: "CONSTELLATIONS", tint: "#9fc0ff", count: 88 }], clock: 1320, kind: "constellation", objects: [ORION] };
const code = (name: string, args: unknown) => {
  try {
    validateCommand(name, args);
  } catch (e) {
    return (e as CommandError).code;
  }
  return "ok";
};

describe("atlas commands: validation", () => {
  it("atlas.load: kinds, a kind's objects (1..96, 32 KB at most), the place's clock; forwarded as given", () => {
    expect(validateCommand("atlas.load", LOAD).args).toEqual(LOAD);
    expect(validateCommand("atlas.load", { kind: "galaxy", objects: [ORION] }).args).toEqual({ kind: "galaxy", objects: [ORION] });
    for (const bad of [{}, { objects: [ORION] }, { kind: "Galaxy!", objects: [ORION] }, { kind: "galaxy", objects: [] }, { kind: "galaxy", objects: [1] },
      { kinds: [] }, { ...LOAD, clock: 1440 }, { ...LOAD, place: "Berlin" }, { kind: "galaxy", objects: Array.from({ length: MAX_ATLAS_OBJECTS + 1 }, () => ORION) },
      { kind: "galaxy", objects: [{ ...ORION, facts: "x".repeat(MAX_ATLAS_BYTES) }] }])
      expect(code("atlas.load", bad), JSON.stringify(bad).slice(0, 80)).toBe("invalid_args");
  });
  it("atlas.show: a kind, an index, the level, the face", () => {
    expect(validateCommand("atlas.show", { kind: "planet", index: 2, level: "objects", face: "where" }).args).toEqual({ kind: "planet", index: 2, level: "objects", face: "where" });
    for (const bad of [{ index: 96 }, { level: "cards" }, { face: "back" }, { kind: 3 }, { place: "Berlin" }]) expect(code("atlas.show", bad)).toBe("invalid_args");
  });
  it("a ring event names the object browsed (an index past a wheel's 32)", () => {
    const d = { spot: "object", index: 87, label: "ORION", id: "con-ori" };
    expect(agentEventData("ring", d)).toEqual(d);
    expect(eventSentence({ id: "e1", type: "ring", ts: 0, data: d })).toBe("The human is browsing the sky atlas on Molty: ORION");
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
    setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: {} })), 1);
  };
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "atlas" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  const backend: RelayBackend = { device: () => ctx.room };
  const token = out.result.agent_token as string;
  const cmd = async (name: string, args: unknown) => {
    const r = await handleRest(new Request(`https://relay.test/v1/devices/${DEVICE}/cmd`, { method: "POST", body: JSON.stringify({ name, args }), headers: { authorization: `Bearer ${token}`, "content-type": "application/json" } }), backend);
    return { status: r.status, body: (await r.json()) as any };
  };
  return { cmds, cmd };
}

describe("atlas commands: forwarding and caps", () => {
  it("a Molty with the atlas cap gets them as validated", async () => {
    const a = await setup(["say", "pkg", "scene", "atlas"]);
    expect((await a.cmd("atlas.load", LOAD)).body).toMatchObject({ ok: true });
    expect((await a.cmd("atlas.show", { kind: "constellation", index: 0, level: "objects" })).body).toMatchObject({ ok: true });
    expect(a.cmds).toEqual([{ name: "atlas.load", args: LOAD }, { name: "atlas.show", args: { kind: "constellation", index: 0, level: "objects" } }]);
  });
  it("firmware without the cap: a clean 409 unsupported, nothing sent", async () => {
    const a = await setup(["say", "pkg", "scene"]);
    for (const [name, args] of [["atlas.load", LOAD], ["atlas.show", { index: 0 }]] as const) {
      const r = await a.cmd(name, args);
      expect(r.status, name).toBe(409);
      expect(r.body).toMatchObject({ ok: false, error: "unsupported" });
    }
    expect(a.cmds).toEqual([]);
  });
});
