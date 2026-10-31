// Latest-release lookup (core/releases.ts): GitHub parsing, the hourly cache,
// stale-on-error with a short negative cache, zero releases, prereleases, board
// asset selection, checksums; and the public GET /v1/releases/latest.
import { describe, expect, it } from "vitest";
import { MemoryReleaseStore, type ReleaseLookup, ReleaseCache, type ReleaseSource, parseSums } from "../src/core/releases.js";
import { FEATURES, compareVersions, parseVersion } from "../src/core/version.js";
import { handleReleases, servePublicRelease } from "../src/surfaces/releases.js";

const REPO = "apresmoi/molty.uno";
const B146 = "waveshare-esp32-s3-touch-lcd-1.46";
const B128 = "waveshare-esp32-s3-touch-lcd-1.28";
const H1 = "a".repeat(64);
const H2 = "b".repeat(64);
const dl = (tag: string, name: string) => `https://github.com/${REPO}/releases/download/${tag}/${name}`;

function ghRelease(version: string, opts: { prerelease?: boolean; draft?: boolean; sums?: boolean; extra?: unknown[]; digest?: Record<string, string> } = {}) {
  const tag = `v${version}`;
  const asset = (name: string, size = 1500000) => ({ name, browser_download_url: dl(tag, name), size, ...(opts.digest?.[name] ? { digest: opts.digest[name] } : {}) });
  return {
    tag_name: tag,
    draft: opts.draft ?? false,
    prerelease: opts.prerelease ?? false,
    html_url: `https://github.com/${REPO}/releases/tag/${tag}`,
    published_at: "2026-11-01T10:00:00Z",
    body: `## ${tag}\n- molty_ask on the device`,
    assets: [
      asset(`molty-${version}-${B146}-ota.bin`),
      asset(`molty-${version}-${B146}-factory.bin`, 9000000),
      asset(`molty-${version}-${B128}-ota.bin`),
      asset(`molty-${version}-console.html`, 90000),
      ...(opts.sums === false ? [] : [asset("SHA256SUMS", 900)]),
      ...(opts.extra ?? []),
    ],
  };
}

/** A fake GitHub: answers per URL; records every request. */
function github(routes: Record<string, () => Response | Promise<Response>>) {
  const calls: string[] = [];
  const fetch = async (req: Request) => {
    calls.push(req.url);
    if (req.signal.aborted) throw new Error("aborted");
    const r = routes[req.url];
    return r ? r() : new Response("not found", { status: 404 });
  };
  return { calls, fetch };
}

const sums = (version: string) => `${H1}  molty-${version}-${B146}-ota.bin\n${H2} *molty-${version}-${B128}-ota.bin\n${"c".repeat(64)}  molty-${version}-console.html\n`;
const LATEST = `https://api.github.com/repos/${REPO}/releases/latest`;

function cache(routes: Record<string, () => Response | Promise<Response>>, opts: { prerelease?: boolean; limits?: Record<string, number>; token?: string } = {}) {
  const gh = github(routes);
  const clock = { t: 1_000_000 };
  const store = new MemoryReleaseStore();
  const c = new ReleaseCache({ repo: REPO, prerelease: opts.prerelease, token: opts.token, fetch: gh.fetch, now: () => clock.t, store, limits: opts.limits });
  return { c, gh, clock, store };
}

const ok = (body: unknown) => () => Response.json(body);

describe("versions", () => {
  it("parses X.Y.Z[-pre] and orders by semver precedence; anything else is not comparable", () => {
    expect(parseVersion("0.1.0")).toBeTruthy();
    expect(parseVersion("v1.2.3-rc.1+build.5")).toBeTruthy();
    for (const bad of ["dev", "test-fw", "1.2", "01.2.3", "", null, 3]) expect(parseVersion(bad), String(bad)).toBeUndefined();
    const order = ["0.1.0", "0.2.0-alpha", "0.2.0-alpha.1", "0.2.0-alpha.beta", "0.2.0-beta.2", "0.2.0-beta.11", "0.2.0-rc.1", "0.2.0", "0.10.0", "1.0.0"];
    for (let i = 0; i < order.length - 1; i++) {
      expect(compareVersions(order[i], order[i + 1]), `${order[i]} < ${order[i + 1]}`).toBeLessThan(0);
      expect(compareVersions(order[i + 1], order[i])).toBeGreaterThan(0);
    }
    expect(compareVersions("v0.2.0", "0.2.0")).toBe(0);
    expect(compareVersions("dev", "0.2.0")).toBeUndefined();
    expect(FEATURES.ask.min_fw).toBe("0.2.0"); // the next release ships the device side of ask
  });
});

describe("release lookup", () => {
  it("parses the latest release: version, notes, per-board OTA image with its SHA256SUMS hash", async () => {
    const { c, gh } = cache({ [LATEST]: ok(ghRelease("0.2.0")), [dl("v0.2.0", "SHA256SUMS")]: () => new Response(sums("0.2.0")) });
    const l = await c.latest();
    expect(l.state).toBe("fresh");
    expect(l.release).toMatchObject({ version: "0.2.0", tag: "v0.2.0", prerelease: false, notes_url: `https://github.com/${REPO}/releases/tag/v0.2.0`, notes: expect.stringContaining("molty_ask") });
    expect(l.release!.boards).toEqual({
      [B146]: { name: `molty-0.2.0-${B146}-ota.bin`, url: dl("v0.2.0", `molty-0.2.0-${B146}-ota.bin`), size: 1500000, sha256: H1 },
      [B128]: { name: `molty-0.2.0-${B128}-ota.bin`, url: dl("v0.2.0", `molty-0.2.0-${B128}-ota.bin`), size: 1500000, sha256: H2 },
    });
    expect(gh.calls).toEqual([LATEST, dl("v0.2.0", "SHA256SUMS")]);
  });

  it("asks GitHub once per hour (fresh cache), with a User-Agent and the optional token", async () => {
    const seen: Request[] = [];
    const { c, gh, clock } = cache({ [LATEST]: ok(ghRelease("0.2.0", { sums: false })) }, { token: "ghp_x" });
    const orig = gh.fetch;
    (c as any).c.fetch = (r: Request) => (seen.push(r), orig(r));
    await c.latest();
    await c.latest();
    clock.t += 59 * 60 * 1000;
    await c.latest("wait");
    expect(gh.calls).toHaveLength(1);
    expect(seen[0].headers.get("user-agent")).toMatch(/molty-relay/);
    expect(seen[0].headers.get("authorization")).toBe("Bearer ghp_x");
    clock.t += 2 * 60 * 1000;
    await c.latest();
    expect(gh.calls).toHaveLength(2);
  });

  it("serves the last good answer stale when a refresh fails, and retries at most once a minute", async () => {
    let down = false;
    const { c, gh, clock } = cache({ [LATEST]: () => (down ? new Response("rate limited", { status: 403 }) : Response.json(ghRelease("0.2.0", { sums: false }))) });
    expect((await c.latest()).state).toBe("fresh");
    down = true;
    clock.t += 61 * 60 * 1000;
    const stale = await c.latest();
    expect(stale.state).toBe("stale");
    expect(stale.release!.version).toBe("0.2.0");
    clock.t += 30 * 1000;
    expect((await c.latest()).state).toBe("stale");
    expect(gh.calls).toHaveLength(2); // negative cache: no retry within a minute
    clock.t += 31 * 1000;
    down = false;
    expect((await c.latest()).state).toBe("fresh");
    expect(gh.calls).toHaveLength(3);
  });

  it("never had an answer and GitHub fails: unavailable (not 'no release'), negative-cached", async () => {
    const { c, gh, clock } = cache({ [LATEST]: () => new Response("boom", { status: 500 }) });
    expect(await c.latest()).toEqual({ release: null, state: "unavailable", checked_at: null });
    expect(await c.latest()).toEqual({ release: null, state: "unavailable", checked_at: null });
    expect(gh.calls).toHaveLength(1);
    clock.t += 61 * 1000;
    await c.latest();
    expect(gh.calls).toHaveLength(2);
  });

  it("zero releases (404 on /releases/latest) is a cached answer: release null, fresh", async () => {
    const { c, gh } = cache({});
    const l = await c.latest();
    expect(l).toEqual({ release: null, state: "fresh", checked_at: 1_000_000 });
    await c.latest();
    expect(gh.calls).toEqual([LATEST]);
  });

  it("a hanging GitHub never holds a call longer than the timeout", async () => {
    const { c } = cache({ [LATEST]: () => new Promise<Response>(() => {}) }, { limits: { timeoutMs: 50 } });
    const t0 = Date.now();
    expect((await c.latest()).state).toBe("unavailable");
    expect(Date.now() - t0).toBeLessThan(500);
  });

  it("'cached' mode never waits: it answers from the store and refreshes in the background", async () => {
    let release: (r: Response) => void = () => {};
    const { c, gh } = cache({ [LATEST]: () => new Promise<Response>((r) => (release = r)) });
    expect(await c.latest("cached")).toEqual({ release: null, state: "unavailable", checked_at: null });
    expect(gh.calls).toHaveLength(1);
    release(Response.json(ghRelease("0.2.0", { sums: false })));
    await new Promise((r) => setTimeout(r, 10));
    expect((await c.latest("cached")).release?.version).toBe("0.2.0");
  });

  it("ignores prereleases and drafts unless configured; configured, picks the highest version", async () => {
    const pre = cache({ [LATEST]: ok(ghRelease("0.3.0-rc.1", { prerelease: true, sums: false })) });
    expect((await pre.c.latest()).release).toBeNull();
    const list = `https://api.github.com/repos/${REPO}/releases?per_page=30`;
    const withPre = cache(
      {
        [list]: ok([ghRelease("0.2.0", { sums: false }), ghRelease("0.3.0-rc.1", { prerelease: true, sums: false }), ghRelease("0.4.0", { draft: true, sums: false }), { tag_name: "nightly" }]),
      },
      { prerelease: true },
    );
    const l = await withPre.c.latest();
    expect(l.release).toMatchObject({ version: "0.3.0-rc.1", prerelease: true });
    expect(withPre.gh.calls).toEqual([list]);
  });

  it("board selection: only this version's -ota.bin, only official URLs of the configured repo", async () => {
    const rel = ghRelease("0.2.0", {
      sums: false,
      extra: [
        { name: `molty-0.1.0-${B146}-ota.bin`, browser_download_url: dl("v0.1.0", `molty-0.1.0-${B146}-ota.bin`), size: 10 }, // another version
        { name: "molty-0.2.0-evil-board-ota.bin", browser_download_url: "https://evil.example/molty-0.2.0-evil-board-ota.bin", size: 10 }, // not official
        { name: "molty-0.2.0-other-ota.bin", browser_download_url: `https://github.com/someone/fork/releases/download/v0.2.0/molty-0.2.0-other-ota.bin`, size: 10 }, // other repo
        { name: "molty-0.2.0-empty-ota.bin", browser_download_url: dl("v0.2.0", "molty-0.2.0-empty-ota.bin"), size: 0 },
      ],
    });
    const { c } = cache({ [LATEST]: ok(rel) });
    const boards = (await c.latest()).release!.boards;
    expect(Object.keys(boards).sort()).toEqual([B128, B146]);
    expect(boards[B146].sha256).toBeNull(); // no SHA256SUMS, no digest
  });

  it("checksums: SHA256SUMS lines (text or *binary mode); GitHub's asset digest as fallback; disagreement = none", async () => {
    expect(parseSums(`${H1}  a.bin\n${H2} *b.bin\nbad line\n${"A".repeat(64)}  c.bin\r\n`)).toEqual(
      new Map([
        ["a.bin", H1],
        ["b.bin", H2],
        ["c.bin", "a".repeat(64)],
      ]),
    );
    const name146 = `molty-0.2.0-${B146}-ota.bin`;
    const name128 = `molty-0.2.0-${B128}-ota.bin`;
    const digestOnly = cache({ [LATEST]: ok(ghRelease("0.2.0", { sums: false, digest: { [name146]: `sha256:${H2}` } })) });
    expect((await digestOnly.c.latest()).release!.boards[B146].sha256).toBe(H2);
    const conflict = cache({
      [LATEST]: ok(ghRelease("0.2.0", { digest: { [name146]: `sha256:${H2}`, [name128]: `sha256:${H2}` } })),
      [dl("v0.2.0", "SHA256SUMS")]: () => new Response(sums("0.2.0")),
    });
    const b = (await conflict.c.latest()).release!.boards;
    expect(b[B146].sha256).toBeNull(); // sums say H1, digest says H2
    expect(b[B128].sha256).toBe(H2); // both agree
  });

  it("a SHA256SUMS that can't be read fails the refresh (no image is offered unverifiable by accident)", async () => {
    const { c } = cache({ [LATEST]: ok(ghRelease("0.2.0")), [dl("v0.2.0", "SHA256SUMS")]: () => new Response("x", { status: 500 }) });
    expect((await c.latest()).state).toBe("unavailable");
  });
});

describe("GET /v1/releases/latest (public)", () => {
  const lookup = (l: Partial<ReleaseLookup>): { latest: () => Promise<ReleaseLookup> } => ({ latest: async () => ({ release: null, state: "fresh", checked_at: 1, ...l }) });
  const src = (async () => {
    const { c } = cache({ [LATEST]: ok(ghRelease("0.2.0")), [dl("v0.2.0", "SHA256SUMS")]: () => new Response(sums("0.2.0")) });
    await c.latest();
    return c;
  })();

  it("returns only public release info, the board's OTA image, CORS-open and cacheable", async () => {
    const res = await handleReleases(new Request(`https://r/v1/releases/latest?board=${B146}`), await src);
    expect(res.status).toBe(200);
    expect(res.headers.get("access-control-allow-origin")).toBe("*");
    expect(res.headers.get("cache-control")).toBe("public, max-age=300");
    const body = (await res.json()) as any;
    expect(body).toEqual({
      ok: true,
      release_check: "fresh",
      latest: { version: "0.2.0", tag: "v0.2.0", prerelease: false, published_at: "2026-11-01T10:00:00Z", notes_url: `https://github.com/${REPO}/releases/tag/v0.2.0`, notes: expect.any(String) },
      board: { name: B146, ota: { url: dl("v0.2.0", `molty-0.2.0-${B146}-ota.bin`), size: 1500000, sha256: H1 } },
      features: [
        { feature: "molty_ask", cap: "ask", needs: "0.2.0" },
        { feature: "molty_update", cap: "update", needs: "0.2.0" },
        { feature: "molty_act", cap: "act", needs: "0.2.0" },
        { feature: "molty_working", cap: "working", needs: "0.2.0" },
      ],
    });
    expect(((await (await handleReleases(new Request("https://r/v1/releases/latest?board=nope-board"), await src)).json()) as any).board).toBeNull();
    expect(((await (await handleReleases(new Request("https://r/v1/releases/latest"), await src)).json()) as any).board).toBeNull();
  });

  it("zero releases: latest null; degraded answers are cached briefly", async () => {
    const body = (await (await handleReleases(new Request(`https://r/v1/releases/latest?board=${B146}`), lookup({}))).json()) as any;
    expect(body).toMatchObject({ ok: true, release_check: "fresh", latest: null, board: null });
    const res = await handleReleases(new Request("https://r/v1/releases/latest"), lookup({ state: "unavailable", checked_at: null }));
    expect(res.headers.get("cache-control")).toBe("public, max-age=30");
  });

  it("validates the board, refuses other methods, answers preflights, rate-limits when told", async () => {
    expect((await handleReleases(new Request("https://r/v1/releases/latest?board=../../etc"), lookup({}))).status).toBe(400);
    expect((await handleReleases(new Request("https://r/v1/releases/latest", { method: "POST" }), lookup({}))).status).toBe(405);
    const pre = await handleReleases(new Request("https://r/v1/releases/latest", { method: "OPTIONS" }), lookup({}));
    expect(pre.status).toBe(204);
    expect(pre.headers.get("access-control-allow-origin")).toBe("*");
    const limited = await handleReleases(new Request("https://r/v1/releases/latest"), lookup({}), { limit: async () => false });
    expect(limited.status).toBe(429);
    expect(limited.headers.get("access-control-allow-origin")).toBe("*");
  });

  it("as served: every method that reaches the lookup (GET miss, HEAD) is rate-limited; cache hits and preflights are not", async () => {
    let lookups = 0;
    let limits = 0;
    const source: ReleaseSource = { latest: async () => (lookups++, { release: null, state: "fresh", checked_at: 1 }) };
    const store = new Map<string, Response>();
    const edge = { match: async (k: Request) => store.get(k.url)?.clone(), put: async (k: Request, r: Response) => void store.set(k.url, r) };
    const host = (allow: boolean) => ({ cache: edge, limit: async () => (limits++, allow) });
    const at = (method: string) => new Request(`https://r/v1/releases/latest?board=${B146}`, { method });
    // denied: neither HEAD nor a GET miss reaches the release lookup
    expect((await servePublicRelease(at("HEAD"), source, host(false))).status).toBe(429);
    expect((await servePublicRelease(at("GET"), source, host(false))).status).toBe(429);
    expect([lookups, limits]).toEqual([0, 2]);
    expect((await servePublicRelease(at("OPTIONS"), source, host(false))).status).toBe(204);
    expect((await servePublicRelease(at("POST"), source, host(false))).status).toBe(405);
    expect(limits).toBe(2);
    // allowed: the GET fills the edge cache; the next GET is a hit (no limit, no lookup); HEAD is limited again
    expect((await servePublicRelease(at("GET"), source, host(true))).status).toBe(200);
    expect((await servePublicRelease(at("GET"), source, host(false))).status).toBe(200);
    expect([lookups, limits]).toEqual([1, 3]);
    expect((await servePublicRelease(at("HEAD"), source, host(false))).status).toBe(429);
    expect([lookups, limits]).toEqual([1, 4]);
  });
});
