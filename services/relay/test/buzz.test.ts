// buzz (docs/AGENT-LINK.md): the vibration motor, firmware cap `haptic`. The relay checks the shape and passes the
// rest to the device (which knows the pattern names and drops a buzz while its mic is open: `listening`); it gates
// on the cap (no_motor) and on do not disturb exactly as it gates speech.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { validateCommand } from "../src/core/commands.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, LINK, connect, flush, makeRoom } from "./helpers.js";

async function setup(caps: string[], answer: (args: any) => object = () => ({ ok: true, result: {} })) {
  const ctx = makeRoom({ timings: { cmdTimeoutMs: 300, pairWindowMs: 400 } });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps });
  const sent: any[] = [];
  const orig = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t !== "cmd") return;
    if (f.name === "buzz") sent.push(f.args);
    setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ...(f.name === "buzz" ? answer(f.args) : { ok: true, result: {} }) })), 1);
  };
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "buzzer" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  await flush();
  const token = out.result.agent_token as string;
  const dnd = async (on: boolean) => {
    await ctx.room.socketMessage(sock, JSON.stringify({ t: "evt", name: "dnd", data: { on } }));
    await flush();
  };
  const buzz = (args: unknown) => ctx.room.command(token, "buzz", args);
  const backend: RelayBackend = { device: () => ctx.room } as unknown as RelayBackend;
  return { ...ctx, token, sent, dnd, buzz, backend };
}

const err = (o: any) => (o.ok ? null : o.error);

describe("buzz", () => {
  it("validates the shape: a name or [on, off, ...] or ms, strength 0..100; in_reply_to stays on the relay", () => {
    expect(validateCommand("buzz", { pattern: "double" })).toEqual({ name: "buzz", args: { pattern: "double" }, job: false });
    expect(validateCommand("buzz", { pattern: [80, 60, 80], strength: 40 }).args).toEqual({ pattern: [80, 60, 80], strength: 40 });
    expect(validateCommand("buzz", { ms: 300 }).args).toEqual({ ms: 300 });
    const reply = `${DEVICE}.j_abcdefgh12`;
    expect(validateCommand("buzz", { pattern: "tap", in_reply_to: reply })).toEqual({ name: "buzz", args: { pattern: "tap" }, job: false, in_reply_to: reply });
    for (const bad of [{}, { pattern: "tap", ms: 100 }, { pattern: "Double!" }, { pattern: [] }, { pattern: [1, 2, 3, 4, 5, 6, 7, 8, 9] }, { pattern: [80, -1] }, { pattern: [80, 2001] },
      { ms: 0 }, { ms: 2001 }, { ms: 1.5 }, { pattern: 5 }, { pattern: "tap", strength: 101 }, { pattern: "tap", loud: true }, { pattern: "tap", in_reply_to: "nope" }]) {
      expect(() => validateCommand("buzz", bad), JSON.stringify(bad)).toThrow();
    }
  });

  it("goes to the device as checked; the device's own refusals come back as they are", async () => {
    const w = await setup(["haptic", "dnd"], (a) => (a.pattern === "bogus" ? { ok: false, error: "unknown_pattern" } : a.pattern === "tap" ? { ok: false, error: "listening" } : { ok: true, result: { ms: 210, strength: 100, pattern: [60, 90, 60] } }));
    expect(await w.buzz({ pattern: "double" })).toMatchObject({ ok: true, result: { ms: 210 } });
    expect(err(await w.buzz({ pattern: "bogus" }))).toBe("unknown_pattern");
    expect(err(await w.buzz({ pattern: "tap" }))).toBe("listening");
    expect(w.sent).toEqual([{ pattern: "double" }, { pattern: "bogus" }, { pattern: "tap" }]);
  });

  it("a Molty whose caps leave out haptic: no_motor, nothing sent", async () => {
    const w = await setup(["say", "listen", "dnd"]);
    expect(err(await w.buzz({ pattern: "double" }))).toBe("no_motor");
    expect(w.sent).toEqual([]);
  });

  it("do not disturb: an unprompted buzz is refused like speech, a reply goes through", async () => {
    const w = await setup(["haptic", "dnd"]);
    await w.dnd(true);
    expect(err(await w.buzz({ pattern: "double" }))).toBe("do_not_disturb");
    expect(w.sent).toEqual([]);
    expect(err(await w.buzz({ pattern: "tick", in_reply_to: `${DEVICE}.j_abcdefgh12` }))).toBeNull();
    expect(w.sent).toEqual([{ pattern: "tick" }]);
    await w.dnd(false);
    expect(err(await w.buzz({ pattern: "double" }))).toBeNull();
  });

  it("REST /cmd {name:'buzz'} and MCP molty_buzz", async () => {
    const w = await setup(["haptic"]);
    const r = await handleRest(
      new Request(`https://relay.test/v1/devices/${DEVICE}/cmd`, { method: "POST", headers: { authorization: `Bearer ${w.token}`, "content-type": "application/json" }, body: JSON.stringify({ name: "buzz", args: { pattern: "double" } }) }),
      w.backend,
    );
    expect(r.status).toBe(200);
    const call = async (args: object) => {
      const res = await handleMcp(
        new Request("https://r/mcp", { method: "POST", headers: { "content-type": "application/json", accept: "application/json, text/event-stream", authorization: `Bearer ${w.token}` }, body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name: "molty_buzz", arguments: args } }) }),
        w.backend,
      );
      return ((await res.json()) as any).result;
    };
    expect((await call({ pattern: [80, 60, 80], strength: 50 })).isError).toBe(false);
    expect((await call({ pattern: "Loud" })).structuredContent).toMatchObject({ error: "invalid_args" });
    expect(w.sent).toEqual([{ pattern: "double" }, { pattern: [80, 60, 80], strength: 50 }]);
    const none = await setup([]);
    const r2 = await handleRest(
      new Request(`https://relay.test/v1/devices/${DEVICE}/cmd`, { method: "POST", headers: { authorization: `Bearer ${none.token}`, "content-type": "application/json" }, body: JSON.stringify({ name: "buzz", args: { ms: 100 } }) }),
      none.backend,
    );
    expect(r2.status).toBe(409);
  });
});
