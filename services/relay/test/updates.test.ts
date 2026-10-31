// "Check Molty for updates": molty_updates (api_version + firmware + actions), the
// firmware_too_old gate on molty_ask, molty_update as a storage-driven job settled
// by the device's next hello, and the one-time firmware_update ride-along notice.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import type { ReleaseInfo, ReleaseLookup, ReleaseSource } from "../src/core/releases.js";
import type { DeviceRoom } from "../src/core/room.js";
import { type Clock, MemoryKV } from "../src/core/store.js";
import { API_VERSION, FEATURES } from "../src/core/version.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, FakeHost, type FakeSocket, KEY, LINK, connect, flush, makeRoom } from "./helpers.js";

const B146 = "waveshare-esp32-s3-touch-lcd-1.46";
const SHA = "d".repeat(64);
const URL146 = `https://github.com/apresmoi/molty.uno/releases/download/v0.2.0/molty-0.2.0-${B146}-ota.bin`;

function release(version = "0.2.0", opts: { sha?: string | null; boards?: string[] } = {}): ReleaseInfo {
  const boards: ReleaseInfo["boards"] = {};
  for (const b of opts.boards ?? [B146]) {
    boards[b] = { name: `molty-${version}-${b}-ota.bin`, url: `https://github.com/apresmoi/molty.uno/releases/download/v${version}/molty-${version}-${b}-ota.bin`, size: 1234567, sha256: opts.sha === undefined ? SHA : opts.sha };
  }
  return { version, tag: `v${version}`, prerelease: false, published_at: "2026-11-01T10:00:00Z", notes_url: `https://github.com/apresmoi/molty.uno/releases/tag/v${version}`, notes: "notes", boards };
}

/** A settable release source. */
class Releases implements ReleaseSource {
  lookup: ReleaseLookup = { release: null, state: "fresh", checked_at: 1 };
  set(release: ReleaseInfo | null, state: ReleaseLookup["state"] = "fresh") {
    this.lookup = { release, state, checked_at: state === "unavailable" ? null : 1 };
  }
  async latest() {
    return structuredClone(this.lookup);
  }
}

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, askGraceMs: 50, updateConfirmMs: 200, updateMs: 300 };

/** A device that answers every cmd ok (or `refuse` for update) and records them. */
function device(room: DeviceRoom, sock: FakeSocket) {
  const cmds: Array<{ name: string; args: any }> = [];
  const ctl = { refuse: undefined as string | undefined, noAck: false };
  const orig = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args });
    const err = f.name === "update" ? ctl.refuse : undefined;
    if (f.name === "update" && ctl.noAck) return;
    setTimeout(() => void room.socketMessage(sock, JSON.stringify(err ? { t: "res", id: f.id, ok: false, error: err } : { t: "res", id: f.id, ok: true, result: { echo: f.name } })), 1);
  };
  const evt = (data: Record<string, unknown>) => room.socketMessage(sock, JSON.stringify({ t: "evt", name: "update", data }));
  return { cmds, ctl, evt, updates: () => cmds.filter((c) => c.name === "update") };
}

async function setup(opts: { fw?: string; caps?: string[]; board?: string | null; release?: ReleaseInfo | null; state?: ReleaseLookup["state"]; kv?: MemoryKV; timings?: Record<string, number>; clock?: Clock } = {}) {
  const releases = new Releases();
  releases.set(opts.release === undefined ? release() : opts.release, opts.state);
  const kv = opts.kv ?? new MemoryKV();
  const host = new FakeHost();
  const ctx = makeRoom({ kv, host, releases, timings: { ...FAST, ...opts.timings }, ...(opts.clock ? { clock: opts.clock } : {}) });
  const hello = { fw: opts.fw ?? "0.1.0", caps: opts.caps ?? ["say", "listen"], ...(opts.board === null ? {} : { board: opts.board ?? B146 }) };
  const sock = await connect(ctx.room, host, KEY, LINK, hello);
  const dev = device(ctx.room, sock);
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "checker" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  const backend: RelayBackend = { device: () => ctx.room };
  /** The device restarts (or reconnects) and says hello with `fw`. */
  const reconnect = async (fw: string, caps = hello.caps) => {
    sock.close(1000, "restart");
    const s2 = await connect(ctx.room, host, KEY, null, { ...hello, fw, caps });
    device(ctx.room, s2);
    return s2;
  };
  const tokRec = async () => [...(await kv.list<any>("tok:")).values()][0];
  return { ...ctx, releases, sock, dev, token: out.result.agent_token, backend, reconnect, tokRec, hello };
}

const until = async (cond: () => boolean, ms = 2000) => {
  const end = Date.now() + ms;
  while (!cond() && Date.now() < end) await new Promise((r) => setTimeout(r, 3));
  if (!cond()) throw new Error("condition never met");
};
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

let rid = 0;
const mcp = async (b: RelayBackend, token: string, name: string, args: unknown = {}) =>
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

const rest = async (b: RelayBackend, token: string, path: string, init: RequestInit = {}) => {
  const r = await handleRest(new Request(`https://relay.test${path}`, { ...init, headers: { authorization: `Bearer ${token}`, "content-type": "application/json" } }), b);
  return { status: r.status, body: (await r.json()) as any };
};

describe("molty_updates", () => {
  it("firmware behind (no self-update): api_version, the update, needed_for, plain actions; nothing about a skill", async () => {
    const w = await setup();
    // A token minted while the relay still tracked skill versions: the legacy field changes nothing.
    await w.kv.put([...(await w.kv.list("tok:")).keys()][0], { ...(await w.tokRec()), skill_seen: 3 });
    const out = await w.room.updates(w.token);
    expect(out.ok).toBe(true);
    const r = (out as any).result;
    expect(Object.keys(r).sort()).toEqual(["actions", "api_version", "firmware"]);
    expect(r.api_version).toBe(API_VERSION);
    expect(r.firmware).toEqual({
      current: "0.1.0",
      board: B146,
      online: true,
      latest: "0.2.0",
      status: "update_available",
      update_available: true,
      published_at: "2026-11-01T10:00:00Z",
      notes_url: "https://github.com/apresmoi/molty.uno/releases/tag/v0.2.0",
      needed_for: [
        { feature: "molty_ask", what: FEATURES.ask.what, needs: "0.2.0", released: true },
        { feature: "molty_update", what: FEATURES.update.what, needs: "0.2.0", released: true },
        { feature: "molty_act", what: FEATURES.act.what, needs: "0.2.0", released: true },
      ],
      can_self_update: false,
      release_check: "fresh",
    });
    expect(r.actions[0]).toMatch(/0\.2\.0 is out .*can't update itself.*console/);
    expect(r.actions.join("\n")).not.toMatch(/skill|re-read/i);
    expect(r.actions.join("\n")).toMatch(/molty_ask .* needs firmware 0\.2\.0: mention that updating unlocks it/);
    // seen: no duplicate firmware notice on this result, and nothing skill-shaped rides along
    expect((out as any).fw_notice).toBeUndefined();
    expect(out).not.toHaveProperty("notice");
    expect(await w.tokRec()).toMatchObject({ skill_seen: 3, fw_seen: "0.2.0" });
  });

  it("firmware current / no release / unknown version / release list unreachable / self-update capable", async () => {
    const cur = await setup({ fw: "0.2.0", caps: ["say", "listen", "ask", "update"] });
    const c = ((await cur.room.updates(cur.token)) as any).result;
    expect(c.firmware).toMatchObject({ status: "current", update_available: false, needed_for: [], can_self_update: true });
    expect(c.actions[0]).toMatch(/0\.2\.0 is the latest/);

    const none = await setup({ release: null });
    const n = ((await none.room.updates(none.token)) as any).result;
    expect(n.firmware).toMatchObject({ status: "no_release", latest: null, update_available: false });
    expect(n.firmware.needed_for.map((x: any) => [x.feature, x.released])).toEqual([["molty_ask", false], ["molty_update", false], ["molty_act", false]]);
    expect(n.actions.join("\n")).toMatch(/No firmware release is published yet/);
    expect(n.actions.join("\n")).toMatch(/molty_ask .* not released yet: don't offer it/);

    const dev = await setup({ fw: "test-fw" });
    const d = ((await dev.room.updates(dev.token)) as any).result;
    expect(d.firmware).toMatchObject({ status: "unknown", current: "test-fw", needed_for: [], update_available: false });
    expect(d.actions[0]).toMatch(/development build/);

    const down = await setup({ release: null, state: "unavailable" });
    const u = ((await down.room.updates(down.token)) as any).result;
    expect(u.firmware).toMatchObject({ status: "unknown", release_check: "unavailable" });
    expect(u.actions[0]).toMatch(/Couldn't reach the release list/);

    const self = await setup({ caps: ["say", "listen", "update"] });
    const s = ((await self.room.updates(self.token)) as any).result;
    expect(s.firmware.can_self_update).toBe(true);
    expect(s.actions[0]).toMatch(/Ask your human: .*Update now\?.*molty_update/);

    const other = await setup({ release: release("0.2.0", { boards: ["some-other-board"] }) });
    expect(((await other.room.updates(other.token)) as any).result.firmware).toMatchObject({ status: "no_build_for_board", update_available: false });
  });

  it("answers while Molty is offline from its last hello", async () => {
    const w = await setup();
    w.sock.close(1000, "bye");
    const r = ((await w.room.updates(w.token)) as any).result;
    expect(r.firmware).toMatchObject({ current: "0.1.0", board: B146, online: false, status: "update_available" });
    expect(r.actions.at(-1)).toMatch(/offline/);
  });

  it("REST GET /updates and MCP molty_updates carry the same report; status carries the firmware block", async () => {
    const w = await setup();
    const r = await rest(w.backend, w.token, `/v1/devices/${DEVICE}/updates`);
    expect(r.status).toBe(200);
    expect(r.body).toMatchObject({ ok: true, result: { api_version: API_VERSION, firmware: { status: "update_available" }, actions: expect.any(Array) } });
    expect((await rest(w.backend, w.token, `/v1/devices/${DEVICE}/updates`, { method: "POST" })).status).toBe(405);
    expect((await rest(w.backend, "nope", `/v1/devices/${DEVICE}/updates`)).status).toBe(401);
    const m = await mcp(w.backend, w.token, "molty_updates");
    expect(m.isError).toBe(false);
    expect(m.structuredContent.firmware.latest).toBe("0.2.0");
    const st = await w.room.command(w.token, "status", {});
    expect((st as any).result.relay.firmware).toMatchObject({ current: "0.1.0", latest: "0.2.0", status: "update_available" });
  });
});

describe("firmware_too_old", () => {
  it("molty_ask on firmware older than its min_fw: firmware_too_old {feature, current, needed, latest, how}", async () => {
    const w = await setup({ fw: "0.1.0", caps: ["say", "listen"] });
    const out = await w.room.ask(w.token, { options: ["ok"] });
    expect(out).toMatchObject({ ok: false, error: "firmware_too_old", details: { feature: "molty_ask", current: "0.1.0", needed: "0.2.0", latest: "0.2.0" } });
    expect((out as any).details.how).toMatch(/0\.2\.0 is out.*console/);
    expect(w.dev.cmds).toEqual([]);
    const r = await rest(w.backend, w.token, `/v1/devices/${DEVICE}/ask`, { method: "POST", body: JSON.stringify({ options: ["ok"] }) });
    expect(r.status).toBe(409);
    expect(r.body).toMatchObject({ ok: false, error: "firmware_too_old", feature: "molty_ask", current: "0.1.0", needed: "0.2.0", latest: "0.2.0", how: expect.any(String) });
    const m = await mcp(w.backend, w.token, "molty_ask", { options: ["ok"] });
    expect(m.isError).toBe(true);
    expect(m.structuredContent).toMatchObject({ error: "firmware_too_old", needed: "0.2.0" });
  });

  it("not released yet: says so; firmware new enough without the cap (no touch) or an unknown version: unsupported", async () => {
    const early = await setup({ release: null });
    const e = await early.room.ask(early.token, { options: ["ok"] });
    expect(e).toMatchObject({ error: "firmware_too_old", details: { latest: null, how: expect.stringMatching(/not released yet/) } });
    const noTouch = await setup({ fw: "0.2.0", caps: ["say", "listen"] });
    expect(await noTouch.room.ask(noTouch.token, { options: ["ok"] })).toMatchObject({ ok: false, error: "unsupported", message: expect.stringMatching(/touch/) });
    const dev = await setup({ fw: "test-fw", caps: ["say", "listen"] });
    expect(await dev.room.ask(dev.token, { options: ["ok"] })).toMatchObject({ ok: false, error: "unsupported" });
    const ok = await setup({ fw: "0.2.0", caps: ["say", "listen", "ask"] });
    const p = ok.room.ask(ok.token, { options: ["ok"], timeout_s: 1 });
    await until(() => ok.dev.cmds.some((c) => c.name === "ask"));
    await ok.room.socketMessage(ok.sock, JSON.stringify({ t: "evt", name: "answer", data: { job: ok.dev.cmds[0].args.job, index: 0, label: "ok" } }));
    expect(await p).toMatchObject({ ok: true, result: { answer: "ok" } });
  });
});

describe("molty_update", () => {
  const CAPS = ["say", "listen", "update"];

  it("without the update cap: unsupported, with how pointing at the console", async () => {
    const w = await setup();
    const out = await w.room.update(w.token, {});
    expect(out).toMatchObject({ ok: false, error: "unsupported", details: { current: "0.1.0", latest: "0.2.0", how: expect.stringMatching(/console/) } });
    expect(w.dev.updates()).toEqual([]);
    const r = await rest(w.backend, w.token, `/v1/devices/${DEVICE}/update`, { method: "POST", body: "{}" });
    expect(r).toMatchObject({ status: 409, body: { error: "unsupported", how: expect.any(String) } });
  });

  it("sends the official release asset (url, sha256, size) for this board; never anything the agent supplies", async () => {
    const w = await setup({ caps: CAPS });
    expect(await w.room.update(w.token, { url: "https://evil.example/x.bin" })).toMatchObject({ ok: false, error: "invalid_args" });
    expect((await mcp(w.backend, w.token, "molty_update", { url: "https://evil.example/x.bin" })).structuredContent.error).toBe("invalid_args");
    const p = w.room.update(w.token, {});
    await until(() => w.dev.updates().length === 1);
    const args = w.dev.updates()[0].args;
    expect(args).toEqual({ job: expect.stringMatching(/\.j_/), version: "0.2.0", url: URL146, sha256: SHA, size: 1234567, timeout_ms: FAST.updateConfirmMs });
    await w.dev.evt({ job: args.job, state: "declined" });
    expect(await p).toMatchObject({ ok: true, result: { job: args.job, status: "done", outcome: "declined", current: "0.1.0", latest: "0.2.0" } });
  });

  it("accepted -> downloading -> rebooting -> hello on the new firmware: done {outcome: updated}", async () => {
    const w = await setup({ caps: CAPS, timings: { talkBudgetMs: 100 } });
    const first = await w.room.update(w.token, {});
    expect(first, JSON.stringify(first)).toMatchObject({ ok: true, result: { status: "pending", phase: "confirm", hint: expect.stringMatching(/screen/) } });
    const job = (first as any).result.job;
    await w.dev.evt({ job, state: "accepted" });
    await w.dev.evt({ job, state: "downloading" });
    const mid = await mcp(w.backend, w.token, "molty_wait", { job, timeout_s: 0 });
    expect(mid.structuredContent).toMatchObject({ status: "pending", phase: "downloading" });
    // a second molty_update while one runs returns the same job
    expect(await w.room.update(w.token, {})).toMatchObject({ ok: true, result: { job, status: "pending", phase: "downloading" } });
    await w.dev.evt({ job, state: "rebooting" });
    await w.reconnect("0.2.0", [...CAPS, "ask"]);
    const done = await mcp(w.backend, w.token, "molty_wait", { job, timeout_s: 1 });
    expect(done.structuredContent).toMatchObject({ ok: true, status: "done", outcome: "updated", from: "0.1.0", to: "0.2.0" });
    expect(await w.kv.get("upd")).toBeUndefined();
    expect(((await w.room.updates(w.token)) as any).result.firmware).toMatchObject({ current: "0.2.0", status: "current" });
  });

  it("settles from storage: a fresh room (host evicted during the reboot) still reports done on the new hello", async () => {
    const kv = new MemoryKV();
    const w = await setup({ caps: CAPS, kv, timings: { talkBudgetMs: 50 } });
    const job = ((await w.room.update(w.token, {})) as any).result.job;
    await w.dev.evt({ job, state: "accepted" });
    await w.dev.evt({ job, state: "rebooting" });
    const host2 = new FakeHost();
    const fresh = makeRoom({ kv, host: host2, releases: w.releases, timings: { ...FAST } });
    expect(await fresh.room.job(w.token, job, 0)).toMatchObject({ ok: true, result: { status: "running", phase: "rebooting" } }); // not "interrupted"
    await connect(fresh.room, host2, KEY, null, { ...w.hello, fw: "0.2.0" });
    expect(await fresh.room.job(w.token, job, 0)).toMatchObject({ ok: true, result: { status: "done", result: { outcome: "updated" } } });
  });

  it("back on the old firmware after rebooting: rolled_back; a reconnect mid-download is not a failure", async () => {
    const w = await setup({ caps: CAPS, timings: { talkBudgetMs: 50, updateMs: 2000 } });
    const job = ((await w.room.update(w.token, {})) as any).result.job;
    await w.dev.evt({ job, state: "accepted" });
    await w.dev.evt({ job, state: "downloading" });
    await w.reconnect("0.1.0"); // Wi-Fi blip: the download goes on
    expect(await w.room.job(w.token, job, 0)).toMatchObject({ ok: true, result: { status: "running", phase: "downloading" } });
    await w.room.socketMessage(w.host.sockets()[0], JSON.stringify({ t: "evt", name: "update", data: { job, state: "rebooting" } }));
    await w.reconnect("0.1.0");
    const r = await rest(w.backend, w.token, `/v1/jobs/${job}`);
    expect(r.body).toMatchObject({ status: "failed", error: "rolled_back", detail: expect.stringMatching(/came back on 0\.1\.0/) });
    const m = await mcp(w.backend, w.token, "molty_wait", { job, timeout_s: 0 });
    expect(m.structuredContent).toMatchObject({ ok: false, error: "rolled_back", message: expect.stringMatching(/did not stick/) });
  });

  it("device-reported failure: update_failed with its reason; device refuses the cmd: that error", async () => {
    const w = await setup({ caps: CAPS });
    const p = w.room.update(w.token, {});
    await until(() => w.dev.updates().length === 1);
    const job = w.dev.updates()[0].args.job;
    await w.dev.evt({ job, state: "accepted" });
    await w.dev.evt({ job: "molty-k7m2q9xa4c.j_someoneelse0", state: "failed", error: "x" }); // not this job: ignored
    await w.dev.evt({ job, state: "failed", error: "sha256 mismatch\u0000" });
    expect(await p).toEqual({ ok: false, error: "update_failed", message: "Molty could not install 0.2.0: sha256 mismatch; it still runs 0.1.0", details: { job } });
    const r = await setup({ caps: CAPS });
    r.dev.ctl.refuse = "update_busy";
    expect(await r.room.update(r.token, {})).toMatchObject({ ok: false, error: "update_busy" });
    expect(await r.kv.get("upd")).toBeUndefined();
  });

  it("an update the device accepted survives a lost ack: still tracked, settled by the next hello", async () => {
    const w = await setup({ caps: CAPS, timings: { talkBudgetMs: 50, updateMs: 2000 } });
    w.dev.ctl.noAck = true;
    const p = w.room.update(w.token, {});
    await until(() => w.dev.updates().length === 1);
    const job = w.dev.updates()[0].args.job;
    await w.dev.evt({ job, state: "accepted" });
    await sleep(FAST.cmdTimeoutMs + 50); // the ack timed out
    expect(await p).toMatchObject({ ok: true, result: { job, status: "pending", phase: "accepted" } });
    expect(await w.room.job(w.token, job, 0)).toMatchObject({ ok: true, result: { status: "running", phase: "accepted" } });
    expect(await w.kv.get("upd")).toMatchObject({ job, phase: "accepted" });
    await w.dev.evt({ job, state: "rebooting" });
    await w.reconnect("0.2.0");
    expect(await w.room.job(w.token, job, 0)).toMatchObject({ ok: true, result: { status: "done", result: { outcome: "updated" } } });
  });

  it("a lost ack is delivery unknown: tracked through the confirmation deadline, a later accept still settles it", async () => {
    // No device event before the ack timed out: the question may be on screen, the human can still tap it.
    const n = await setup({ caps: CAPS, timings: { talkBudgetMs: 50, updateConfirmMs: 1500, updateMs: 2000 } });
    n.dev.ctl.noAck = true;
    const p = n.room.update(n.token, {});
    await until(() => n.dev.updates().length === 1);
    const job = n.dev.updates()[0].args.job;
    expect(await p).toMatchObject({ ok: true, result: { job, status: "pending", phase: "confirm" } });
    await sleep(FAST.cmdTimeoutMs + 50); // the ack timed out
    expect(await n.room.job(n.token, job, 0)).toMatchObject({ ok: true, result: { status: "running", phase: "confirm" } });
    expect(await n.kv.get("upd")).toMatchObject({ job, phase: "confirm", acked: false });
    await n.dev.evt({ job, state: "accepted" }); // the human taps Update after the ack was lost
    expect(await n.room.job(n.token, job, 0)).toMatchObject({ ok: true, result: { status: "running", phase: "accepted" } });
    await n.dev.evt({ job, state: "rebooting" });
    await n.reconnect("0.2.0");
    expect(await n.room.job(n.token, job, 0)).toMatchObject({ ok: true, result: { status: "done", result: { outcome: "updated", from: "0.1.0", to: "0.2.0" } } });
    expect(await n.kv.get("upd")).toBeUndefined();
    // ... a reconnect on the new firmware (accepted event lost too) settles it as well
    const r = await setup({ caps: CAPS, timings: { talkBudgetMs: 50, updateConfirmMs: 1500 } });
    r.dev.ctl.noAck = true;
    const rj = ((await r.room.update(r.token, {})) as any).result.job;
    await sleep(FAST.cmdTimeoutMs + 50);
    await r.reconnect("0.2.0");
    expect(await r.room.job(r.token, rj, 0)).toMatchObject({ ok: true, result: { status: "done", result: { outcome: "updated" } } });
    // ... and nobody answering ends it at the confirmation deadline, not at the ack timeout
    const q = await setup({ caps: CAPS, timings: { updateConfirmMs: 700 } });
    q.dev.ctl.noAck = true;
    const t0 = Date.now();
    expect(await q.room.update(q.token, {})).toMatchObject({ ok: false, error: "update_timeout", message: expect.stringMatching(/never confirmed/) });
    expect(Date.now() - t0).toBeGreaterThanOrEqual(700);
    expect(await q.kv.get("upd")).toBeUndefined();
  });

  it("a deadline timer that fires early re-arms: the update still times out (it never hangs until the call budget)", async () => {
    // Timers fire up to 40 ms before the clock reaches the deadline, as Node's sometimes do on a busy runner.
    const early: Clock = { now: () => Date.now(), setTimeout: (fn, ms) => setTimeout(fn, Math.max(0, ms - 40)), clearTimeout: (h) => clearTimeout(h as ReturnType<typeof setTimeout>) };
    const w = await setup({ caps: CAPS, clock: early, timings: { talkBudgetMs: 3000 } });
    const t0 = Date.now();
    const out = await w.room.update(w.token, {});
    expect(out).toMatchObject({ ok: false, error: "update_timeout" });
    expect(Date.now() - t0).toBeLessThan(2000);
  });

  it("nobody answers the on-screen question, or the device never comes back: update_timeout", async () => {
    const w = await setup({ caps: CAPS });
    const out = await w.room.update(w.token, {});
    expect(out).toMatchObject({ ok: false, error: "update_timeout", message: expect.stringMatching(/nobody answered/) });
    const v = await setup({ caps: CAPS, timings: { talkBudgetMs: 50 } });
    const job = ((await v.room.update(v.token, {})) as any).result.job;
    await v.dev.evt({ job, state: "accepted" });
    await sleep(FAST.updateMs + 50);
    expect(await v.room.job(v.token, job, 0)).toMatchObject({ ok: true, result: { status: "failed", error: "update_timeout", detail: expect.stringMatching(/did not come back/) } });
  });

  it("up to date, no release, dev build, no image for the board, no checksum, release list down, offline", async () => {
    const cur = await setup({ fw: "0.2.0", caps: CAPS });
    expect(await cur.room.update(cur.token, {})).toEqual({ ok: true, result: { status: "up_to_date", current: "0.2.0", latest: "0.2.0" } });
    const none = await setup({ caps: CAPS, release: null });
    expect(await none.room.update(none.token, {})).toEqual({ ok: true, result: { status: "up_to_date", current: "0.1.0", latest: null } });
    const dev = await setup({ fw: "dev", caps: CAPS });
    expect(await dev.room.update(dev.token, {})).toMatchObject({ ok: false, error: "unknown_version" });
    const other = await setup({ caps: CAPS, release: release("0.2.0", { boards: ["other-board"] }) });
    expect(await other.room.update(other.token, {})).toMatchObject({ ok: false, error: "no_build" });
    const unsigned = await setup({ caps: CAPS, release: release("0.2.0", { sha: null }) });
    expect(await unsigned.room.update(unsigned.token, {})).toMatchObject({ ok: false, error: "no_checksum" });
    const down = await setup({ caps: CAPS, release: null, state: "unavailable" });
    const r = await rest(down.backend, down.token, `/v1/devices/${DEVICE}/update`, { method: "POST", body: "{}" });
    expect(r).toMatchObject({ status: 503, body: { error: "release_unavailable" } });
    for (const w of [cur, none, dev, other, unsigned, down]) expect(w.dev.updates()).toEqual([]);
    cur.sock.close(1000, "x");
    expect(await cur.room.update(cur.token, {})).toMatchObject({ ok: false, error: "device_offline" });
  });

  it("REST POST /update: 202 pending with phase, then GET /v1/jobs reports done", async () => {
    const w = await setup({ caps: CAPS, timings: { talkBudgetMs: 50 } });
    const r = await rest(w.backend, w.token, `/v1/devices/${DEVICE}/update`, { method: "POST", body: "{}" });
    expect(r).toMatchObject({ status: 202, body: { ok: true, result: { status: "pending", phase: "confirm" } } });
    const job = r.body.result.job;
    expect((await rest(w.backend, w.token, `/v1/jobs/${job}`)).body).toMatchObject({ kind: "update", status: "running", phase: "confirm" });
    await w.dev.evt({ job, state: "accepted" });
    await w.reconnect("0.2.0");
    expect((await rest(w.backend, w.token, `/v1/jobs/${job}`)).body).toMatchObject({ status: "done", result: { outcome: "updated" } });
    expect((await rest(w.backend, w.token, `/v1/devices/${DEVICE}/update`)).status).toBe(405);
  });
});

describe("firmware_update notice (passive)", () => {
  it("once per token per release, on the next success only; a newer release notifies again", async () => {
    const w = await setup();
    expect(await w.room.command(w.token, "nope", {})).toEqual({ ok: false, error: "unknown_command", message: "unknown command nope" });
    expect((await w.tokRec()).fw_seen).toBeUndefined();
    const first = await w.room.command(w.token, "imu", {});
    expect(first.fw_notice).toEqual({ current: "0.1.0", latest: "0.2.0", board: B146, notes_url: "https://github.com/apresmoi/molty.uno/releases/tag/v0.2.0", can_self_update: false, how: expect.stringMatching(/console/) });
    expect((await w.room.command(w.token, "imu", {})).fw_notice).toBeUndefined();
    expect((await w.room.events(w.token, { wait_ms: 0 })).fw_notice).toBeUndefined();
    w.releases.set(release("0.3.0"));
    expect((await w.room.events(w.token, { wait_ms: 0 })).fw_notice).toMatchObject({ latest: "0.3.0" });
    expect((await w.room.command(w.token, "imu", {})).fw_notice).toBeUndefined();
  });

  it("none when current, without a release, for a dev build, or without an image for the board", async () => {
    for (const o of [{ fw: "0.2.0" }, { release: null }, { fw: "test-fw" }, { board: null }, { release: release("0.2.0", { boards: ["x-board"] }) }]) {
      const w = await setup(o);
      expect((await w.room.command(w.token, "imu", {})).fw_notice, JSON.stringify(o)).toBeUndefined();
    }
  });

  it("REST renders firmware_update; MCP adds it to the result plus one line", async () => {
    const a = await setup();
    const r = await rest(a.backend, a.token, `/v1/devices/${DEVICE}/cmd`, { method: "POST", body: JSON.stringify({ name: "imu" }) });
    expect(r.body.firmware_update).toMatchObject({ current: "0.1.0", latest: "0.2.0" });
    const b = await setup();
    const m = await mcp(b.backend, b.token, "molty_imu");
    expect(m.structuredContent.firmware_update).toMatchObject({ latest: "0.2.0" });
    expect(m.content.map((c: any) => c.text).at(-1)).toMatch(/^Molty firmware 0\.2\.0 is out \(Molty runs 0\.1\.0\)\. .*ask before updating/);
    expect((await mcp(b.backend, b.token, "molty_imu")).structuredContent.firmware_update).toBeUndefined();
  });

  it("a revoked token is never recreated by the notice write", async () => {
    const w = await setup();
    await flush();
    const p = w.room.command(w.token, "imu", {});
    await w.room.revoke(w.token);
    await p;
    expect([...(await w.kv.list("tok:")).keys()]).toEqual([]);
  });
});
