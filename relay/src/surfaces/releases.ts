// SPDX-License-Identifier: MIT
// GET /v1/releases/latest[?board=<board>]: the latest public firmware release, for the
// device console's "update available" line. No auth and no device data: only what the
// GitHub release page already shows publicly. CORS-open (the console runs on the
// device's own origin), cacheable (public, 5 min), optionally rate-limited per client
// by the host. Fetch-API only.
//
//   200 {ok:true, release_check: "fresh"|"stale"|"unavailable",
//        latest: null | {version, tag, prerelease, published_at, notes_url, notes},
//        board: null | {name, ota: {url, size, sha256|null}},
//        features: [{feature, cap, needs}]}
//   400 bad board · 405 not GET · 429 rate limited

import { BOARD_NAME_RE, type ReleaseSource } from "../core/releases.js";
import { FEATURES } from "../core/version.js";

export const RELEASES_PATH = "/v1/releases/latest";

const CORS = { "access-control-allow-origin": "*", "access-control-allow-methods": "GET, HEAD, OPTIONS", "access-control-max-age": "86400" };

const json = (body: unknown, status: number, cache: string) =>
  new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json", "cache-control": cache, ...CORS } });

export interface ReleasesOptions {
  /** true = allowed. Absent = no rate limit (the response is cached anyway). */
  limit?: () => Promise<boolean>;
}

export async function handleReleases(request: Request, source: ReleaseSource, opts: ReleasesOptions = {}): Promise<Response> {
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
  if (request.method !== "GET" && request.method !== "HEAD") return json({ ok: false, error: "method_not_allowed" }, 405, "no-store");
  const board = new URL(request.url).searchParams.get("board");
  if (board !== null && !BOARD_NAME_RE.test(board)) return json({ ok: false, error: "invalid_args", message: "board must be a board name like waveshare-esp32-s3-touch-lcd-1.46" }, 400, "no-store");
  if (opts.limit && !(await opts.limit())) return json({ ok: false, error: "rate_limited" }, 429, "no-store");
  const lookup = await source.latest("wait");
  const r = lookup.release;
  const asset = r && board ? r.boards[board] : undefined;
  const body = {
    ok: true,
    release_check: lookup.state,
    latest: r ? { version: r.version, tag: r.tag, prerelease: r.prerelease, published_at: r.published_at, notes_url: r.notes_url, notes: r.notes } : null,
    board: asset ? { name: board, ota: { url: asset.url, size: asset.size, sha256: asset.sha256 } } : null,
    features: Object.values(FEATURES).map((f) => ({ feature: f.tool, cap: f.cap, needs: f.min_fw })),
  };
  // A fresh answer may sit in caches for a while; a degraded one should not.
  return json(body, 200, lookup.state === "fresh" ? "public, max-age=300" : "public, max-age=30");
}

/** The edge cache in front of the lookup (the Workers Cache API's shape). */
export interface ReleasesEdgeCache {
  match(key: Request): Promise<Response | undefined>;
  put(key: Request, res: Response): Promise<void>;
}

/**
 * The public endpoint as the host serves it: GET answers from the edge cache
 * (keyed on the normalized URL) when it can; every request that reaches the
 * release lookup (GET on a cache miss, HEAD always) passes the rate limit first.
 */
export async function servePublicRelease(
  request: Request,
  source: ReleaseSource,
  host: { limit?: () => Promise<boolean>; cache?: ReleasesEdgeCache; waitUntil?: (p: Promise<unknown>) => void } = {},
): Promise<Response> {
  const opts: ReleasesOptions = { limit: host.limit };
  if (request.method !== "GET" || !host.cache) return handleReleases(request, source, opts);
  const url = new URL(request.url);
  const board = url.searchParams.get("board");
  const key = new Request(`${url.origin}${RELEASES_PATH}${board === null ? "" : `?board=${encodeURIComponent(board)}`}`);
  const hit = await host.cache.match(key);
  if (hit) return hit;
  const res = await handleReleases(request, source, opts);
  if (res.status === 200) {
    const put = host.cache.put(key, res.clone());
    if (host.waitUntil) host.waitUntil(put);
    else await put;
  }
  return res;
}
