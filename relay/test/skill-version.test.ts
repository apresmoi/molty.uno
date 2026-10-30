// Skill versions: one source of truth, a one-time ride-along notice per agent
// per version, and the `skill.updated` inbox event for agents paired earlier.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { SKILL_EVENT } from "../src/core/events.js";
import type { DeviceRoom } from "../src/core/room.js";
import { MemoryKV } from "../src/core/store.js";
import { SKILL_VERSION, WHATS_NEW, skillHeadline } from "../src/core/version.js";
import { SERVER_INFO, handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { skillMarkdown } from "../src/surfaces/skill.js";
import { DEVICE, FakeHost, type FakeSocket, LINK, connect, flush, makeRoom } from "./helpers.js";

/** What a pre-versions (v1) agent is told: every entry. */
const ALL_NEW = WHATS_NEW.map((e) => `v${e.version}: ${e.text}`);

const FAST = { cmdTimeoutMs: 2000, pairWindowMs: 2000 };

/** Device that answers commands (unless `hold` is set) and accepts every pairing. */
function device(room: DeviceRoom, sock: FakeSocket) {
  const ctl = { hold: false, held: [] as string[] };
  const orig = sock.sendText.bind(sock);
  let n = 0;
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "cmd") {
      if (ctl.hold) ctl.held.push(f.id);
      else setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: { echo: f.name } })), 1);
    }
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: `NEXT${"ABCDEFGH"[n++ % 8]}ZZZ` })), 2);
  };
  const release = (id: string) => room.socketMessage(sock, JSON.stringify({ t: "res", id, ok: true, result: { echo: "late" } }));
  return { ctl, release };
}

async function world(kv = new MemoryKV(), host = new FakeHost()) {
  const ctx = makeRoom({ kv, host, timings: FAST, scheduler: { setAlarm: async () => {} }, resolveHost: async () => ["93.184.216.34"] });
  await ctx.room.noteOrigin("https://relay.test");
  const sock = await connect(ctx.room, ctx.host);
  const dev = device(ctx.room, sock);
  let code = LINK;
  const pairAgent = async (name: string) => {
    const out = await ctx.room.pair({ device_id: DEVICE, link_code: code, agent_name: name });
    if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
    await flush();
    code = (await ctx.kv.get<{ code: string }>("link"))?.code ?? code;
    return out.result.agent_token;
  };
  const tokKey = async () => [...(await ctx.kv.list("tok:")).keys()];
  /** Make every paired token look like one minted before skill versions existed. */
  const legacy = async () => {
    for (const [k, v] of await ctx.kv.list<Record<string, unknown>>("tok:")) {
      delete v.skill_seen;
      await ctx.kv.put(k, v);
    }
  };
  const backend: RelayBackend = { device: () => ctx.room };
  return { ...ctx, sock, dev, pairAgent, tokKey, legacy, backend };
}

const rest = async (b: RelayBackend, path: string, token: string, init: RequestInit = {}) =>
  (await handleRest(new Request(`https://relay.test${path}`, { ...init, headers: { authorization: `Bearer ${token}`, "content-type": "application/json" } }), b).then((r) => r.json())) as any;

let rid = 0;
const mcp = async (b: RelayBackend, name: string, args: unknown, token: string) =>
  ((await (
    await handleMcp(
      new Request("https://relay.test/mcp", {
        method: "POST",
        headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
        body: JSON.stringify({ jsonrpc: "2.0", id: ++rid, method: "tools/call", params: { name, arguments: args } }),
      }),
      b,
    )
  ).json()) as any).result;

describe("skill version: single source of truth", () => {
  it("skill.md, MCP instructions and serverInfo all state SKILL_VERSION and its what's-new", async () => {
    expect(WHATS_NEW.map((e) => e.version)).toEqual([...WHATS_NEW.map((e) => e.version)].sort((a, b) => a - b));
    expect(WHATS_NEW[0].version).toBeGreaterThan(1);
    expect(SKILL_VERSION).toBe(WHATS_NEW.at(-1)!.version);
    const md = skillMarkdown("https://r");
    expect(md.split("\n").slice(0, 6).join("\n")).toContain(`${skillHeadline()}.`);
    expect(skillHeadline()).toBe(`Skill v${SKILL_VERSION} — what's new: ${WHATS_NEW.at(-1)!.text}`);
    const stated = [...md.matchAll(/Skill v(\d+)/g)].map((m) => Number(m[1]));
    expect(stated.length).toBeGreaterThanOrEqual(2);
    expect(new Set(stated)).toEqual(new Set([SKILL_VERSION]));
    expect(SERVER_INFO.title).toContain(`skill v${SKILL_VERSION}`);
    const { backend } = await world();
    const init = (await (
      await handleMcp(new Request("https://relay.test/mcp", { method: "POST", body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2025-06-18" } }) }), backend)
    ).json()) as any;
    expect(init.result.instructions).toContain(skillHeadline());
    expect(init.result.serverInfo.title).toBe(SERVER_INFO.title);
  });
});

describe("skill notice (ride-along)", () => {
  it("a pre-versions token gets the notice exactly once; a freshly paired one never", async () => {
    const w = await world();
    const old = await w.pairAgent("old");
    await w.legacy();
    const fresh = await w.pairAgent("fresh");
    const first = await w.room.command(old, "imu", {});
    expect(first).toEqual({ ok: true, result: { echo: "imu" }, notice: { version: SKILL_VERSION, whats_new: ALL_NEW } });
    expect(await w.room.command(old, "imu", {})).toEqual({ ok: true, result: { echo: "imu" } });
    expect((await w.room.events(old, { wait_ms: 0 })).notice).toBeUndefined();
    expect(await w.room.command(fresh, "imu", {})).toEqual({ ok: true, result: { echo: "imu" } });
    const recs = [...(await w.kv.list<any>("tok:")).values()];
    expect(recs.map((r) => r.skill_seen)).toEqual([SKILL_VERSION, SKILL_VERSION]);
  });

  it("v4 bump: an agent that saw v3 is told only what is new in v4 (molty_ask), once; one that saw v2 hears v3 and v4", async () => {
    expect(SKILL_VERSION).toBe(4);
    const w = await world();
    const a = await w.pairAgent("v3 agent");
    for (const [k, v] of await w.kv.list<Record<string, unknown>>("tok:")) await w.kv.put(k, { ...v, skill_seen: 3 });
    const first = await w.room.command(a, "imu", {});
    expect(first.notice).toEqual({ version: 4, whats_new: [`v4: ${WHATS_NEW.at(-1)!.text}`] });
    expect(first.notice!.whats_new[0]).toMatch(/molty_ask/);
    expect((await w.room.command(a, "imu", {})).notice).toBeUndefined();
    for (const [k, v] of await w.kv.list<Record<string, unknown>>("tok:")) await w.kv.put(k, { ...v, skill_seen: 2 });
    const older = await w.room.command(a, "imu", {});
    expect(older.notice!.whats_new.map((l) => l.slice(0, 3))).toEqual(["v3:", "v4:"]);
  });

  it("failed outcomes do not consume it; the next success does", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    await w.legacy();
    expect(await w.room.command(a, "nope", {})).toEqual({ ok: false, error: "unknown_command", message: "unknown command nope" });
    expect(await w.room.job(a, `${DEVICE}.j_abcdefghijkl`, 0)).toEqual({ ok: false, error: "not_found" });
    expect(await w.room.events(a, { since: -1, wait_ms: 0 })).toMatchObject({ ok: false });
    expect(await w.room.webhook(a, { action: "set", spec: { url: "http://x" } })).toMatchObject({ ok: false });
    expect([...(await w.kv.list<any>("tok:")).values()][0].skill_seen).toBeUndefined();
    expect((await w.room.events(a, { wait_ms: 0 })).notice?.version).toBe(SKILL_VERSION);
    expect((await w.room.command(a, "imu", {})).notice).toBeUndefined();
  });

  it("concurrent calls: exactly one carries it", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    await w.legacy();
    // Hold every reply, then release them in one tick so the ride-along steps interleave.
    w.dev.ctl.hold = true;
    const pending = Array.from({ length: 6 }, () => w.room.command(a, "imu", {}));
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.ctl.held).toHaveLength(6);
    await Promise.all(w.dev.ctl.held.map((id) => w.dev.release(id)));
    const outs = await Promise.all(pending);
    expect(outs.every((o) => o.ok)).toBe(true);
    expect(outs.filter((o) => o.notice)).toHaveLength(1);
  });

  it("never resurrects a token revoked while the call was in flight", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    await w.legacy();
    w.dev.ctl.hold = true;
    const pending = w.room.command(a, "imu", {});
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.ctl.held).toHaveLength(1);
    expect(await w.room.revoke(a)).toMatchObject({ ok: true });
    await w.dev.release(w.dev.ctl.held[0]);
    const out = await pending;
    expect(out).toMatchObject({ ok: true });
    expect(out.notice).toBeUndefined();
    expect(await w.tokKey()).toEqual([]);
    expect(await w.room.command(a, "imu", {})).toMatchObject({ ok: false, error: "unauthorized" });
  });
});

describe("skill notice on the surfaces", () => {
  it("REST: additive top-level skill_update, once", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    await w.legacy();
    const cmd = (t: string) => rest(w.backend, `/v1/devices/${DEVICE}/cmd`, t, { method: "POST", body: JSON.stringify({ name: "imu" }) });
    expect(await cmd(a)).toEqual({
      ok: true,
      result: { echo: "imu" },
      skill_update: { version: SKILL_VERSION, whats_new: ALL_NEW, url: "https://relay.test/skill.md" },
    });
    expect(await cmd(a)).toEqual({ ok: true, result: { echo: "imu" } });
  });

  it("REST: the events pull and job routes carry it too; an error response does not", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    await w.legacy();
    expect(await rest(w.backend, `/v1/devices/${DEVICE}/cmd`, a, { method: "POST", body: JSON.stringify({ name: "nope" }) })).not.toHaveProperty("skill_update");
    const ev = await rest(w.backend, `/v1/devices/${DEVICE}/events`, a);
    expect(ev).toMatchObject({ ok: true, skill_update: { version: SKILL_VERSION, url: "https://relay.test/skill.md" } });
    expect(await rest(w.backend, `/v1/devices/${DEVICE}/events`, a)).not.toHaveProperty("skill_update");
  });

  it("MCP: structuredContent.skill_update + one text line, once", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    await w.legacy();
    const r = await mcp(w.backend, "molty_imu", {}, a);
    expect(r.isError).toBe(false);
    expect(r.structuredContent).toMatchObject({ ok: true, echo: "imu", skill_update: { version: SKILL_VERSION, whats_new: ALL_NEW, url: "https://relay.test/skill.md" } });
    const lines = r.content.map((c: any) => c.text);
    expect(lines.at(-1)).toBe(`Molty's skill was updated (${ALL_NEW.join("; ")}). Re-read https://relay.test/skill.md.`);
    expect(lines.at(-1)).toMatch(/^Molty's skill was updated \(v2: device events/);
    const again = await mcp(w.backend, "molty_imu", {}, a);
    expect(again.structuredContent).not.toHaveProperty("skill_update");
    expect(again.content).toHaveLength(1);
  });

  it("MCP: molty_wait reshapes the job outcome but keeps the notice", async () => {
    const w = await world();
    w.voice.ttsSamples = 1600; // 100 ms of speech
    const a = await w.pairAgent("a");
    const job = await w.room.command(a, "say", { text: "hi" });
    expect(job.ok && job.notice).toBeFalsy();
    await w.legacy();
    const r = await mcp(w.backend, "molty_wait", { job: (job as any).result.job, timeout_s: 3 }, a);
    expect(r.structuredContent).toMatchObject({ ok: true, status: "done", skill_update: { version: SKILL_VERSION } });
    expect(r.content.at(-1).text).toMatch(/^Molty's skill was updated/);
  });

  it("MCP: an error result does not consume it", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    await w.legacy();
    const bad = await mcp(w.backend, "molty_wait", { job: `${DEVICE}.j_abcdefghijkl` }, a);
    expect(bad.isError).toBe(true);
    expect(bad.structuredContent).not.toHaveProperty("skill_update");
    expect((await mcp(w.backend, "molty_imu", {}, a)).structuredContent).toHaveProperty("skill_update");
  });
});

describe("skill.updated inbox event", () => {
  it("after a deploy, one event reaches older agents' inbox and unfiltered webhooks; current agents' hooks are skipped", async () => {
    const kv = new MemoryKV();
    const host = new FakeHost();
    const w = await world(kv, host);
    const old = await w.pairAgent("old");
    const filtered = await w.pairAgent("filtered");
    await w.room.webhook(old, { action: "set", spec: { url: "https://hooks.example.org/old" } });
    await w.room.webhook(filtered, { action: "set", spec: { url: "https://hooks.example.org/f", events: ["shake"] } });
    await w.legacy();
    const current = await w.pairAgent("current");
    await w.room.webhook(current, { action: "set", spec: { url: "https://hooks.example.org/cur" } });
    // Simulate the deploy: state from before skill versions, fresh room, device reconnects.
    await kv.delete("skillev");
    const after = makeRoom({ kv, host, timings: FAST, scheduler: { setAlarm: async () => {} } });
    await connect(after.room, host, undefined, null);
    const evs = [...(await kv.list<any>("ev:")).values()];
    expect(evs).toEqual([
      { id: "e1", type: SKILL_EVENT, ts: expect.any(Number), data: { version: SKILL_VERSION, whats_new: ALL_NEW, url: "https://relay.test/skill.md" } },
    ]);
    const queued = [...(await kv.list<any>("whq:")).values()];
    expect(queued).toHaveLength(1);
    // Exactly the old agent's (unfiltered, older) webhook.
    const oldHash = [...(await kv.list<any>("tok:")).entries()].find(([, r]) => r.agent_name === "old")![0].slice(4);
    expect(queued[0].tok).toBe(oldHash);
    // Once per version: another reconnect or room does not append again.
    await connect(after.room, host, undefined, null);
    await connect(makeRoom({ kv, host, timings: FAST }).room, host, undefined, null);
    expect([...(await kv.list("ev:")).keys()]).toHaveLength(1);
    // Older agents read it once like any event (pull or ride-along).
    expect(((await after.room.events(old, { wait_ms: 0 })) as any).result.events.map((e: any) => e.type)).toEqual([SKILL_EVENT]);
    expect(((await after.room.events(old, { wait_ms: 0 })) as any).result.events).toEqual([]);
  });

  it("no older agent -> no event (fresh installs never see it)", async () => {
    const w = await world();
    const a = await w.pairAgent("a");
    await w.room.command(a, "imu", {});
    expect([...(await w.kv.list("ev:")).keys()]).toEqual([]);
    expect(await w.kv.get("skillev")).toBe(SKILL_VERSION);
  });
});
