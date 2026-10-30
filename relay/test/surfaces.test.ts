import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { handleMcp, TOOLS } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, LINK, connect, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 150, pairWindowMs: 500, sayChunkSamples: 1600, sayLeadFrames: 10, listenGraceMs: 100 };

async function setup() {
  const ctx = makeRoom({ timings: FAST });
  const sock = await connect(ctx.room, ctx.host);
  const orig = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "cmd") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: { echo: f.name, args: f.args } })), 1);
    if (f.t === "pair.request") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEXT2345" })), 5);
  };
  ctx.voice.ttsSamples = 1600;
  const backend: RelayBackend = { device: (id) => (id === DEVICE ? ctx.room : (null as never)) };
  return { ...ctx, sock, backend };
}

let rid = 0;
async function rpc(backend: RelayBackend, method: string, params?: unknown, token?: string) {
  const res = await handleMcp(
    new Request("https://relay.test/mcp", {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json, text/event-stream", ...(token ? { authorization: `Bearer ${token}` } : {}) },
      body: JSON.stringify({ jsonrpc: "2.0", id: ++rid, method, params }),
    }),
    backend,
  );
  expect(res.headers.get("content-type")).toBe("application/json");
  return (await res.json()) as any;
}
const call = async (b: RelayBackend, name: string, args: unknown, token?: string) => (await rpc(b, "tools/call", { name, arguments: args }, token)).result;

describe("MCP surface", () => {
  it("initializes with version negotiation and lists the 10 spec tools", async () => {
    const { backend } = await setup();
    const init = await rpc(backend, "initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "t", version: "1" } });
    expect(init.result).toMatchObject({ protocolVersion: "2025-06-18", capabilities: { tools: {} }, serverInfo: { name: "molty-relay" } });
    expect((await rpc(backend, "initialize", { protocolVersion: "1999-01-01" })).result.protocolVersion).toBe("2025-11-25");
    const list = (await rpc(backend, "tools/list")).result.tools;
    expect(list.map((t: any) => t.name).sort()).toEqual(
      ["molty_pair", "molty_status", "molty_mood", "molty_show", "molty_say", "molty_listen", "molty_wait", "molty_state", "molty_volume", "molty_imu", "molty_diag", "molty_log", "molty_events", "molty_webhook"].sort(),
    );
    for (const t of list) expect(t.inputSchema).toMatchObject({ type: "object", additionalProperties: false });
  });

  it("notifications get 202, GET gets 405, bad JSON gets -32700, unknown method -32601", async () => {
    const { backend } = await setup();
    const n = await handleMcp(new Request("https://r/mcp", { method: "POST", body: JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) }), backend);
    expect(n.status).toBe(202);
    expect((await handleMcp(new Request("https://r/mcp"), backend)).status).toBe(405);
    const bad = await handleMcp(new Request("https://r/mcp", { method: "POST", body: "{" }), backend);
    expect(((await bad.json()) as any).error.code).toBe(-32700);
    expect((await rpc(backend, "resources/list")).error.code).toBe(-32601);
    const v = await handleMcp(new Request("https://r/mcp", { method: "POST", headers: { "mcp-protocol-version": "2020-01-01" }, body: "{}" }), backend);
    expect(v.status).toBe(400);
  });

  it("batches (2025-03-26 clients) return an array", async () => {
    const { backend } = await setup();
    const res = await handleMcp(
      new Request("https://r/mcp", { method: "POST", body: JSON.stringify([{ jsonrpc: "2.0", id: 1, method: "ping" }, { jsonrpc: "2.0", method: "notifications/x" }]) }),
      backend,
    );
    expect(await res.json()).toEqual([{ jsonrpc: "2.0", id: 1, result: {} }]);
  });

  it("molty_pair works without a token; other tools need it", async () => {
    const { backend } = await setup();
    const denied = await call(backend, "molty_status", {});
    expect(denied).toMatchObject({ isError: true, structuredContent: { ok: false, error: "unauthorized" } });
    const pair = await call(backend, "molty_pair", { device_id: DEVICE, link_code: LINK, agent_name: "claude" });
    expect(pair.isError).toBe(false);
    expect(pair.structuredContent).toMatchObject({ ok: true, status: "approved", device_id: DEVICE });
    expect(JSON.parse(pair.content[0].text)).toEqual(pair.structuredContent);
    const token = pair.structuredContent.agent_token;
    const status = await call(backend, "molty_status", {}, token);
    expect(status.structuredContent).toMatchObject({ ok: true, echo: "status", relay: { online: true } });
  });

  it("validates arguments like the Go server", async () => {
    const { backend } = await setup();
    expect((await call(backend, "molty_pair", { device_id: DEVICE, link_code: LINK, agent_name: "x", bogus: 1 })).structuredContent.message).toBe("unknown argument bogus");
    expect((await call(backend, "molty_pair", { link_code: LINK })).structuredContent.message).toBe("missing required argument device_id");
    expect((await call(backend, "molty_pair", { device_id: DEVICE })).structuredContent.error).toBe("invalid_args");
    expect((await call(backend, "molty_pair", { device_id: "evil/../x", pair_id: "p" })).structuredContent.error).toBe("invalid_args");
    expect((await rpc(backend, "tools/call", { name: "molty_reboot", arguments: {} })).error.code).toBe(-32602);
  });

  it("every authed tool reaches the device with the right command and args; say/listen via molty_wait", async () => {
    const { backend, sock } = await setup();
    const token = (await call(backend, "molty_pair", { device_id: DEVICE, link_code: LINK, agent_name: "x" })).structuredContent.agent_token;
    expect((await call(backend, "molty_mood", { mood: "happy" }, token)).structuredContent).toMatchObject({ echo: "mood", args: { mood: "happy" } });
    expect((await call(backend, "molty_mood", { mood: "happy", hold_s: 1.5 }, token)).structuredContent).toMatchObject({ echo: "mood", args: { mood: "happy", hold_s: 1.5 } });
    expect((await call(backend, "molty_mood", { mood: "happy", hold_s: "1" }, token)).structuredContent.message).toBe("hold_s must be a number");
    expect((await call(backend, "molty_mood", { mood: "happy", hold_s: 3601 }, token)).structuredContent.error).toBe("invalid_args");
    expect((await call(backend, "molty_show", { text: "hi" }, token)).structuredContent).toMatchObject({ echo: "show", args: { text: "hi", wpm: 300 } });
    expect((await call(backend, "molty_state", {}, token)).structuredContent).toMatchObject({ echo: "state", args: {} });
    expect((await call(backend, "molty_state", { state: { blink: 1 } }, token)).structuredContent).toMatchObject({ echo: "state", args: { blink: 1 } });
    expect((await call(backend, "molty_volume", { volume: 3 }, token)).structuredContent).toMatchObject({ echo: "volume", args: { volume: 3 } });
    expect((await call(backend, "molty_imu", {}, token)).structuredContent).toMatchObject({ echo: "imu" });
    expect((await call(backend, "molty_volume", { volume: "3" }, token)).structuredContent.message).toBe("volume must be an integer");

    const say = await call(backend, "molty_say", { text: "hello" }, token);
    const job = say.structuredContent.job;
    expect(job).toMatch(/^molty-k7m2q9xa4c\.j_/);
    const pending = await call(backend, "molty_wait", { job, timeout_s: 0 }, token);
    expect(pending.structuredContent).toMatchObject({ ok: true, job, status: "pending" });
    const done = await call(backend, "molty_wait", { job, timeout_s: 5 }, token);
    expect(done.structuredContent).toMatchObject({ ok: true, job, status: "done", ms: 100, text: "hello" });
    expect(sock.bins.length).toBe(1);

    const listen = await call(backend, "molty_listen", { ms: 10 }, token);
    const failed = await call(backend, "molty_wait", { job: listen.structuredContent.job }, token);
    expect(failed).toMatchObject({ isError: true, structuredContent: { ok: false, error: "no_audio" } });
    expect((await call(backend, "molty_wait", { job, timeout_s: 9 }, token)).structuredContent.error).toBe("invalid_args");
  });

  it("molty_pair pending -> resume with pair_id", async () => {
    const ctx = makeRoom({ timings: FAST });
    const sock = await connect(ctx.room, ctx.host);
    const backend: RelayBackend = { device: () => ctx.room };
    const first = await call(backend, "molty_pair", { device_id: DEVICE, link_code: LINK, agent_name: "x", wait_s: 0 });
    expect(first.structuredContent).toMatchObject({ ok: true, status: "pending", code: expect.stringMatching(/^\d{4}$/) });
    const req = sock.last("pair.request");
    await ctx.room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: req.req, ok: true }));
    const second = await call(backend, "molty_pair", { device_id: DEVICE, pair_id: first.structuredContent.pair_id });
    expect(second.structuredContent).toMatchObject({ status: "approved" });
  });

  it("descriptions keep the local stdio server's wording where semantics match", () => {
    const d = Object.fromEntries(TOOLS.map((t) => [t.name, t.description]));
    expect(d.molty_volume).toBe("Set the physical speaker volume from 0 (mute) to 21 (maximum).");
    expect(d.molty_imu).toBe("Read one IMU snapshot: ax/ay/az acceleration in g, gx/gy/gz angular velocity in degrees/second, ms device sample timestamp.");
  });
});

describe("REST surface (envelopes agreed with the Go CLI)", () => {
  const req = (path: string, init: RequestInit & { token?: string } = {}) =>
    new Request(`https://relay.test${path}`, { ...init, headers: { "content-type": "application/json", ...(init.token ? { authorization: `Bearer ${init.token}` } : {}) } });

  it("pair -> cmd -> job", async () => {
    const { backend } = await setup();
    const p = await handleRest(req("/v1/pair", { method: "POST", body: JSON.stringify({ device_id: DEVICE, link_code: LINK, agent_name: "cli" }) }), backend);
    expect(p.status).toBe(200);
    const pb = (await p.json()) as any;
    expect(pb).toMatchObject({ agent_token: expect.any(String), device_id: DEVICE });
    const token = pb.agent_token;

    const c = await handleRest(req(`/v1/devices/${DEVICE}/cmd`, { method: "POST", token, body: JSON.stringify({ name: "imu" }) }), backend);
    expect(await c.json()).toEqual({ ok: true, result: { echo: "imu", args: {} } });

    const s = await handleRest(req(`/v1/devices/${DEVICE}/cmd`, { method: "POST", token, body: JSON.stringify({ name: "say", args: { text: "yo" } }) }), backend);
    const sb = (await s.json()) as any;
    expect(sb).toEqual({ ok: true, result: { job: expect.stringMatching(/^molty-k7m2q9xa4c\.j_/) } });
    const j0 = (await (await handleRest(req(`/v1/jobs/${sb.result.job}`, { token }), backend)).json()) as any;
    expect(["queued", "running"]).toContain(j0.status);
    const j = (await (await handleRest(req(`/v1/jobs/${sb.result.job}?wait_s=5`, { token }), backend)).json()) as any;
    expect(j).toEqual({ job: sb.result.job, kind: "say", status: "done", result: { ms: 100, text: "yo" } });
  });

  it("401 on bad or missing bearer, or a token for another device", async () => {
    const { backend } = await setup();
    const body = JSON.stringify({ name: "status" });
    expect((await handleRest(req(`/v1/devices/${DEVICE}/cmd`, { method: "POST", body }), backend)).status).toBe(401);
    expect((await handleRest(req(`/v1/devices/${DEVICE}/cmd`, { method: "POST", body, token: `${DEVICE}.${"A".repeat(43)}` }), backend)).status).toBe(401);
    expect((await handleRest(req(`/v1/devices/${DEVICE}/cmd`, { method: "POST", body, token: `molty-zzzzzzzzzz.${"A".repeat(43)}` }), backend)).status).toBe(401);
    const r = await handleRest(req(`/v1/jobs/${DEVICE}.j_abcdefghijkl`), backend);
    expect(r.status).toBe(401);
    expect(await r.json()).toEqual({ ok: false, error: "unauthorized" });
  });

  it("pair errors are {ok:false,error}", async () => {
    const { backend } = await setup();
    const r = await handleRest(req("/v1/pair", { method: "POST", body: JSON.stringify({ device_id: DEVICE, link_code: "WRNG2345", agent_name: "x" }) }), backend);
    expect(r.status).toBe(403);
    expect(await r.json()).toEqual({ ok: false, error: "bad_link_code" });
    const bad = await handleRest(req("/v1/pair", { method: "POST", body: "[]" }), backend);
    expect(bad.status).toBe(400);
  });

  it("revoke via DELETE /v1/token", async () => {
    const { backend } = await setup();
    const token = ((await (await handleRest(req("/v1/pair", { method: "POST", body: JSON.stringify({ device_id: DEVICE, link_code: LINK, agent_name: "x" }) }), backend)).json()) as any).agent_token;
    expect((await handleRest(req("/v1/token", { method: "DELETE", token }), backend)).status).toBe(200);
    expect((await handleRest(req(`/v1/devices/${DEVICE}/cmd`, { method: "POST", token, body: JSON.stringify({ name: "status" }) }), backend)).status).toBe(401);
  });
});
