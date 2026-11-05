// SPDX-License-Identifier: MIT
// The package catalog (docs/PACKAGES.md "6. Relay"): the repo's packages/index.json at one ref,
// fetched from GitHub and cached briefly, each entry turned into the exact args of pkg.install.
// The relay never downloads a package; it only reads the list of what exists.

import type { Outcome } from "./api.js";
import { MAX_PKG_BYTES, PKG_ID_RE } from "./commands.js";
import { isRecord, type JsonObject } from "./protocol.js";
import type { Clock } from "./store.js";

/** The firmware's download allowlist (docs/PACKAGES.md section 2): every install url starts here. */
export const PACKAGES_RAW = "https://raw.githubusercontent.com/apresmoi/molty.uno/";
export const DEFAULT_PACKAGES_REF = "main";
export const CATALOG_LIMITS = { freshMs: 5 * 60 * 1000, failRetryMs: 30 * 1000, timeoutMs: 3000 };
const REF_RE = /^[A-Za-z0-9._-]+(\/[A-Za-z0-9._-]+)*$/;
const PATH_RE = /^packages\/[a-z0-9-]{1,32}\/[a-z0-9-]{1,32}\.json$/;
const PRINTABLE = (max: number) => new RegExp(`^[\\x20-\\x7e]{1,${max}}$`);

/** One package; scenes packages also carry `layouts` (what scene.play takes) and `skills` (what plays them). */
export type CatalogEntry = JsonObject & {
  id: string;
  name: string;
  version: string;
  kind: string;
  description: string;
  /** Pass as is to pkg.install. */
  install: { id: string; url: string; sha256: string; size: number; name: string };
};

/** One index entry, or null when it is not something pkg.install would take. */
function entry(id: string, e: unknown, base: string): CatalogEntry | null {
  if (!PKG_ID_RE.test(id) || !isRecord(e)) return null;
  const { name, version, kind, description, layouts, skills, path, size, sha256 } = e;
  if (typeof name !== "string" || !PRINTABLE(32).test(name) || typeof version !== "string" || typeof kind !== "string") return null;
  if (typeof path !== "string" || !PATH_RE.test(path) || typeof sha256 !== "string" || !/^[0-9a-f]{64}$/.test(sha256)) return null;
  if (typeof size !== "number" || !Number.isInteger(size) || size < 1 || size > MAX_PKG_BYTES) return null;
  const names = (v: unknown) => (Array.isArray(v) && v.every((s) => typeof s === "string") ? (v as string[]) : undefined);
  return {
    id, name, version, kind,
    description: typeof description === "string" ? description : "",
    ...(names(layouts) ? { layouts: names(layouts) } : {}),
    ...(names(skills) ? { skills: names(skills) } : {}),
    install: { id, url: base + path, sha256, size, name },
  };
}

export class PackageCatalog {
  private good: { entries: CatalogEntry[]; at: number } | undefined;
  private failedAt = -Infinity;
  readonly ref: string;

  constructor(
    private readonly fetcher: (req: Request) => Promise<Response>,
    private readonly clock: Clock,
    ref = DEFAULT_PACKAGES_REF,
  ) {
    this.ref = ref;
  }

  get indexUrl(): string {
    return `${PACKAGES_RAW}${this.ref}/packages/index.json`;
  }

  /** Every package in the index, fresh within freshMs; the last good list when a refresh fails. */
  async entries(): Promise<Outcome<{ ref: string; packages: CatalogEntry[] }>> {
    const now = this.clock.now();
    const fresh = this.good && now - this.good.at < CATALOG_LIMITS.freshMs;
    if (!fresh && now - this.failedAt >= CATALOG_LIMITS.failRetryMs) {
      const why = await this.refresh(now);
      if (why && !this.good) return { ok: false, error: "catalog_unavailable", message: `catalog unavailable: ${why} (${this.indexUrl}); try again in a minute` };
    }
    if (!this.good) return { ok: false, error: "catalog_unavailable", message: `catalog unavailable: ${this.indexUrl} could not be read just now; try again in a minute` };
    return { ok: true, result: { ref: this.ref, packages: this.good.entries } };
  }

  /** Fetches the index; returns why it failed, or undefined. */
  private async refresh(now: number): Promise<string | undefined> {
    const fail = (why: string) => ((this.failedAt = now), why);
    if (!REF_RE.test(this.ref) || this.ref.split("/").includes("..")) return fail(`PACKAGES_REF ${JSON.stringify(this.ref)} is not a branch or tag`);
    let index: unknown;
    try {
      const res = await this.fetcher(new Request(this.indexUrl, { redirect: "manual", signal: AbortSignal.timeout(CATALOG_LIMITS.timeoutMs) }));
      if (!res.ok) return fail(`GitHub answered ${res.status}`);
      index = await res.json();
    } catch {
      return fail("GitHub could not be reached or sent no JSON");
    }
    if (!isRecord(index)) return fail("the index is not a JSON object");
    const base = `${PACKAGES_RAW}${this.ref}/`;
    const entries = Object.entries(index).flatMap(([id, e]) => entry(id, e, base) ?? []);
    this.good = { entries, at: now };
    return undefined;
  }
}
