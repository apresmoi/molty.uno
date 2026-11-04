// creature.select / creature.list: validated here, forwarded to the device, gated by its `creatures` cap.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { CommandError, validateCommand } from "../src/core/commands.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, LINK, connect, makeRoom } from "./helpers.js";

async function setup(caps: string[]) {
  const ctx = makeRoom({ timings: { cmdTimeoutMs: 300, pairWindowMs: 400 } });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps });
  const cmds: Array<{ name: string; args: unknown }> = [];
  const orig = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args });
    const result = f.name === "creature.select" ? { id: f.args.id, name: "Fox" } : { current: "fox", creatures: [{ id: "fox", name: "Fox", version: "1.0.0", source: "builtin" }] };
    setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result })), 1);
  };
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "zoo" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  const backend: RelayBackend = { device: () => ctx.room };
  const token = out.result.agent_token as string;
  const cmd = async (name: string, args: unknown) => {
    const r = await handleRest(new Request(`https://relay.test/v1/devices/${DEVICE}/cmd`, { method: "POST", headers: { authorization: `Bearer ${token}`, "content-type": "application/json" }, body: JSON.stringify({ name, args }) }), backend);
    return { status: r.status, body: (await r.json()) as any };
  };
  return { cmds, cmd };
}

const code = (f: () => unknown) => {
  try {
    f();
  } catch (e) {
    return (e as CommandError).code;
  }
  return "ok";
};

describe("creature commands: validation", () => {
  it("creature.select takes only a pkg-style id", () => {
    expect(validateCommand("creature.select", { id: "fox-2" })).toMatchObject({ args: { id: "fox-2" }, job: false });
    for (const bad of [{}, { id: "Fox" }, { id: "" }, { id: "a".repeat(33) }, { id: 3 }, { id: "fox", extra: 1 }]) {
      expect(code(() => validateCommand("creature.select", bad)), JSON.stringify(bad)).toBe("invalid_args");
    }
  });

  it("creature.list takes nothing", () => {
    expect(validateCommand("creature.list", undefined).args).toEqual({});
    expect(validateCommand("creature.list", {}).args).toEqual({});
    expect(code(() => validateCommand("creature.list", { id: "fox" }))).toBe("invalid_args");
  });
});

describe("creature commands: forwarding", () => {
  it("a device with the cap sees exactly the validated args and its answer comes back", async () => {
    const w = await setup(["say", "creatures"]);
    expect((await w.cmd("creature.select", { id: "fox" })).body).toMatchObject({ ok: true, result: { id: "fox", name: "Fox" } });
    expect((await w.cmd("creature.list", {})).body).toMatchObject({ ok: true, result: { current: "fox" } });
    expect(w.cmds).toEqual([
      { name: "creature.select", args: { id: "fox" } },
      { name: "creature.list", args: {} },
    ]);
  });

  it("without the cap: a clean 409 unsupported and nothing is sent", async () => {
    const w = await setup(["say", "pkg", "scene"]);
    for (const [name, args] of [["creature.select", { id: "fox" }], ["creature.list", {}]] as const) {
      const r = await w.cmd(name, args);
      expect(r.status, name).toBe(409);
      expect(r.body).toMatchObject({ ok: false, error: "unsupported" });
    }
    expect(w.cmds).toEqual([]);
  });
});
