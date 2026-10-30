// SPDX-License-Identifier: MIT
// The "want one built?" waitlist: validation, the storage seam and the rate-limit rule.
// Host-neutral: the Cloudflare host passes a D1-backed store (src/waitlist-host.ts),
// tests use MemoryWaitlistStore. Personal data kept: the address, when, and which page
// form it came from. Nothing else (no IP, no user agent); the rate limiter keys on a
// hash of the IP and its rows only live for the current window.

export interface WaitlistEntry {
  email: string;
  /** ISO 8601 */
  createdAt: string;
  /** which form: "build", "home", ... */
  source: string;
}

export interface WaitlistStore {
  /** Idempotent: an address that is already on the list is left as it was. */
  add(entry: WaitlistEntry): Promise<"added" | "exists">;
  /**
   * Counts one request for `key` in the fixed window starting at `windowStart` (ms) and
   * returns how many it has had in that window, this one included. Entries of older
   * windows may be dropped.
   */
  hit(key: string, windowStart: number): Promise<number>;
}

/** Optional human check (e.g. a Turnstile token): true = passed. */
export type HumanCheck = (token: string, ip: string | undefined) => Promise<boolean>;

/** Per IP: at most `max` requests per `windowMs`. */
export const WAITLIST_RATE = { max: 5, windowMs: 60 * 60 * 1000 } as const;

export const EMAIL_MAX = 254;
// Deliberately plain: one @, no spaces, a dot in the domain, sane characters.
const EMAIL_RE = /^[a-z0-9.!#$%&'*+/=?^_`{|}~-]+@[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$/;
const SOURCE_RE = /^[a-z0-9-]{1,32}$/;

/** Trimmed and lower-cased, or undefined when it is not a plausible address. */
export function normalizeEmail(raw: unknown): string | undefined {
  if (typeof raw !== "string") return undefined;
  const email = raw.trim().toLowerCase();
  if (!email || email.length > EMAIL_MAX || !EMAIL_RE.test(email)) return undefined;
  const [local] = email.split("@");
  if (local.length > 64 || local.startsWith(".") || local.endsWith(".") || local.includes("..")) return undefined;
  return email;
}

/** The form's own name for itself, or "site". */
export function normalizeSource(raw: unknown): string {
  return typeof raw === "string" && SOURCE_RE.test(raw) ? raw : "site";
}

export const windowStart = (now: number, windowMs: number = WAITLIST_RATE.windowMs) => Math.floor(now / windowMs) * windowMs;

export class MemoryWaitlistStore implements WaitlistStore {
  readonly entries = new Map<string, WaitlistEntry>();
  readonly hits = new Map<string, { windowStart: number; hits: number }>();
  async add(entry: WaitlistEntry) {
    if (this.entries.has(entry.email)) return "exists" as const;
    this.entries.set(entry.email, { ...entry });
    return "added" as const;
  }
  async hit(key: string, start: number) {
    for (const [k, v] of this.hits) if (v.windowStart < start) this.hits.delete(k);
    const cur = this.hits.get(key);
    const next = cur && cur.windowStart === start ? { windowStart: start, hits: cur.hits + 1 } : { windowStart: start, hits: 1 };
    this.hits.set(key, next);
    return next.hits;
  }
}
