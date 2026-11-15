// Do not disturb (core/events.ts DND_EVENT, docs/AGENT-LINK.md "Device events"): the device reports the
// human's switch on each welcome and each change; while it is on, agents may speak, listen or ask only
// in reply to an utterance. Moods and gestures stay allowed. A device without `dnd` in caps is never gated.
import { describe, expect, it } from "vitest";
import type { AgentEvent } from "../src/core/events.js";
import { DEVICE, LINK, connect, flush, makeRoom } from "./helpers.js";

async function setup(caps: string[]) {
  const ctx = makeRoom({ timings: { cmdTimeoutMs: 300, pairWindowMs: 400 } });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps });
  const orig = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t === "cmd") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: {} })), 1);
  };
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "quiet" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  await flush();
  const token = out.result.agent_token as string;
  const emit = async (on: boolean) => {
    await ctx.room.socketMessage(sock, JSON.stringify({ t: "evt", name: "dnd", data: { on } }));
    await flush();
  };
  const inbox = async () => ((await ctx.room.events(token, { wait_ms: 0 })) as any).result.events as AgentEvent[];
  const status = async () => ((await ctx.room.command(token, "status", {})) as any).result.relay;
  const err = (o: any) => (o.ok ? null : o.error);
  return { ...ctx, token, emit, inbox, status, err };
}

describe("do not disturb", () => {
  it("kept per device: in status, an event only when it changes, and unprompted speech is refused", async () => {
    const w = await setup(["say", "listen", "ask", "mood", "dnd"]);
    expect((await w.status()).dnd).toBe(false);
    await w.emit(true);
    expect((await w.inbox()).map((e) => [e.type, e.data])).toEqual([["dnd", { on: true }]]);
    await w.emit(true); // the next welcome: the same switch, no event
    expect(await w.inbox()).toEqual([]);
    expect((await w.status()).dnd).toBe(true);

    expect(w.err(await w.room.command(w.token, "say", { text: "hello" }))).toBe("do_not_disturb");
    expect(w.err(await w.room.command(w.token, "listen", {}))).toBe("do_not_disturb");
    expect(w.err(await w.room.talk(w.token, { say: "hello" }))).toBe("do_not_disturb");
    expect(w.err(await w.room.ask(w.token, { say: "ok?", options: ["yes", "no"] }))).toBe("do_not_disturb");
    // a reply goes through the gate (it may fail later for its own reasons: this turn does not exist)
    expect(w.err(await w.room.command(w.token, "say", { text: "hi", in_reply_to: `${DEVICE}.j_abcdefgh12` }))).not.toBe("do_not_disturb");
    // silent things stay allowed
    expect(w.err(await w.room.command(w.token, "mood", { mood: "happy" }))).toBeNull();

    await w.emit(false);
    expect((await w.inbox()).map((e) => [e.type, e.data])).toEqual([["dnd", { on: false }]]);
    expect(w.err(await w.room.command(w.token, "say", { text: "hello" }))).not.toBe("do_not_disturb");
  });

  it("a device that does not report it (no `dnd` in caps) is never gated", async () => {
    const w = await setup(["say", "listen"]);
    await w.emit(true);
    expect((await w.status()).dnd).toBeNull();
    expect(w.err(await w.room.command(w.token, "say", { text: "hello" }))).not.toBe("do_not_disturb");
  });
});
