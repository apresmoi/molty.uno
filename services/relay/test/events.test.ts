// Agent events: inbox, cursors, pull/long-poll, ride-along, webhooks.
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import ts from "typescript";
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { type AgentEvent, INBOX_LIMITS, SAMPLE_EVENTS as SAMPLE } from "../src/core/events.js";
import type { DeviceRoom } from "../src/core/room.js";
import { type Clock, MemoryKV } from "../src/core/store.js";
import { ESCAPE, checkWebhookUrl, guardWebhookUrl, isPrivateAddress, parseWebhookSpec, renderRequest, renderTemplate } from "../src/core/webhook.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, FakeHost, type FakeSocket, LINK, connect, flush, makeRoom } from "./helpers.js";

// TestClock scales timers 100x down: these are 150 ms / 4 s of real time.
const FAST = { cmdTimeoutMs: 15_000, pairWindowMs: 400_000 };

/** Manual time; timers run on real time scaled down 100x (5 s = 50 ms) and advance `t` when they fire. */
class TestClock implements Clock {
  t = Date.now();
  now() {
    return this.t;
  }
  setTimeout(fn: () => void, ms: number) {
    return globalThis.setTimeout(() => {
      this.t += ms;
      fn();
    }, ms / 100);
  }
  clearTimeout(h: unknown) {
    globalThis.clearTimeout(h as ReturnType<typeof setTimeout>);
  }
}

interface Sent {
  url: string;
  method: string;
  headers: Record<string, string>;
  body: string;
}

function autoAnswer(room: DeviceRoom, sock: FakeSocket) {
  const orig = sock.sendText.bind(sock);
  let n = 0;
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "cmd") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: { echo: f.name } })), 1);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: `NEXT${++n}AAA`.slice(0, 8).replace(/[^A-Z2-9]/g, "Z") })), 2);
  };
}

async function world(opts: { kv?: MemoryKV; host?: FakeHost; fetchImpl?: (r: Request) => Promise<Response>; resolve?: (h: string) => Promise<string[]> } = {}) {
  const clock = new TestClock();
  const alarms: number[] = [];
  const sent: Sent[] = [];
  const logs: string[] = [];
  const fetchImpl =
    opts.fetchImpl ??
    (async (r: Request) => {
      sent.push({ url: r.url, method: r.method, headers: Object.fromEntries(r.headers), body: await r.text() });
      return new Response("ok");
    });
  const ctx = makeRoom({
    kv: opts.kv,
    host: opts.host,
    timings: FAST,
    clock,
    scheduler: { setAlarm: async (at) => void alarms.push(at) },
    fetch: async (r) => {
      const res = await fetchImpl(r);
      return res;
    },
    resolveHost: opts.resolve ?? (async () => ["93.184.216.34"]),
    log: (msg, data) => logs.push(JSON.stringify({ msg, ...data })),
  });
  const sock = await connect(ctx.room, ctx.host);
  autoAnswer(ctx.room, sock);
  let code = LINK;
  const pairAgent = async (name: string) => {
    const out = await ctx.room.pair({ device_id: DEVICE, link_code: code, agent_name: name });
    if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
    await flush();
    const link = (await ctx.kv.get<{ code: string }>("link"))?.code;
    if (link) code = link;
    return out.result.agent_token;
  };
  const emit = (name: string, data: unknown) => ctx.room.socketMessage(sock, JSON.stringify({ t: "evt", name, data }));
  return { ...ctx, clock, sock, alarms, sent, logs, pairAgent, emit };
}

const shake = { peak_g: 2.4, reversals: 5, ms: 900 };
const evs = (out: any) => (out.result.events as AgentEvent[]).map((e) => e.type);

describe("inbox", () => {
  it("allow-list: only shake/tap/button with valid shapes reach agents; internal events stay internal", async () => {
    const w = await world();
    const token = await w.pairAgent("a");
    await w.emit("say.done", { job: "x" });
    await w.emit("link_code", { link_code: "QQQQ2345" });
    await w.emit("touch", { x: 1 });
    await w.emit("shake", { peak_g: "big", reversals: 1, ms: 1 });
    await w.emit("tap", { x: 1.5, y: 0 });
    await w.emit("button", { id: "maybe" });
    await w.emit("__proto__", {});
    expect((await w.room.events(token, { wait_ms: 0 })) as any).toMatchObject({ ok: true, result: { events: [], next: "e0" } });
    await w.emit("shake", { ...shake, extra: "dropped" });
    await w.emit("tap", { x: 0.25, y: 1 });
    await w.emit("button", { id: "accept" });
    const out = (await w.room.events(token, { wait_ms: 0 })) as any;
    expect(out.result.events).toEqual([
      { id: "e1", type: "shake", ts: w.clock.t, data: shake },
      { id: "e2", type: "tap", ts: w.clock.t, data: { x: 0.25, y: 1 } },
      { id: "e3", type: "button", ts: w.clock.t, data: { id: "accept" } },
    ]);
    expect(out.result.next).toBe("e3");
  });

  it("caps intake at 30 events/min per device, counts drops, reopens after a minute", async () => {
    const w = await world();
    const token = await w.pairAgent("a");
    for (let i = 0; i < 35; i++) await w.emit("tap", { x: 0, y: 0 });
    expect([...(await w.kv.list("ev:")).keys()]).toHaveLength(30);
    expect(await w.kv.get("evrate")).toMatchObject({ n: 30, dropped: 5 });
    w.clock.t += 60_000;
    await w.emit("tap", { x: 0, y: 0 });
    const out = (await w.room.events(token, { wait_ms: 0 })) as any;
    expect(out.result.events).toHaveLength(31);
    expect(out.result.next).toBe("e31");
  });

  it("keeps a ring of the last 200 events and nothing older than 24 h", async () => {
    const w = await world();
    const token = await w.pairAgent("a");
    for (let i = 0; i < 205; i++) {
      if (i % 30 === 0) w.clock.t += 60_000;
      await w.emit("tap", { x: 0, y: 0 });
    }
    const keys = [...(await w.kv.list("ev:")).keys()];
    expect(keys).toHaveLength(200);
    expect(keys[0]).toBe("ev:000000000006");
    w.clock.t += 24 * 3600_000 + 1;
    await w.emit("button", { id: "reject" });
    expect([...(await w.kv.list("ev:")).keys()]).toEqual(["ev:000000000206"]);
    const out = (await w.room.events(token, { wait_ms: 0 })) as any;
    expect(out.result.events.map((e: AgentEvent) => e.id)).toEqual(["e206"]);
  });

  it("per-token cursors: two agents each get every event exactly once; a new agent starts at the head", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    await w.emit("tap", { x: 0, y: 0 }); // before b exists
    const b = await w.pairAgent("b");
    await w.emit("shake", shake);
    await w.emit("button", { id: "accept" });
    expect(evs(await w.room.events(a, { wait_ms: 0 }))).toEqual(["tap", "shake", "button"]);
    expect(evs(await w.room.events(a, { wait_ms: 0 }))).toEqual([]);
    expect(evs(await w.room.events(b, { wait_ms: 0 }))).toEqual(["shake", "button"]);
    expect(evs(await w.room.events(b, { wait_ms: 0 }))).toEqual([]);
    // explicit since re-reads without moving the cursor backwards
    expect(evs(await w.room.events(a, { since: 1, wait_ms: 0 }))).toEqual(["shake", "button"]);
    await w.emit("tap", { x: 1, y: 1 });
    expect(evs(await w.room.events(a, { wait_ms: 0 }))).toEqual(["tap"]);
    expect((await w.room.events("nope", { wait_ms: 0 })).ok).toBe(false);
  });

  it("survives hibernation: a fresh room on the same storage keeps events and cursors", async () => {
    const kv = new MemoryKV();
    const host = new FakeHost();
    const w = await world({ kv, host });
    const a = await w.pairAgent("a");
    await w.emit("shake", shake);
    await w.emit("tap", { x: 0, y: 0 });
    expect(evs(await w.room.events(a, { wait_ms: 0 }))).toEqual(["shake", "tap"]);
    await w.emit("button", { id: "reject" });
    const fresh = makeRoom({ kv, host, timings: FAST });
    expect(evs(await fresh.room.events(a, { wait_ms: 0 }))).toEqual(["button"]);
    expect(evs(await fresh.room.events(a, { wait_ms: 0 }))).toEqual([]);
  });

  it("long-poll wakes on append instead of waiting out the budget", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    const started = Date.now();
    const pending = w.room.events(a, { wait_ms: 20_000 }); // 200 ms on the scaled clock
    setTimeout(() => void w.emit("shake", shake), 20);
    const out = (await pending) as any;
    expect(evs(out)).toEqual(["shake"]);
    expect(Date.now() - started).toBeLessThan(150);
    const empty = (await w.room.events(a, { wait_ms: 1000 })) as any; // times out empty
    expect(empty.result).toEqual({ events: [], next: "e1", more: false });
  });

  it("ride-along: every authenticated outcome carries unread events once and advances the cursor", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    const plain = await w.room.command(a, "imu", {});
    expect(plain).toEqual({ ok: true, result: { echo: "imu" } }); // no events = unchanged
    await w.emit("shake", shake);
    const withEv = await w.room.command(a, "imu", {});
    expect(withEv).toMatchObject({ ok: true, result: { echo: "imu" }, inbox: { events: [{ id: "e1", type: "shake" }], more: false } });
    expect(await w.room.command(a, "imu", {})).toEqual({ ok: true, result: { echo: "imu" } });
    expect(evs(await w.room.events(a, { wait_ms: 0 }))).toEqual([]);
    // errors carry nothing and move nothing; the next success gets them, at most 10 at a time
    for (let i = 0; i < 12; i++) await w.emit("tap", { x: 0, y: 0 });
    const bad = await w.room.command(a, "nope", {});
    expect(bad).toEqual({ ok: false, error: "unknown_command", message: "unknown command nope" });
    expect(await w.room.job(a, `${DEVICE}.j_abcdefghijkl`, 0)).toEqual({ ok: false, error: "not_found" });
    const good = await w.room.command(a, "imu", {});
    expect(good.inbox).toMatchObject({ more: true });
    expect(good.inbox!.events).toHaveLength(10);
    expect((await w.room.command(a, "imu", {})).inbox!.events).toHaveLength(2);
  });

  it("revoke and forget remove cursors, events and queued deliveries", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    await w.room.webhook(a, { action: "set", spec: { url: "https://hooks.example.org/x" } });
    await w.emit("shake", shake);
    expect([...(await w.kv.list("whq:")).keys()]).toHaveLength(1);
    await w.room.revoke(a);
    expect([...(await w.kv.list("cur:")).keys()]).toHaveLength(0);
    expect([...(await w.kv.list("whq:")).keys()]).toHaveLength(0);
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "forget" }));
    expect(w.kv.data.size).toBe(0);
  });
});

// ---------------------------------------------------------------- surfaces

async function surfaceWorld() {
  const w = await world();
  const backend: RelayBackend = { device: () => w.room };
  const token = await w.pairAgent("s");
  return { ...w, backend, token };
}

const rest = (b: RelayBackend, path: string, init: RequestInit & { token?: string } = {}) =>
  handleRest(new Request(`https://relay.test${path}`, { ...init, headers: { "content-type": "application/json", ...(init.token ? { authorization: `Bearer ${init.token}` } : {}) } }), b);

let rid = 0;
async function mcpCall(b: RelayBackend, name: string, args: unknown, token: string) {
  const res = await handleMcp(
    new Request("https://relay.test/mcp", { method: "POST", headers: { authorization: `Bearer ${token}` }, body: JSON.stringify({ jsonrpc: "2.0", id: ++rid, method: "tools/call", params: { name, arguments: args } }) }),
    b,
  );
  return ((await res.json()) as any).result;
}

describe("surfaces", () => {
  it("REST: events endpoint, additive `events` on cmd/job responses, argument limits", async () => {
    const { backend, token, emit } = await surfaceWorld();
    const cmd = () => rest(backend, `/v1/devices/${DEVICE}/cmd`, { method: "POST", token, body: JSON.stringify({ name: "imu" }) });
    expect(await (await cmd()).json()).toEqual({ ok: true, result: { echo: "imu" } });
    await emit("shake", shake);
    const body = (await (await cmd()).json()) as any;
    expect(body).toMatchObject({ ok: true, result: { echo: "imu" }, events: [{ id: "e1", type: "shake", data: shake }] });
    expect(await (await cmd()).json()).toEqual({ ok: true, result: { echo: "imu" } });

    await emit("tap", { x: 0.5, y: 0.5 });
    const ev = await rest(backend, `/v1/devices/${DEVICE}/events?wait_s=0`, { token });
    expect(ev.status).toBe(200);
    expect(await ev.json()).toMatchObject({ ok: true, events: [{ id: "e2", type: "tap" }], next: "e2", more: false });
    const again = (await (await rest(backend, `/v1/devices/${DEVICE}/events?since=e0`, { token })).json()) as any;
    expect(again.events.map((e: AgentEvent) => e.id)).toEqual(["e1", "e2"]);
    expect((await rest(backend, `/v1/devices/${DEVICE}/events?wait_s=26`, { token })).status).toBe(400);
    expect((await rest(backend, `/v1/devices/${DEVICE}/events?since=x9`, { token })).status).toBe(400);
    expect((await rest(backend, `/v1/devices/${DEVICE}/events`)).status).toBe(401);
    expect((await rest(backend, `/v1/devices/molty-zzzzzzzzzz/events`, { token })).status).toBe(401);
    expect((await rest(backend, `/v1/devices/${DEVICE}/events`, { method: "POST", token })).status).toBe(405);
  });

  it("MCP: events in structuredContent + a 'Molty:' text line, only when unread; molty_events validates", async () => {
    const { backend, token, emit } = await surfaceWorld();
    const quiet = await mcpCall(backend, "molty_imu", {}, token);
    expect(quiet.content).toHaveLength(1);
    expect(quiet.structuredContent).toEqual({ ok: true, echo: "imu" });
    await emit("shake", shake);
    const loud = await mcpCall(backend, "molty_imu", {}, token);
    expect(loud.structuredContent).toMatchObject({ ok: true, echo: "imu", events: [{ id: "e1", type: "shake" }] });
    expect(JSON.parse(loud.content[0].text)).toEqual(loud.structuredContent);
    expect(loud.content[1].text).toMatch(/^Molty: was shaken \(peak 2\.4 g\) \d+ s ago \[e1\]\.$/);
    await emit("button", { id: "accept" });
    const pulled = await mcpCall(backend, "molty_events", { wait_s: 0 }, token);
    expect(pulled.structuredContent).toMatchObject({ ok: true, events: [{ type: "button" }], next: "e2" });
    expect(pulled.content[1].text).toMatch(/accept button was pressed/);
    expect((await mcpCall(backend, "molty_events", { wait_s: 9 }, token)).structuredContent.error).toBe("invalid_args");
    expect((await mcpCall(backend, "molty_events", { since: "yesterday" }, token)).structuredContent.error).toBe("invalid_args");
    expect((await mcpCall(backend, "molty_webhook", { url: "https://a.example.org", method: "DELETE" }, token)).structuredContent.message).toBe("method must be one of POST, GET, PUT, PATCH");
    expect((await mcpCall(backend, "molty_webhook", { url: "https://a.example.org", headers: "x" }, token)).structuredContent.message).toBe("headers must be an object");
    expect((await mcpCall(backend, "molty_webhook", { remove: "yes" }, token)).structuredContent.message).toBe("remove must be a boolean");
  });

  it("REST + MCP webhook lifecycle: register (secret once), read redacted, remove", async () => {
    const { backend, token } = await surfaceWorld();
    const put = await rest(backend, "/v1/webhook", { method: "PUT", token, body: JSON.stringify({ url: "https://hooks.example.org/bot123:SECRETPATH/send", headers: { "X-Api-Key": "sk-live-SUPERSECRET" } }) });
    expect(put.status).toBe(200);
    const pb = (await put.json()) as any;
    expect(pb.result.secret).toMatch(/^whsec_[A-Za-z0-9_-]{32}$/);
    expect(pb.result.dry_run).toMatchObject({ method: "POST", url: "https://hooks.example.org/…", headers: { "X-Api-Key": "[redacted]", "content-type": "application/json" } });
    const got = await (await rest(backend, "/v1/webhook", { token })).text();
    const viaMcp = JSON.stringify(await mcpCall(backend, "molty_webhook", {}, token));
    const status = JSON.stringify(await mcpCall(backend, "molty_status", {}, token));
    for (const text of [got, viaMcp, status, JSON.stringify(pb.result.dry_run), JSON.stringify(pb.result.webhook)]) {
      expect(text).not.toContain("SUPERSECRET");
      expect(text).not.toContain("SECRETPATH");
      expect(text).not.toContain(pb.result.secret);
    }
    expect(JSON.parse(got).result.webhook).toMatchObject({ url: "https://hooks.example.org/…", headers: ["X-Api-Key"], active: true });
    expect(JSON.parse(status).structuredContent.relay.webhook).toMatchObject({ active: true });
    expect(await (await rest(backend, "/v1/webhook", { method: "DELETE", token })).json()).toEqual({ ok: true, result: { removed: true } });
    expect((await mcpCall(backend, "molty_webhook", {}, token)).structuredContent).toEqual({ ok: true, webhook: null });
    expect((await rest(backend, "/v1/webhook", { method: "POST", token })).status).toBe(405);
    expect((await rest(backend, "/v1/webhook", {})).status).toBe(401);
  });
});

// ---------------------------------------------------------------- webhooks

const docs = readFileSync(new URL("../../../docs/AGENT-LINK.md", import.meta.url), "utf8");
function docBlock(marker: string): string {
  const m = new RegExp(`<!-- verifier:${marker} -->\\n\`\`\`\\w+\\n([\\s\\S]*?)\`\`\``).exec(docs);
  if (!m) throw new Error(`no ${marker} verifier in docs`);
  return m[1];
}
async function tsVerifier(): Promise<(secret: string, header: string | null, method: string, target: string, body: string, tol?: number) => Promise<boolean>> {
  const js = ts.transpileModule(docBlock("ts"), { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext } }).outputText;
  const mod = await import(`data:text/javascript,${encodeURIComponent(js)}`);
  return mod.verifyMolty;
}
function pyVerify(secret: string, header: string, method: string, target: string, body: string): boolean | "skipped" {
  const dir = mkdtempSync(join(tmpdir(), "molty-verifier-"));
  const file = join(dir, "check.py");
  writeFileSync(file, `${docBlock("py")}\nimport sys\nprint(verify_molty(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5].encode()))\n`);
  try {
    return execFileSync("python3", ["-I", file, secret, header, method, target, body], { encoding: "utf8" }).trim() === "True";
  } catch (e: any) {
    if (e.code === "ENOENT") return "skipped";
    throw e;
  }
}

async function hooked(spec: Record<string, unknown>, extra: Parameters<typeof world>[0] = {}) {
  const w = await world(extra);
  const token = await w.pairAgent("h");
  const reg = (await w.room.webhook(token, { action: "set", spec })) as any;
  if (!reg.ok) throw new Error(JSON.stringify(reg));
  return { ...w, token, secret: reg.result.secret as string, reg };
}

describe("webhooks", () => {
  it("default body is signed; the documented TS and Python verifiers accept it and reject tampering", async () => {
    const w = await hooked({ url: "https://hooks.example.org/molty" });
    await w.emit("shake", shake);
    expect(w.alarms).toEqual([w.clock.t]); // first attempt is due immediately
    await w.room.alarm();
    expect(w.sent).toHaveLength(1);
    const req = w.sent[0];
    expect(req).toMatchObject({ url: "https://hooks.example.org/molty", method: "POST", headers: { "content-type": "application/json" } });
    expect(JSON.parse(req.body)).toEqual({ device: DEVICE, event: { id: "e1", type: "shake", ts: w.clock.t, data: shake } });
    const sig = req.headers["molty-signature"];
    expect(sig).toMatch(/^t=\d+,v2=[0-9a-f]{64}$/);
    const verify = await tsVerifier();
    expect(await verify(w.secret, sig, "POST", "/molty", req.body)).toBe(true);
    expect(await verify(w.secret, sig, "POST", "/molty", req.body.replace("2.4", "9.9"))).toBe(false);
    expect(await verify(w.secret, sig, "PUT", "/molty", req.body)).toBe(false);
    expect(await verify(w.secret, sig, "POST", "/other", req.body)).toBe(false);
    expect(await verify("whsec_wrong", sig, "POST", "/molty", req.body)).toBe(false);
    expect(await verify(w.secret, null, "POST", "/molty", req.body)).toBe(false);
    expect(await verify(w.secret, sig.replace(/t=\d+/, "t=1000"), "POST", "/molty", req.body)).toBe(false); // stale + mismatched
    const py = pyVerify(w.secret, sig, "POST", "/molty", req.body);
    if (py !== "skipped") {
      expect(py).toBe(true);
      expect(pyVerify(w.secret, sig, "POST", "/molty", req.body + " ")).toBe(false);
      expect(pyVerify(w.secret, sig, "POST", "/molty?x=1", req.body)).toBe(false);
    }
    // webhook acks do not touch the read cursor: the inbox stays the record
    expect(evs(await w.room.events(w.token, { wait_ms: 0 }))).toEqual(["shake"]);
    expect(await w.kv.list("whq:")).toEqual(new Map());
  });

  it("agent-defined request: placeholders rendered and escaped per target, custom method/headers", async () => {
    const w = await hooked({
      url: "https://bot.example.org/bot<TOKEN>/send?text={{text}}&id={{event.id}}",
      method: "PUT",
      headers: { Authorization: "Bearer agent-own-secret" },
      content_type: "application/json",
      body: '{"msg":"{{text}}","type":"{{event.type}}","data":{{event.data}},"peak":{{event.data.peak_g}},"dev":"{{device}}","ts":{{event.ts}}}',
    });
    expect(w.reg.result.dry_run.body).toContain('"msg":"Molty was shaken (peak 2.4 g)"');
    await w.emit("shake", shake);
    await w.room.alarm();
    const req = w.sent[0];
    expect(req.method).toBe("PUT");
    expect(req.headers.authorization).toBe("Bearer agent-own-secret");
    expect(req.url).toBe("https://bot.example.org/bot%3CTOKEN%3E/send?text=Molty%20was%20shaken%20(peak%202.4%20g)&id=e1");
    expect(JSON.parse(req.body)).toEqual({ msg: "Molty was shaken (peak 2.4 g)", type: "shake", data: shake, peak: 2.4, dev: DEVICE, ts: w.clock.t });
  });

  it("template escaping: JSON strings, form bodies, raw text; quotes and newlines cannot break out", () => {
    const ev: AgentEvent = { id: "e7", type: "tap", ts: 5, data: { note: 'a"b\n</x>&y=1', n: 3 } as any };
    expect(renderTemplate('{"v":"{{event.data.note}}"}', ev, DEVICE, ESCAPE.json)).toBe('{"v":"a\\"b\\n</x>&y=1"}');
    expect(JSON.parse(renderTemplate('{"v":"{{event.data.note}}","n":{{event.data.n}},"d":{{event.data}}}', ev, DEVICE, ESCAPE.json))).toEqual({ v: 'a"b\n</x>&y=1', n: 3, d: ev.data });
    expect(renderTemplate("v={{event.data.note}}&m={{event.data.missing}}", ev, DEVICE, ESCAPE.url)).toBe("v=a%22b%0A%3C%2Fx%3E%26y%3D1&m=");
    expect(renderTemplate("{{event.data.note}}", ev, DEVICE, ESCAPE.raw)).toBe('a"b\n</x>&y=1');
    const form = renderRequest({ url: "https://x.example.org", method: "POST", headers: {}, body: "text={{text}}", content_type: "application/x-www-form-urlencoded" }, DEVICE, ev);
    expect(form.body).toBe("text=Molty's%20screen%20was%20tapped");
    const plain = renderRequest({ url: "https://x.example.org", method: "POST", headers: {}, body: "{{text}} / {{event.type}}" }, DEVICE, ev);
    expect(plain).toMatchObject({ body: "Molty's screen was tapped / tap", headers: { "content-type": "text/plain; charset=utf-8" } });
    expect(renderRequest({ url: "https://x.example.org/{{event.type}}", method: "GET", headers: {} }, DEVICE, ev)).toMatchObject({ url: "https://x.example.org/tap", body: "" });
  });

  it("registration dry-run rejects bad templates, headers and sizes with a reason", async () => {
    const w = await world();
    const t = await w.pairAgent("v");
    const reject = async (spec: Record<string, unknown>, msg: RegExp) => {
      const out = (await w.room.webhook(t, { action: "set", spec })) as any;
      expect(out, JSON.stringify(spec)).toMatchObject({ ok: false, error: "invalid_args" });
      expect(out.message).toMatch(msg);
    };
    await reject({ url: "https://a.example.org", content_type: "application/json", body: '{"x":"{{event.data}}"' }, /valid JSON/);
    await reject({ url: "https://a.example.org", body: "{{secrets}}" }, /unknown placeholder/);
    await reject({ url: "https://a.example.org", method: "GET", body: "x" }, /GET takes no body/);
    await reject({ url: "https://a.example.org", headers: { Host: "evil" } }, /reserved/);
    await reject({ url: "https://a.example.org", headers: { "Molty-Signature": "forged" } }, /reserved/);
    await reject({ url: "https://a.example.org", headers: { "Content-Type": "x/y" } }, /content_type/);
    await reject({ url: "https://a.example.org", headers: { X: "a\r\nInjected: 1" } }, /single-line/);
    await reject({ url: "https://a.example.org", headers: Object.fromEntries(Array.from({ length: 17 }, (_, i) => [`h${i}`, "v"])) }, /at most 16/);
    await reject({ url: "https://a.example.org", body: "x".repeat(4097) }, /4096/);
    await reject({ url: `https://a.example.org/${"x".repeat(2048)}` }, /2048/);
    await reject({ url: "https://a.example.org", content_type: "json please" }, /media type/);
    expect(await w.kv.get<any>(`tok:${[...(await w.kv.list("tok:")).keys()][0].slice(4)}`)).not.toHaveProperty("webhook");
    expect(parseWebhookSpec({ url: "https://a.example.org", method: "patch" })).toMatchObject({ ok: true, value: { method: "PATCH" } });
  });

  it("SSRF guard: https only, no IP literals, no local names, no private resolutions, no credentials", async () => {
    for (const url of [
      "http://hooks.example.org/x",
      "https://127.0.0.1/x",
      "https://10.0.0.1/",
      "https://[::1]/",
      "https://[fe80::1]/",
      "https://2130706433/",
      "https://0x7f000001/",
      "https://localhost/",
      "https://printer.local/",
      "https://foo.localhost/",
      "https://metadata.internal/",
      "https://router/",
      "https://user:pw@hooks.example.org/",
      "ftp://hooks.example.org/",
      "not a url",
    ]) {
      expect(checkWebhookUrl(url).ok, url).toBe(false);
    }
    expect(checkWebhookUrl("https://hooks.example.org:8443/a?b=c").ok).toBe(true);
    for (const ip of ["10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1", "127.0.0.1", "169.254.169.254", "100.64.0.1", "0.0.0.0", "224.0.0.1", "::1", "fc00::1", "fd12::1", "fe80::1", "::ffff:10.0.0.1", "ff02::1", "garbage"]) {
      expect(isPrivateAddress(ip), ip).toBe(true);
    }
    for (const ip of ["93.184.216.34", "1.1.1.1", "172.32.0.1", "2606:4700::1111"]) expect(isPrivateAddress(ip), ip).toBe(false);
    expect(await guardWebhookUrl("https://rebind.example.org/", async () => ["93.184.216.34", "10.0.0.5"])).toMatchObject({ ok: false, message: expect.stringMatching(/private/) });
    expect(await guardWebhookUrl("https://gone.example.org/", async () => [])).toMatchObject({ ok: false });
    expect(await guardWebhookUrl("https://err.example.org/", async () => Promise.reject(new Error("x")))).toMatchObject({ ok: false });
    // through the room: registration refuses; a host that later resolves privately fails delivery
    let addrs = ["93.184.216.34"];
    const w = await hooked({ url: "https://flip.example.org/h" }, { resolve: async () => addrs });
    expect((await w.room.webhook(w.token, { action: "set", spec: { url: "https://10.0.0.1/" } })).ok).toBe(false);
    addrs = ["192.168.0.10"];
    await w.emit("tap", { x: 0, y: 0 });
    await w.room.alarm();
    expect(w.sent).toHaveLength(0);
    expect(((await w.room.webhook(w.token, { action: "get" })) as any).result.webhook.last_error).toBe("blocked_address");
  });

  it("retries 1 s / 10 s / 60 s via the alarm, then drops; success resets the failure count", async () => {
    let status = 500;
    const calls: number[] = [];
    const w = await hooked({ url: "https://hooks.example.org/r" }, { fetchImpl: async () => (calls.push(Date.now()), new Response(null, { status })) });
    await w.emit("shake", shake);
    const t0 = w.clock.t;
    await w.room.alarm();
    expect(w.alarms.at(-1)).toBe(t0 + 1000);
    await w.room.alarm(); // not due yet: nothing sent
    expect(calls).toHaveLength(1);
    w.clock.t = t0 + 1000;
    await w.room.alarm();
    expect(w.alarms.at(-1)).toBe(t0 + 1000 + 10_000);
    w.clock.t += 10_000;
    await w.room.alarm();
    expect(w.alarms.at(-1)).toBe(w.clock.t + 60_000);
    w.clock.t += 60_000;
    await w.room.alarm();
    expect(calls).toHaveLength(4);
    expect(await w.kv.list("whq:")).toEqual(new Map()); // gave up on this event
    const view = ((await w.room.webhook(w.token, { action: "get" })) as any).result.webhook;
    expect(view).toMatchObject({ active: true, consecutive_failures: 4, last_error: "http_500" });
    status = 204;
    await w.emit("tap", { x: 0, y: 0 });
    await w.room.alarm();
    expect(((await w.room.webhook(w.token, { action: "get" })) as any).result.webhook).toMatchObject({ consecutive_failures: 0, last_ok_at: w.clock.t });
  });

  it("redirects are not followed and time out after 5 s count as failures", async () => {
    let mode: "redirect" | "hang" = "redirect";
    const modes: string[] = [];
    const w = await hooked({ url: "https://hooks.example.org/x" }, { fetchImpl: (r) => (modes.push(r.redirect), mode === "redirect" ? Promise.resolve(new Response(null, { status: 302, headers: { location: "https://10.0.0.1/" } })) : new Promise<Response>((_, rej) => r.signal.addEventListener("abort", () => rej(new Error("aborted"))))) });
    await w.emit("tap", { x: 0, y: 0 });
    await w.room.alarm();
    expect(((await w.room.webhook(w.token, { action: "get" })) as any).result.webhook.last_error).toBe("http_302");
    expect(modes).toEqual(["manual"]); // never follow: a redirect could point anywhere
    mode = "hang";
    w.clock.t += 1000;
    const started = Date.now();
    await w.room.alarm(); // scaled clock: 5 s timeout = 50 ms
    expect(Date.now() - started).toBeLessThan(1000);
    expect(((await w.room.webhook(w.token, { action: "get" })) as any).result.webhook.last_error).toBe("timeout");
  });

  it("auto-disables after 20 consecutive failed attempts; visible in status; re-register re-enables", async () => {
    const w = await hooked({ url: "https://hooks.example.org/dead" }, { fetchImpl: async () => Promise.reject(new Error("connection refused")) });
    for (let i = 0; i < 5; i++) {
      w.clock.t += 60_000; // keep under the intake cap
      await w.emit("tap", { x: 0, y: 0 });
    }
    for (let round = 0; round < 6; round++) {
      await w.room.alarm();
      w.clock.t += 61_000;
    }
    const status = (await w.room.command(w.token, "status", {})) as any;
    expect(status.result.relay.webhook).toMatchObject({ active: false, consecutive_failures: 20, disabled: expect.stringMatching(/20 consecutive/) });
    expect(await w.kv.list("whq:")).toEqual(new Map());
    await w.emit("tap", { x: 0, y: 0 });
    expect(await w.kv.list("whq:")).toEqual(new Map()); // disabled: nothing queued
    expect(w.logs.join("\n")).not.toContain("/dead"); // logs carry the origin only
    const again = (await w.room.webhook(w.token, { action: "set", spec: { url: "https://hooks.example.org/alive" } })) as any;
    expect(again.result.webhook).toMatchObject({ active: true, consecutive_failures: 0 });
  });

  it("each agent has its own webhook; deliveries go only to agents that registered one", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    const b = await w.pairAgent("b");
    await w.room.webhook(a, { action: "set", spec: { url: "https://a.example.org/h" } });
    await w.emit("shake", shake);
    await w.room.alarm();
    expect(w.sent.map((s) => s.url)).toEqual(["https://a.example.org/h"]);
    expect(evs(await w.room.events(b, { wait_ms: 0 }))).toEqual(["shake"]);
    expect(evs(await w.room.events(a, { wait_ms: 0 }))).toEqual(["shake"]); // delivery is not a read
  });
});

// ---------------------------------------------------------------- review round 1 (one test per finding)

describe("review fixes", () => {
  it("P1-1: a registration racing a revoke never resurrects the token; racing a replacement returns conflict", async () => {
    let release: () => void = () => {};
    const gate = new Promise<void>((r) => (release = r));
    const w = await world({ resolve: async (host) => (host.startsWith("slow.") ? gate.then(() => ["93.184.216.34"]) : ["93.184.216.34"]) });
    const a = await w.pairAgent("a");
    const pending = w.room.webhook(a, { action: "set", spec: { url: "https://slow.example.org/h" } });
    await flush();
    expect(await w.room.revoke(a)).toMatchObject({ ok: true });
    release();
    expect(await pending).toMatchObject({ ok: false, error: "unauthorized" });
    expect(await w.room.command(a, "imu", {})).toMatchObject({ ok: false, error: "unauthorized" });
    expect([...(await w.kv.list("tok:")).keys()]).toHaveLength(0);

    let release2: () => void = () => {};
    const gate2 = new Promise<void>((r) => (release2 = r));
    const w2 = await world({ resolve: async (host) => (host.startsWith("slow.") ? gate2.then(() => ["93.184.216.34"]) : ["93.184.216.34"]) });
    const b = await w2.pairAgent("b");
    const slow = w2.room.webhook(b, { action: "set", spec: { url: "https://slow.example.org/h" } });
    await flush();
    const fast = (await w2.room.webhook(b, { action: "set", spec: { url: "https://fast.example.org/h" } })) as any;
    release2();
    expect(await slow).toMatchObject({ ok: false, error: "conflict" });
    expect(((await w2.room.webhook(b, { action: "get" })) as any).result.webhook.url).toBe("https://fast.example.org/…");
    expect(fast.result.secret).toMatch(/^whsec_/);
  });

  it("P1-2: every IPv6 spelling of private/loopback/embedded-private IPv4 is refused; malformed answers too", () => {
    for (const ip of [
      "::ffff:7f00:1", "::ffff:a00:1", "::ffff:c0a8:101", "::FFFF:127.0.0.1", "0:0:0:0:0:0:0:1", "0000:0000:0000:0000:0000:0000:0000:0001",
      "0::1", "::0:1", "::", "::127.0.0.1", "::a00:1", "::8.8.8.8", "64:ff9b::a00:1", "64:ff9b::7f00:1", "64:ff9b:1::1",
      "2002:a00:1::1", "2002:7f00:1::", "2001:0:4136:e378:8000:63bf:3fff:fdd2", "2001:db8::1", "2001:10::1", "3fff::1",
      "fe80::1%eth0", "fec0::1", "100::1", "5f00::1", "::ffff:0:0", "1::", "2001:db8::1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "12345::1", "::1::", "1.2.3", "01.2.3.4", "256.1.1.1", "",
    ]) {
      expect(isPrivateAddress(ip), ip).toBe(true);
    }
    for (const ip of ["2606:4700:4700::1111", "2a00:1450:4001:80b::200e", "64:ff9b::808:808", "2002:808:808::1", "::ffff:8.8.8.8", "::ffff:808:808", "8.8.8.8"]) {
      expect(isPrivateAddress(ip), ip).toBe(false);
    }
  });

  it("P1-3: molty_wait keeps ride-along events on pending and done results; a failed job carries none and moves nothing", async () => {
    const { backend, token, emit, room, voice } = await surfaceWorld();
    voice.ttsSamples = 16000 * 30; // 30 s of audio = 300 ms on the scaled clock: still pending below
    const say = await mcpCall(backend, "molty_say", { text: "hi" }, token);
    await emit("shake", shake);
    const pending = await mcpCall(backend, "molty_wait", { job: say.structuredContent.job, timeout_s: 0 }, token);
    expect(pending.structuredContent).toMatchObject({ status: "pending", events: [{ id: "e1", type: "shake" }] });
    expect(pending.content[1].text).toMatch(/^Molty: was shaken/);
    await emit("tap", { x: 0, y: 0 });
    const waits = [pending];
    while (waits.length < 400 && waits.at(-1).structuredContent.status !== "done") waits.push(await mcpCall(backend, "molty_wait", { job: say.structuredContent.job, timeout_s: 1 }, token));
    expect(waits.at(-1).structuredContent).toMatchObject({ status: "done" });
    const all = waits.flatMap((r) => r.structuredContent.events ?? []).map((e: AgentEvent) => e.id);
    expect(all).toEqual(["e1", "e2"]); // nothing silently consumed
    const listen = await mcpCall(backend, "molty_listen", { ms: 10 }, token); // fake device never streams: no_audio
    let failed;
    for (let i = 0; i < 50 && !failed?.isError; i++) failed = await mcpCall(backend, "molty_wait", { job: listen.structuredContent.job, timeout_s: 1 }, token);
    await emit("button", { id: "reject" });
    failed = await mcpCall(backend, "molty_wait", { job: listen.structuredContent.job, timeout_s: 0 }, token);
    expect(failed).toMatchObject({ isError: true, structuredContent: { ok: false, error: "no_audio" } });
    expect(failed.structuredContent.events).toBeUndefined();
    expect(evs(await room.events(token, { wait_ms: 0 }))).toEqual(["button"]);
  });

  it("P1-4: a later webhook success never hides an earlier undelivered event; exhausted events stay readable", async () => {
    const w = await hooked({ url: "https://hooks.example.org/x" }, { fetchImpl: async (r) => new Response(null, { status: (JSON.parse(await r.text()).event.id === "e1" ? 500 : 204) }) });
    await w.emit("shake", shake);
    await w.emit("tap", { x: 0, y: 0 });
    for (let i = 0; i < 5; i++) {
      await w.room.alarm();
      w.clock.t += 61_000;
    }
    expect(await w.kv.list("whq:")).toEqual(new Map()); // e1 exhausted its retries, e2 delivered
    expect(evs(await w.room.events(w.token, { wait_ms: 0 }))).toEqual(["shake", "tap"]);
  });

  it("P1-5: GET deliveries sign the request target: a swapped query fails verification", async () => {
    const w = await hooked({ url: "https://bot.example.org/send?type={{event.type}}&id={{event.id}}", method: "GET" });
    await w.emit("tap", { x: 0, y: 0 });
    await w.room.alarm();
    const req = w.sent[0];
    const u = new URL(req.url);
    expect(u.search).toBe("?type=tap&id=e1");
    const verify = await tsVerifier();
    const sig = req.headers["molty-signature"];
    expect(await verify(w.secret, sig, "GET", u.pathname + u.search, "")).toBe(true);
    expect(await verify(w.secret, sig, "GET", "/send?type=shake&id=e1", "")).toBe(false);
    const py = pyVerify(w.secret, sig, "GET", u.pathname + u.search, "");
    if (py !== "skipped") expect(py).toBe(true);
  });

  it("P2-6: failures are stored, logged and shown as fixed categories, never exception text", async () => {
    const w = await hooked({ url: "https://hooks.example.org/bot999:SECRETPATH/send?key=QUERYSECRET" }, {
      fetchImpl: async (r) => Promise.reject(new TypeError(`fetch failed: ${r.url}`)),
    });
    await w.emit("tap", { x: 0, y: 0 });
    await w.room.alarm();
    const view = JSON.stringify(await w.room.webhook(w.token, { action: "get" }));
    expect(JSON.parse(view).result.webhook.last_error).toBe("network_error");
    for (const text of [view, w.logs.join("\n"), JSON.stringify([...w.kv.data.values()].filter((v: any) => v?.webhook).map((v: any) => v.webhook.last_error))]) {
      expect(text).not.toContain("SECRETPATH");
      expect(text).not.toContain("QUERYSECRET");
    }
  });

  it("P2-7: one deadline covers DNS + request: a hanging resolver fails as `timeout` (delivery) or quickly (registration)", async () => {
    let hang = false;
    const hangingResolver = (_h: string, signal?: AbortSignal) =>
      hang ? new Promise<string[]>((_, rej) => signal?.addEventListener("abort", () => rej(new Error("aborted")))) : Promise.resolve(["93.184.216.34"]);
    const w = await hooked({ url: "https://hooks.example.org/x" }, { resolve: hangingResolver });
    hang = true;
    await w.emit("tap", { x: 0, y: 0 });
    const started = Date.now();
    await w.room.alarm(); // scaled clock: 5 s = 50 ms
    expect(Date.now() - started).toBeLessThan(1000);
    expect(((await w.room.webhook(w.token, { action: "get" })) as any).result.webhook).toMatchObject({ last_error: "timeout", consecutive_failures: 1 });
    expect([...(await w.kv.list("whq:")).values()]).toMatchObject([{ attempt: 1 }]); // counted, retried
    const reg = (await w.room.webhook(w.token, { action: "set", spec: { url: "https://other.example.org/x" } })) as any;
    expect(reg).toMatchObject({ ok: false, error: "invalid_args", message: expect.stringMatching(/in time/) });
  });

  it("P2-8: long-polls are capped per token and per device; an aborted poll frees its slot at once", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    const b = await w.pairAgent("b");
    const c1 = new AbortController();
    const p1 = w.room.events(a, { wait_ms: 20_000, signal: c1.signal });
    const p2 = w.room.events(a, { wait_ms: 20_000 });
    await flush();
    expect(await w.room.events(a, { wait_ms: 20_000 })).toMatchObject({ ok: false, error: "too_many_polls" });
    expect(await w.room.events(a, { wait_ms: 0 })).toMatchObject({ ok: true }); // immediate reads are not counted
    const t0 = Date.now();
    c1.abort();
    expect(await p1).toMatchObject({ ok: false, error: "cancelled" });
    expect(Date.now() - t0).toBeLessThan(100); // freed now, not at its 200 ms (scaled) deadline
    const p3 = w.room.events(a, { wait_ms: 20_000 }); // slot freed by the abort
    const saved = INBOX_LIMITS.pollsPerDevice;
    INBOX_LIMITS.pollsPerDevice = 3;
    try {
      await flush();
      const p4 = w.room.events(b, { wait_ms: 20_000 });
      await flush();
      expect(await w.room.events(b, { wait_ms: 20_000 })).toMatchObject({ ok: false, error: "too_many_polls" });
      await w.emit("tap", { x: 0, y: 0 });
      for (const p of [p2, p3, p4]) expect(evs(await p)).toEqual(["tap"].slice(0, 1));
    } finally {
      INBOX_LIMITS.pollsPerDevice = saved;
    }
    expect((await rest({ device: () => w.room }, `/v1/devices/${DEVICE}/events?wait_s=0`, { token: a })).status).toBe(200);
  });

  it("P2-9: templates are validated for every event type; missing fields render null/default; an events filter limits delivery", async () => {
    const w = await world();
    const t = await w.pairAgent("t");
    const json = { url: "https://hooks.example.org/x", content_type: "application/json" };
    // valid for a shake, invalid for a tap (explicit empty default): only an all-types dry run catches it
    const strict = (await w.room.webhook(t, { action: "set", spec: { ...json, body: '{"v":{{event.data.peak_g|}}}' } })) as any;
    expect(strict).toMatchObject({ ok: false, error: "invalid_args", message: expect.stringMatching(/^for a tap event: .*valid JSON/) });
    expect((await w.room.webhook(t, { action: "set", spec: { ...json, body: '{"v":{{event.data.peak_g|}}}', events: ["shake"] } })).ok).toBe(true);
    expect((await w.room.webhook(t, { action: "set", spec: { ...json, events: ["wave"] } })) as any).toMatchObject({ ok: false, message: expect.stringMatching(/events must be/) });
    const ok = (await w.room.webhook(t, { action: "set", spec: { ...json, body: '{"peak":{{event.data.peak_g}},"n":{{event.data.reversals|0}},"id":"{{event.data.id|none}}"}', events: ["tap", "button"] } })) as any;
    expect(ok.ok).toBe(true);
    expect(ok.result.webhook.events).toEqual(["tap", "button"]);
    await w.emit("shake", shake); // filtered out: not queued
    expect(await w.kv.list("whq:")).toEqual(new Map());
    await w.emit("tap", { x: 0.5, y: 0.5 });
    await w.emit("button", { id: "accept" });
    await w.room.alarm();
    expect(w.sent.map((r) => JSON.parse(r.body))).toEqual([
      { peak: null, n: 0, id: "none" },
      { peak: null, n: 0, id: "accept" },
    ]);
    expect(renderTemplate("{{event.data.toString}}", SAMPLE.tap, DEVICE, ESCAPE.json)).toBe("null"); // no prototype leaks
  });

  it("MCP molty_webhook accepts an events list and rejects unknown types", async () => {
    const { backend, token } = await surfaceWorld();
    const ok = await mcpCall(backend, "molty_webhook", { url: "https://hooks.example.org/x", events: ["shake"] }, token);
    expect(ok.structuredContent).toMatchObject({ ok: true, webhook: { events: ["shake"] } });
    expect((await mcpCall(backend, "molty_webhook", { url: "https://hooks.example.org/x", events: ["wave"] }, token)).structuredContent.message).toBe("events must be a list of shake, tap, button, answer, utterance");
  });
});

// ---------------------------------------------------------------- review round 2

describe("review fixes, round 2", () => {
  /** Resolver that holds hosts starting with `slow` until released (per host). */
  function gatedResolver() {
    const gates = new Map<string, () => void>();
    const waits = new Map<string, Promise<void>>();
    const asked = new Set<string>();
    const gate = (host: string) => {
      if (!waits.has(host)) waits.set(host, new Promise<void>((r) => gates.set(host, r)));
      return waits.get(host)!;
    };
    return {
      resolve: async (host: string) => (asked.add(host), host.startsWith("slow") ? gate(host).then(() => ["93.184.216.34"]) : ["93.184.216.34"]),
      /** Resolves once every host has been asked for: those registrations have read their revision. */
      reached: async (...hosts: string[]) => {
        for (let i = 0; i < 500 && !hosts.every((h) => asked.has(h)); i++) await new Promise((r) => setTimeout(r, 1));
        if (!hosts.every((h) => asked.has(h))) throw new Error(`never reached ${hosts}`);
      },
      release: (host: string) => {
        gate(host);
        gates.get(host)!();
      },
    };
  }
  const stored = async (w: Awaited<ReturnType<typeof world>>) => [...(await w.kv.list<any>("tok:")).values()][0];

  it("P1 revision: register / register / delete / stale-complete never reinstalls (no prior webhook)", async () => {
    const g = gatedResolver();
    const w = await world({ resolve: g.resolve });
    const t = await w.pairAgent("a");
    const stale = w.room.webhook(t, { action: "set", spec: { url: "https://slow1.example.org/h" } });
    await g.reached("slow1.example.org");
    expect((await w.room.webhook(t, { action: "set", spec: { url: "https://fast.example.org/h" } })).ok).toBe(true);
    expect(await w.room.webhook(t, { action: "delete" })).toMatchObject({ ok: true, result: { removed: true } });
    g.release("slow1.example.org");
    expect(await stale).toMatchObject({ ok: false, error: "conflict" });
    expect((await stored(w)).webhook).toBeUndefined();
  });

  it("P1 revision: with a webhook already set, register / delete / stale-complete never reinstalls", async () => {
    const g = gatedResolver();
    const w = await world({ resolve: g.resolve });
    const t = await w.pairAgent("a");
    expect((await w.room.webhook(t, { action: "set", spec: { url: "https://first.example.org/h" } })).ok).toBe(true);
    const stale = w.room.webhook(t, { action: "set", spec: { url: "https://slow2.example.org/h" } });
    await g.reached("slow2.example.org");
    expect(await w.room.webhook(t, { action: "delete" })).toMatchObject({ ok: true, result: { removed: true } });
    g.release("slow2.example.org");
    expect(await stale).toMatchObject({ ok: false, error: "conflict" });
    expect((await stored(w)).webhook).toBeUndefined();
  });

  it("P1 revision: two concurrent registrations -> exactly one success, the loser gets 409 conflict, the winner's secret is the stored one", async () => {
    const g = gatedResolver();
    const w = await world({ resolve: g.resolve });
    const t = await w.pairAgent("a");
    const one = w.room.webhook(t, { action: "set", spec: { url: "https://slowa.example.org/h" } });
    const two = w.room.webhook(t, { action: "set", spec: { url: "https://slowb.example.org/h" } });
    await g.reached("slowa.example.org", "slowb.example.org"); // both have read the same revision
    g.release("slowa.example.org");
    g.release("slowb.example.org");
    const results = (await Promise.all([one, two])) as any[];
    const winners = results.filter((r) => r.ok);
    expect(winners).toHaveLength(1);
    expect(results.filter((r) => !r.ok)).toMatchObject([{ error: "conflict" }]);
    expect((await stored(w)).webhook.secret).toBe(winners[0].result.secret);
  });

  it("P1 revision: REST answers 409 to the losing registration", async () => {
    const g = gatedResolver();
    const w = await world({ resolve: g.resolve });
    const t = await w.pairAgent("a");
    const backend = { device: () => w.room };
    const put = (url: string) => rest(backend, "/v1/webhook", { method: "PUT", token: t, body: JSON.stringify({ url }) });
    const a = put("https://slowx.example.org/h");
    const b = put("https://slowy.example.org/h");
    await g.reached("slowx.example.org", "slowy.example.org");
    g.release("slowx.example.org");
    g.release("slowy.example.org");
    expect((await Promise.all([a, b])).map((r) => r.status).sort()).toEqual([200, 409]);
  });

  it("P1 same class: a revoke (or replacement) during an in-flight delivery is never undone by the stats write", async () => {
    let release: (() => void) | undefined;
    const w = await hooked({ url: "https://hooks.example.org/x" }, { fetchImpl: () => new Promise<Response>((r) => (release = () => r(new Response(null, { status: 500 })))) });
    await w.emit("tap", { x: 0, y: 0 });
    const running = w.room.alarm();
    for (let i = 0; i < 40 && !release; i++) await new Promise((r) => setTimeout(r, 1)); // until the request is in flight (inside its 50 ms scaled deadline)
    expect(release).toBeDefined();
    await w.room.revoke(w.token);
    release!();
    await running;
    expect([...(await w.kv.list("tok:")).keys()]).toHaveLength(0);
    expect(await w.room.command(w.token, "imu", {})).toMatchObject({ ok: false, error: "unauthorized" });
  });

  it("P2 canonical target: a bare '?' (or empty rendered query) is dropped before sending AND signing; documented verifiers agree on the wire target", async () => {
    for (const url of ["https://hooks.example.org/path?", "https://hooks.example.org/path?{{event.data.missing}}#frag"]) {
      const w = await hooked({ url });
      await w.emit("tap", { x: 0, y: 0 });
      await w.room.alarm();
      const req = w.sent[0];
      expect(req.url).toBe("https://hooks.example.org/path");
      const wireTarget = req.url.slice(new URL(req.url).origin.length); // exactly what the request line carries
      expect(await (await tsVerifier())(w.secret, req.headers["molty-signature"], "POST", wireTarget, req.body)).toBe(true);
      const py = pyVerify(w.secret, req.headers["molty-signature"], "POST", wireTarget, req.body);
      if (py !== "skipped") expect(py).toBe(true);
    }
  });

  it("P2 registration deadline: one 5 s budget over the whole validation; each host is resolved once", async () => {
    let calls = 0;
    const slowResolver = (_h: string, signal?: AbortSignal) =>
      new Promise<string[]>((res, rej) => {
        calls++;
        const t = setTimeout(() => res(["93.184.216.34"]), 30); // 3 s on the scaled clock
        signal?.addEventListener("abort", () => (clearTimeout(t), rej(new Error("aborted"))));
      });
    const w = await world({ resolve: slowResolver });
    const t = await w.pairAgent("a");
    // three distinct hosts x 3 s each > one 5 s budget
    const over = (await w.room.webhook(t, { action: "set", spec: { url: "https://{{event.type}}.example.org/x" } })) as any;
    expect(over).toMatchObject({ ok: false, error: "invalid_args", message: expect.stringMatching(/in time/) });
    // three rendered URLs on one host: one resolution, well within the budget
    calls = 0;
    const one = (await w.room.webhook(t, { action: "set", spec: { url: "https://hooks.example.org/{{event.type}}" } })) as any;
    expect(one.ok).toBe(true);
    expect(calls).toBe(1);
  });
});
