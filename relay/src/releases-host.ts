// SPDX-License-Identifier: MIT
// Cloudflare host for the latest-release lookup (core/releases.ts). ONE cache for
// the whole relay: a dedicated instance of the MoltyDevice Durable Object class
// (name `releases`, which no device id can equal) owns the ReleaseCache and its
// storage, so GitHub is asked about once an hour in total, not once per device.
// Each isolate keeps a short memo in front of it so ride-alongs cost no RPC.
import { DEFAULT_RELEASE_REPO, type LookupMode, ReleaseCache, type ReleaseEntry, type ReleaseLookup, type ReleaseSource, type ReleaseStore } from "./core/releases.js";

export interface ReleaseEnv {
  /** `owner/repo` whose GitHub Releases are Molty's (default apresmoi/molty.uno). */
  RELEASES_REPO?: string;
  /** "true": prereleases (vX.Y.Z-rc.N) count as the latest release too. */
  RELEASES_PRERELEASE?: string;
  /** Optional secret: a GitHub token, for the higher API rate limit. */
  GITHUB_TOKEN?: string;
}

/** The singleton's Durable Object name (not a valid device id, so it never shares a room). */
export const RELEASES_OBJECT = "releases";
const MEMO_MS = 60 * 1000;

export function releaseCache(env: ReleaseEnv, storage: DurableObjectStorage): ReleaseCache {
  const store: ReleaseStore = {
    get: () => storage.get<ReleaseEntry>("release"),
    put: (e) => storage.put("release", e),
  };
  return new ReleaseCache({
    repo: env.RELEASES_REPO || DEFAULT_RELEASE_REPO,
    prerelease: env.RELEASES_PRERELEASE === "true",
    token: env.GITHUB_TOKEN || undefined,
    fetch: (req) => fetch(req),
    now: () => Date.now(),
    store,
  });
}

interface ReleaseRpc {
  releaseLookup(mode: LookupMode): Promise<ReleaseLookup>;
}

let memo: { at: number; value: ReleaseLookup } | undefined;

/** A ReleaseSource that asks the singleton, memoized per isolate for MEMO_MS. */
export function releaseSource(ns: DurableObjectNamespace): ReleaseSource {
  return {
    async latest(mode: LookupMode = "wait") {
      const now = Date.now();
      if (memo && (now - memo.at < MEMO_MS || mode === "cached")) {
        if (mode === "cached" && now - memo.at >= MEMO_MS) void refresh(ns, "cached");
        return memo.value;
      }
      return refresh(ns, mode);
    },
  };
}

async function refresh(ns: DurableObjectNamespace, mode: LookupMode): Promise<ReleaseLookup> {
  try {
    const stub = ns.get(ns.idFromName(RELEASES_OBJECT)) as unknown as ReleaseRpc;
    const value = await stub.releaseLookup(mode);
    memo = { at: Date.now(), value };
    return value;
  } catch {
    return memo?.value ?? { release: null, state: "unavailable", checked_at: null };
  }
}
