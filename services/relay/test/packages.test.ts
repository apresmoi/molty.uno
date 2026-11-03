// Packages and scenes (docs/PACKAGES.md): pkg.install / remove / list and scene.play / queue / clear
// are validated here and forwarded to the device, gated by its caps. The relay never sees a package's bytes.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { CommandError, MAX_PKG_BYTES, MAX_SCENE_PARAMS_BYTES, validateCommand } from "../src/core/commands.js";
import type { DeviceRoom } from "../src/core/room.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeSocket, LINK, connect, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, pkgConfirmMs: 500, pkgInstallMs: 1500 };
const CAPS = ["say", "listen", "pkg", "scene"];
const SHA = "7d618cba219c6e97b7706c43a352ee6b4a6253ab84ad61c954a4c9ae0dbf8be2";
const INSTALL = { id: "astrology", url: "https://raw.githubusercontent.com/apresmoi/molty.uno/main/packages/astrology/astrology.json", sha256: SHA, size: 75232, name: "Astrology" };
const PLAY = { pkg: "astrology", layout: "tonight", params: { moon_lit: 0.499, moon_side: -1, sign: "leo" } };

type Opts = { caps?: string[]; installMs?: number; refuse?: string; outcome?: object | null };
/**
 * A device that answers every cmd at once. pkg.install: `refuse` in the res, else {job, id} and, after
 * `installMs` (the owner's tap and the download), its `pkg` event (`outcome`; null = never).
 */
function device(room: DeviceRoom, sock: FakeSocket, opts: Opts = {}) {
  const cmds: Array<{ name: string; args: any }> = [];
  const orig = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args });
    const install = f.name === "pkg.install";
    const res = install && opts.refuse ? { ok: false, error: opts.refuse } : { ok: true, result: install ? { job: f.args.job, id: f.args.id } : f.name === "pkg.list" ? { packages: [] } : {} };
    setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ...res })), 1);
    const ev = opts.outcome === undefined ? { state: "installed", version: "1.0.0" } : opts.outcome;
    if (install && !opts.refuse && ev) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "pkg", data: { job: f.args.job, id: f.args.id, ...ev } })), opts.installMs ?? 5);
  };
  return { cmds, sent: (name: string) => cmds.filter((c) => c.name === name).map((c) => c.args) };
}

async function setup(opts: Opts = {}) {
  const ctx = makeRoom({ timings: FAST });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: opts.caps ?? CAPS });
  const dev = device(ctx.room, sock, opts);
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "astro" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  const backend: RelayBackend = { device: () => ctx.room };
  return { ...ctx, dev, token: out.result.agent_token as string, backend };
}

const code = (f: () => unknown) => {
  try {
    f();
  } catch (e) {
    return (e as CommandError).code;
  }
  return "ok";
};
const cmd = async (w: Awaited<ReturnType<typeof setup>>, name: string, args: unknown, wait?: boolean) => {
  const r = await handleRest(
    new Request(`https://relay.test/v1/devices/${DEVICE}/cmd`, { method: "POST", headers: { authorization: `Bearer ${w.token}`, "content-type": "application/json" }, body: JSON.stringify({ name, args, ...(wait ? { wait } : {}) }) }),
    w.backend,
  );
  return { status: r.status, body: (await r.json()) as any };
};
let rid = 0;
const mcp = async (b: RelayBackend, token: string, name: string, args: unknown) =>
  ((await (await handleMcp(new Request("https://relay.test/mcp", { method: "POST", headers: { "content-type": "application/json", authorization: `Bearer ${token}` }, body: JSON.stringify({ jsonrpc: "2.0", id: ++rid, method: "tools/call", params: { name, arguments: args } }) }), b)).json()) as any).result;

describe("package and scene commands: validation", () => {
  it("pkg.install: id, https url, sha256, size <= 256 KB, a printable name; forwarded as given", () => {
    expect(validateCommand("pkg.install", INSTALL)).toEqual({ name: "pkg.install", args: INSTALL, job: true });
    for (const bad of [
      { ...INSTALL, id: "Astro" },
      { ...INSTALL, id: "a".repeat(33) },
      { ...INSTALL, url: "http://example.org/a.json" },
      { ...INSTALL, url: "https://example.org/a b.json" },
      { ...INSTALL, url: `https://example.org/${"a".repeat(512)}` },
      { ...INSTALL, sha256: SHA.toUpperCase() },
      { ...INSTALL, sha256: SHA.slice(1) },
      { ...INSTALL, size: MAX_PKG_BYTES + 1 },
      { ...INSTALL, size: 0 },
      { ...INSTALL, size: 1.5 },
      { ...INSTALL, name: "" },
      { ...INSTALL, name: "Astrología" },
      { ...INSTALL, extra: 1 },
      { id: "astrology" },
    ]) expect(code(() => validateCommand("pkg.install", bad)), JSON.stringify(bad)).toBe("invalid_args");
  });

  it("scene.play / queue: pkg, layout, params (an object, at most 2 KB), ttl_s 1..600 (default 60)", () => {
    expect(validateCommand("scene.play", PLAY)).toEqual({ name: "scene.play", args: { ...PLAY, ttl_s: 60 }, job: false });
    expect(validateCommand("scene.queue", { pkg: "astrology", layout: "signs" }).args).toEqual({ pkg: "astrology", layout: "signs", params: {}, ttl_s: 60 });
    expect(validateCommand("scene.play", { ...PLAY, ttl_s: 600 }).args.ttl_s).toBe(600);
    const big = { path: Array.from({ length: 200 }, (_, i) => ({ x: i / 200, y: 0.5, on: i, off: i + 1 })) };
    expect(JSON.stringify(big).length).toBeGreaterThan(MAX_SCENE_PARAMS_BYTES);
    for (const bad of [{ ...PLAY, params: big }, { ...PLAY, params: [1] }, { ...PLAY, ttl_s: 601 }, { ...PLAY, ttl_s: 0 }, { ...PLAY, layout: "../x" }, { ...PLAY, pkg: undefined }, { ...PLAY, color: "red" }]) {
      expect(code(() => validateCommand("scene.play", bad)), JSON.stringify(bad).slice(0, 80)).toBe("invalid_args");
    }
  });

  it("pkg.remove {id}; pkg.list and scene.clear take nothing", () => {
    expect(validateCommand("pkg.remove", { id: "astrology" }).args).toEqual({ id: "astrology" });
    expect(validateCommand("pkg.list", {}).args).toEqual({});
    expect(validateCommand("scene.clear", undefined).args).toEqual({});
    expect(code(() => validateCommand("pkg.remove", {}))).toBe("invalid_args");
    expect(code(() => validateCommand("pkg.list", { id: "x" }))).toBe("invalid_args");
    expect(code(() => validateCommand("scene.clear", { pkg: "x" }))).toBe("invalid_args");
  });
});

describe("package and scene commands: forwarding", () => {
  it("scene.play, scene.queue, scene.clear, pkg.list and pkg.remove go to the device as validated", async () => {
    const w = await setup();
    expect((await cmd(w, "scene.play", PLAY)).body).toMatchObject({ ok: true });
    expect((await cmd(w, "scene.queue", { pkg: "astrology", layout: "signs", ttl_s: 40 })).body).toMatchObject({ ok: true });
    expect((await cmd(w, "scene.clear", {})).body).toMatchObject({ ok: true });
    expect((await cmd(w, "pkg.list", {})).body).toMatchObject({ ok: true, result: { packages: [] } });
    expect((await cmd(w, "pkg.remove", { id: "astrology" })).body).toMatchObject({ ok: true });
    expect(w.dev.cmds).toEqual([
      { name: "scene.play", args: { ...PLAY, ttl_s: 60 } },
      { name: "scene.queue", args: { pkg: "astrology", layout: "signs", params: {}, ttl_s: 40 } },
      { name: "scene.clear", args: {} },
      { name: "pkg.list", args: {} },
      { name: "pkg.remove", args: { id: "astrology" } },
    ]);
  });

  it("firmware without the caps: a clean 409 unsupported, nothing sent", async () => {
    const old = await setup({ caps: ["say", "listen"] });
    for (const [name, args] of [["pkg.install", INSTALL], ["pkg.list", {}], ["scene.play", PLAY], ["scene.clear", {}]] as const) {
      const r = await cmd(old, name, args);
      expect(r.status, name).toBe(409);
      expect(r.body).toMatchObject({ ok: false, error: "unsupported" });
    }
    // scenes without packages (and the other way round) are gated separately
    const half = await setup({ caps: ["pkg"] });
    expect((await cmd(half, "pkg.list", {})).status).toBe(200);
    expect((await cmd(half, "scene.play", PLAY)).body).toMatchObject({ error: "unsupported" });
    expect(old.dev.cmds).toEqual([]);
    expect(half.dev.sent("scene.play")).toEqual([]);
  });

  it("pkg.install: the device acks at once (job, timeout_ms added), the job ends on its pkg event, past a plain command's timeout", async () => {
    // the owner taps after 600 ms, twice the plain command timeout
    const w = await setup({ installMs: 600 });
    const r = await cmd(w, "pkg.install", INSTALL, true);
    expect(r.status).toBe(200);
    expect(r.body.result).toMatchObject({ status: "done", installed: "astrology", version: "1.0.0" });
    const [sent] = w.dev.sent("pkg.install");
    expect(sent).toEqual({ ...INSTALL, job: r.body.result.job, timeout_ms: 500 });
    // without wait: the job id at once, its end from GET /v1/jobs
    const j = await cmd(w, "pkg.install", INSTALL);
    expect(j.body.result.job).toMatch(/\.j_/);
    expect(await w.room.job(w.token, j.body.result.job, 2000)).toMatchObject({ ok: true, result: { kind: "pkg", status: "done", result: { installed: "astrology" } } });
  });

  it("declined / failed events and refusals end the job with a clear error; no event at all is pkg_timeout", async () => {
    const no = await setup({ outcome: { state: "declined", reason: "declined" } });
    expect((await cmd(no, "pkg.install", INSTALL, true)).body).toMatchObject({ ok: false, error: "declined", message: expect.stringMatching(/said no/) });
    const late = await setup({ outcome: { state: "declined", reason: "timeout" } });
    expect((await cmd(late, "pkg.install", INSTALL, true)).body).toMatchObject({ ok: false, error: "declined", message: expect.stringMatching(/in time/) });
    const bad = await setup({ outcome: { state: "failed", error: "hash_mismatch" } });
    expect((await cmd(bad, "pkg.install", INSTALL, true)).body).toMatchObject({ ok: false, error: "hash_mismatch" });
    const full = await setup({ refuse: "no_space" });
    expect((await cmd(full, "pkg.install", INSTALL, true)).body).toMatchObject({ ok: false, error: "no_space" });
    const silent = await setup({ outcome: null });
    expect((await cmd(silent, "pkg.install", INSTALL, true)).body).toMatchObject({ ok: false, error: "pkg_timeout" });
  });

  it("a pkg event for another job or package changes nothing", async () => {
    const w = await setup({ outcome: null });
    const j = (await cmd(w, "pkg.install", INSTALL)).body.result.job as string;
    await new Promise((r) => setTimeout(r, 20));
    const sock = (w.room as any).liveSocket();
    await w.room.socketMessage(sock, JSON.stringify({ t: "evt", name: "pkg", data: { job: "molty-k7m2q9xa4c.j_other0000000", id: "astrology", state: "installed" } }));
    await w.room.socketMessage(sock, JSON.stringify({ t: "evt", name: "pkg", data: { job: j, id: "cats", state: "installed" } }));
    expect(await w.room.job(w.token, j, 0)).toMatchObject({ ok: true, result: { status: "running" } });
    await w.room.socketMessage(sock, JSON.stringify({ t: "evt", name: "pkg", data: { id: "astrology", state: "installed", version: "1.0.0" } }));
    expect(await w.room.job(w.token, j, 500)).toMatchObject({ ok: true, result: { status: "done", result: { installed: "astrology", version: "1.0.0" } } });
  });

  it("MCP molty_package and molty_scene go through the same validation", async () => {
    const w = await setup();
    expect((await mcp(w.backend, w.token, "molty_package", { op: "install", ...INSTALL, wait: true })).structuredContent).toMatchObject({ status: "done", installed: "astrology" });
    expect((await mcp(w.backend, w.token, "molty_scene", { op: "play", ...PLAY })).isError).toBe(false);
    expect((await mcp(w.backend, w.token, "molty_scene", { op: "clear" })).isError).toBe(false);
    expect((await mcp(w.backend, w.token, "molty_scene", { op: "play", ...PLAY, ttl_s: 9999 })).structuredContent).toMatchObject({ error: "invalid_args" });
    expect(w.dev.cmds.map((c) => c.name)).toEqual(["pkg.install", "scene.play", "scene.clear"]);
  });
});
