// SPDX-License-Identifier: MIT
// Latest published firmware release, from the configured GitHub repository's
// Releases (docs/RELEASING.md "Release assets"). Host-neutral: the host supplies
// fetch, a clock and one small store for the cached answer.
//
// Rules:
//  - GitHub is asked at most once per FRESH_MS (positive cache, also for "no
//    release yet"), and at most once per FAIL_RETRY_MS after a failure
//    (negative cache). The last good answer is kept and served stale whenever
//    a refresh fails.
//  - One deadline (timeoutMs) covers a whole refresh (the release + its
//    SHA256SUMS): an agent call never waits on GitHub for longer than that.
//  - Prereleases (and drafts) are ignored unless `prerelease` is configured.
//  - Only official asset URLs survive parsing:
//    https://github.com/<repo>/releases/download/<tag>/<name>. Nothing an agent
//    sends ever becomes a download URL.

import { compareVersions, parseVersion } from "./version.js";

export interface OtaAsset {
  name: string;
  url: string;
  size: number;
  /** Hex sha256 from SHA256SUMS (or GitHub's asset digest); null when the release publishes none. */
  sha256: string | null;
}

export interface ReleaseInfo {
  version: string;
  tag: string;
  prerelease: boolean;
  published_at: string | null;
  /** The release page (human-readable notes). */
  notes_url: string;
  /** The release notes (CHANGELOG section), truncated to MAX_NOTES chars. */
  notes: string;
  /** board name (device/firmware/boards/**\/board.json "name") -> its OTA image. */
  boards: Record<string, OtaAsset>;
}

/** fresh: cached answer within FRESH_MS · stale: older, because refreshing failed · unavailable: never got one. */
export type ReleaseState = "fresh" | "stale" | "unavailable";

export interface ReleaseLookup {
  /** null with state fresh/stale = the repo has no (matching) release yet. */
  release: ReleaseInfo | null;
  state: ReleaseState;
  /** When the answer was fetched from GitHub (unix ms), null when unavailable. */
  checked_at: number | null;
}

/** "wait": refresh if needed (bounded by timeoutMs). "cached": never wait; refresh in the background when due. */
export type LookupMode = "wait" | "cached";

export interface ReleaseSource {
  latest(mode?: LookupMode): Promise<ReleaseLookup>;
}

export interface ReleaseEntry {
  release: ReleaseInfo | null;
  fetched_at: number;
  /** Last failed refresh (negative cache). */
  failed_at?: number;
}

export interface ReleaseStore {
  get(): Promise<ReleaseEntry | undefined>;
  put(e: ReleaseEntry): Promise<void>;
}

export const RELEASE_LIMITS = {
  freshMs: 60 * 60 * 1000,
  failRetryMs: 60 * 1000,
  timeoutMs: 2000,
  maxNotes: 4000,
};

export const DEFAULT_RELEASE_REPO = "apresmoi/molty.uno";
export const REPO_RE = /^[A-Za-z0-9-]{1,39}\/[A-Za-z0-9._-]{1,100}$/;
export const BOARD_NAME_RE = /^[a-z0-9][a-z0-9.-]{0,63}$/;

export interface ReleaseConfig {
  repo: string;
  /** Consider prereleases (vX.Y.Z-rc.N) too. Default false. */
  prerelease?: boolean;
  /** Optional GitHub token (raises the API rate limit). */
  token?: string;
  fetch: (req: Request) => Promise<Response>;
  now: () => number;
  store: ReleaseStore;
  limits?: Partial<typeof RELEASE_LIMITS>;
}

export const ota = (version: string, board: string) => `molty-${version}-${board}-ota.bin`;

/** Parses `sha256sum` output: "<64 hex>  <name>" (or " *<name>"). */
export function parseSums(text: string): Map<string, string> {
  const out = new Map<string, string>();
  for (const line of text.split(/\r?\n/)) {
    const m = /^([0-9a-fA-F]{64}) [ *](\S+)\s*$/.exec(line);
    if (m) out.set(m[2], m[1].toLowerCase());
  }
  return out;
}

interface GhAsset {
  name?: unknown;
  browser_download_url?: unknown;
  size?: unknown;
  digest?: unknown;
}
interface GhRelease {
  tag_name?: unknown;
  draft?: unknown;
  prerelease?: unknown;
  html_url?: unknown;
  published_at?: unknown;
  body?: unknown;
  assets?: unknown;
}

const isObj = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);

export class ReleaseCache implements ReleaseSource {
  private readonly c: ReleaseConfig;
  private readonly l: typeof RELEASE_LIMITS;
  private inflight: Promise<{ entry: ReleaseEntry; ok: boolean }> | undefined;

  constructor(config: ReleaseConfig) {
    if (!REPO_RE.test(config.repo)) throw new Error(`bad releases repo ${config.repo}`);
    this.c = config;
    this.l = { ...RELEASE_LIMITS, ...config.limits };
  }

  async latest(mode: LookupMode = "wait"): Promise<ReleaseLookup> {
    const now = this.c.now();
    const entry = await this.c.store.get();
    const fresh = !!entry && entry.fetched_at > 0 && now - entry.fetched_at < this.l.freshMs;
    const backoff = entry?.failed_at !== undefined && now - entry.failed_at < this.l.failRetryMs;
    if (fresh || backoff) return view(entry, fresh ? "fresh" : "stale");
    const refresh = this.refresh(entry);
    if (mode === "cached") {
      refresh.catch(() => {});
      return view(entry, "stale");
    }
    const next = await refresh;
    return view(next.entry, next.ok ? "fresh" : "stale");
  }

  /** One refresh at a time; resolves to the entry now stored (new, or the old one marked failed). */
  private refresh(old: ReleaseEntry | undefined): Promise<{ entry: ReleaseEntry; ok: boolean }> {
    this.inflight ??= this.doRefresh(old).finally(() => (this.inflight = undefined));
    return this.inflight;
  }

  private async doRefresh(old: ReleaseEntry | undefined): Promise<{ entry: ReleaseEntry; ok: boolean }> {
    const ctl = new AbortController();
    let timer: ReturnType<typeof setTimeout> | undefined;
    const deadline = new Promise<never>((_, reject) => {
      timer = setTimeout(() => {
        ctl.abort();
        reject(new Error("timeout"));
      }, this.l.timeoutMs);
    });
    try {
      const release = await Promise.race([this.fetchLatest(ctl.signal), deadline]);
      const entry: ReleaseEntry = { release, fetched_at: this.c.now() };
      await this.c.store.put(entry);
      return { entry, ok: true };
    } catch {
      // Keep the last good answer (served stale); fetched_at 0 = never had one.
      const failed: ReleaseEntry = old ? { ...old, failed_at: this.c.now() } : { release: null, fetched_at: 0, failed_at: this.c.now() };
      await this.c.store.put(failed);
      return { entry: failed, ok: false };
    } finally {
      if (timer !== undefined) clearTimeout(timer);
    }
  }

  private headers(): Record<string, string> {
    const h: Record<string, string> = { accept: "application/vnd.github+json", "user-agent": "molty-relay (+https://github.com/apresmoi/molty.uno)", "x-github-api-version": "2022-11-28" };
    if (this.c.token) h.authorization = `Bearer ${this.c.token}`;
    return h;
  }

  private async fetchLatest(signal: AbortSignal): Promise<ReleaseInfo | null> {
    const api = `https://api.github.com/repos/${this.c.repo}/releases`;
    let gh: GhRelease | undefined;
    if (this.c.prerelease) {
      const res = await this.c.fetch(new Request(`${api}?per_page=30`, { headers: this.headers(), signal }));
      if (!res.ok) throw new Error(`github ${res.status}`);
      const list = (await res.json()) as unknown;
      if (!Array.isArray(list)) throw new Error("github: not a list");
      gh = list
        .filter((r): r is GhRelease => isObj(r) && r.draft !== true && typeof r.tag_name === "string" && !!parseVersion(r.tag_name as string))
        .sort((a, b) => compareVersions(b.tag_name as string, a.tag_name as string) ?? 0)[0];
      if (!gh) return null;
    } else {
      const res = await this.c.fetch(new Request(`${api}/latest`, { headers: this.headers(), signal }));
      if (res.status === 404) return null; // no release published yet
      if (!res.ok) throw new Error(`github ${res.status}`);
      const body = (await res.json()) as unknown;
      if (!isObj(body)) throw new Error("github: not an object");
      gh = body;
      // /releases/latest never returns these, but never trust it to.
      if (gh.draft === true || gh.prerelease === true) return null;
    }
    return this.parse(gh, signal);
  }

  private async parse(gh: GhRelease, signal: AbortSignal): Promise<ReleaseInfo | null> {
    const tag = typeof gh.tag_name === "string" ? gh.tag_name : "";
    const v = parseVersion(tag);
    if (!v) return null; // not a vX.Y.Z tag: not one of ours
    const version = tag.replace(/^v/, "");
    const prefix = `https://github.com/${this.c.repo}/releases/download/${encodeURIComponent(tag)}/`;
    const assets = (Array.isArray(gh.assets) ? gh.assets : []).filter(isObj) as GhAsset[];
    const official = (a: GhAsset) => typeof a.name === "string" && typeof a.browser_download_url === "string" && a.browser_download_url === prefix + encodeURIComponent(a.name);
    let sums = new Map<string, string>();
    const sumsAsset = assets.find((a) => a.name === "SHA256SUMS" && official(a));
    if (sumsAsset) {
      const res = await this.c.fetch(new Request(sumsAsset.browser_download_url as string, { headers: { "user-agent": this.headers()["user-agent"] }, signal }));
      if (!res.ok) throw new Error(`SHA256SUMS ${res.status}`);
      sums = parseSums(await res.text());
    }
    const boards: Record<string, OtaAsset> = {};
    const re = new RegExp(`^molty-${version.replace(/[.+]/g, "\\$&")}-(.+)-ota\\.bin$`);
    for (const a of assets) {
      const m = typeof a.name === "string" ? re.exec(a.name) : null;
      if (!m || !BOARD_NAME_RE.test(m[1]) || !official(a)) continue;
      if (!Number.isInteger(a.size) || (a.size as number) <= 0) continue;
      const fromSums = sums.get(a.name as string) ?? null;
      const dm = typeof a.digest === "string" ? /^sha256:([0-9a-f]{64})$/i.exec(a.digest) : null;
      const fromDigest = dm ? dm[1].toLowerCase() : null;
      // Two sources that disagree: trust neither (the device refuses an update without one).
      const sha256 = fromSums && fromDigest && fromSums !== fromDigest ? null : (fromSums ?? fromDigest);
      boards[m[1]] = { name: a.name as string, url: a.browser_download_url as string, size: a.size as number, sha256 };
    }
    const htmlOk = typeof gh.html_url === "string" && gh.html_url.startsWith(`https://github.com/${this.c.repo}/releases/`);
    return {
      version,
      tag,
      prerelease: gh.prerelease === true,
      published_at: typeof gh.published_at === "string" ? gh.published_at : null,
      notes_url: htmlOk ? (gh.html_url as string) : `https://github.com/${this.c.repo}/releases/tag/${encodeURIComponent(tag)}`,
      notes: typeof gh.body === "string" ? gh.body.slice(0, this.l.maxNotes) : "",
      boards,
    };
  }
}

function view(e: ReleaseEntry | undefined, state: Exclude<ReleaseState, "unavailable">): ReleaseLookup {
  if (!e || e.fetched_at === 0) return { release: null, state: "unavailable", checked_at: null };
  return { release: e.release, state, checked_at: e.fetched_at };
}

export class MemoryReleaseStore implements ReleaseStore {
  entry: ReleaseEntry | undefined;
  async get() {
    return this.entry === undefined ? undefined : structuredClone(this.entry);
  }
  async put(e: ReleaseEntry) {
    this.entry = structuredClone(e);
  }
}
