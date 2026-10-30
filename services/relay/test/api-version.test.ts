// The relay is just a relay: it states the agent API contract it speaks
// (`api_version`) where agents already look, and nothing about the skill. The
// skill ships as the molty plugin (skills/molty/); the old skill-version
// machinery (skill_update notices, skill.updated events) is gone, and tokens or
// rooms left over from it change nothing.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import type { DeviceRoom } from "../src/core/room.js";
import { MemoryKV } from "../src/core/store.js";
import { API_VERSION, healthResponse } from "../src/core/version.js";
import { SERVER_INFO, SKILL_HOME, TOOLS, handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, FakeHost, type FakeSocket, LINK, connect, flush, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 2000, pairWindowMs: 2000 };

/** Device that answers commands and accepts every pairing. */
function device(room: DeviceRoom, sock: FakeSocket) {
  const orig = sock.sendText.bind(sock);
  let n = 0;
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "cmd") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: { echo: f.name } })), 1);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: `NEXT${"ABCDEFGH"[n++ % 8]}ZZZ` })), 2);
  };
}

async function world(kv = new MemoryKV(), host = new FakeHost()) {
  const ctx = makeRoom({ kv, host, timings: FAST, scheduler: { setAlarm: async () => {} }, resolveHost: async () => ["93.184.216.34"] });
  const sock = await connect(ctx.room, ctx.host);
  device(ctx.room, sock);
  let code = LINK;
  const pairAgent = async (name: string) => {
    const out = await ctx.room.pair({ device_id: DEVICE, link_code: code, agent_name: name });
    if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
    await flush();
    code = (await ctx.kv.get<{ code: string }>("link"))?.code ?? code;
    return out.result;
  };
  /** Make every paired token look like one minted while the relay tracked skill versions. */
  const legacy = async (seen: number | undefined) => {
    for (const [k, v] of await ctx.kv.list<Record<string, unknown>>("tok:")) {
      if (seen === undefined) delete v.skill_seen;
      else v.skill_seen = seen;
      await ctx.kv.put(k, v);
    }
  };
  const backend: RelayBackend = { device: () => ctx.room };
  return { ...ctx, sock, pairAgent, legacy, backend };
}

const rest = async (b: RelayBackend, path: string, token: string, init: RequestInit = {}) =>
  (await handleRest(new Request(`https://relay.test${path}`, { ...init, headers: { authorization: `Bearer ${token}`, "content-type": "application/json" } }), b).then((r) => r.json())) as any;

let rid = 0;
const rpc = async (b: RelayBackend, method: string, params: unknown, token?: string) =>
  ((await (
    await handleMcp(
      new Request("https://relay.test/mcp", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json, text/event-stream", ...(token ? { authorization: `Bearer ${token}` } : {}) },
        body: JSON.stringify({ jsonrpc: "2.0", id: ++rid, method, params }),
      }),
      b,
    )
  ).json()) as any).result;

describe("api_version: where agents already look", () => {
  it("is a positive integer", () => {
    expect(Number.isInteger(API_VERSION) && API_VERSION >= 1).toBe(true);
  });

  it("pair result, status relay block, GET /v1/health, molty_updates, MCP serverInfo + instructions", async () => {
    const w = await world();
    const p = await w.pairAgent("a");
    expect(p).toMatchObject({ status: "approved", api_version: API_VERSION });
    const status = await w.room.command(p.agent_token, "status", {});
    expect(status.ok && (status.result as any).relay).toMatchObject({ api_version: API_VERSION, caps: expect.any(Array) });
    expect(await healthResponse().json()).toMatchObject({ api_version: API_VERSION });
    const upd = await w.room.updates(p.agent_token);
    expect(upd.ok && (upd.result as any).api_version).toBe(API_VERSION);
    const init = await rpc(w.backend, "initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "t", version: "1" } });
    expect(init.serverInfo).toEqual(SERVER_INFO);
    expect(SERVER_INFO.title).toBe(`Molty relay (api v${API_VERSION})`);
    expect(init.instructions).toContain(`api_version ${API_VERSION}`);
    expect(init.instructions).toContain(SKILL_HOME);
  });

  it("the REST pair reply carries it too", async () => {
    const w = await world();
    const r = await handleRest(
      new Request("https://relay.test/v1/pair", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ device_id: DEVICE, link_code: LINK, agent_name: "rest" }) }),
      w.backend,
    );
    expect(await r.json()).toMatchObject({ ok: true, agent_token: expect.any(String), api_version: API_VERSION });
  });
});

describe("no skill machinery left in the relay", () => {
  it("MCP instructions and tool descriptions point at the skill on GitHub, never at /skill.md", async () => {
    expect(SKILL_HOME).toBe("https://github.com/apresmoi/molty.uno/tree/main/skills/molty");
    const w = await world();
    const init = await rpc(w.backend, "initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "t", version: "1" } });
    for (const text of [init.instructions as string, ...TOOLS.map((t) => t.description)]) {
      expect(text).not.toMatch(/skill\.md|skill_update|re-read (the|this|your) skill/i);
    }
  });

  it("a token from the skill-version era (behind, or never recorded) gets no notice on REST or MCP, success or failure", async () => {
    for (const seen of [undefined, 3]) {
      const w = await world();
      const { agent_token: a } = await w.pairAgent("old");
      await w.legacy(seen);
      const ok = await rest(w.backend, `/v1/devices/${DEVICE}/cmd`, a, { method: "POST", body: JSON.stringify({ name: "imu" }) });
      expect(ok).toEqual({ ok: true, result: { echo: "imu" } });
      const bad = await rest(w.backend, `/v1/devices/${DEVICE}/cmd`, a, { method: "POST", body: JSON.stringify({ name: "nope" }) });
      expect(bad).not.toHaveProperty("skill_update");
      expect(await rest(w.backend, `/v1/devices/${DEVICE}/events`, a)).toEqual({ ok: true, events: [], next: "e0", more: false });
      const r = await rpc(w.backend, "tools/call", { name: "molty_imu", arguments: {} }, a);
      expect(r.structuredContent).toEqual({ ok: true, echo: "imu" });
      expect(r.content).toHaveLength(1);
      const out = await w.room.command(a, "imu", {});
      expect(out).not.toHaveProperty("notice");
    }
  });

  it("a device reconnect (a deploy) appends no event, even with older agents paired and a stale skillev key", async () => {
    const kv = new MemoryKV();
    const host = new FakeHost();
    const w = await world(kv, host);
    const { agent_token: old } = await w.pairAgent("old");
    await w.room.webhook(old, { action: "set", spec: { url: "https://hooks.example.org/old" } });
    await w.legacy(undefined);
    await kv.put("skillev", 5);
    const after = makeRoom({ kv, host, timings: FAST, scheduler: { setAlarm: async () => {} } });
    await connect(after.room, host, undefined, null);
    await after.room.command(old, "imu", {}).catch(() => undefined);
    expect([...(await kv.list("ev:")).keys()]).toEqual([]);
    expect([...(await kv.list("whq:")).keys()]).toEqual([]);
    expect(await kv.get("origin")).toBeUndefined();
  });
});
