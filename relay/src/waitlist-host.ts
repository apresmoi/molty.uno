// SPDX-License-Identifier: MIT
// Cloudflare host for the waitlist: the D1-backed WaitlistStore (binding WAITLIST,
// schema in migrations/) and the optional Turnstile check (secret TURNSTILE_SECRET).
// All rules live in src/core/waitlist.ts and src/surfaces/waitlist.ts.
import type { HumanCheck, WaitlistEntry, WaitlistStore } from "./core/waitlist.js";

export class D1WaitlistStore implements WaitlistStore {
  constructor(private readonly db: D1Database) {}

  async add(e: WaitlistEntry) {
    const r = await this.db
      .prepare("INSERT INTO waitlist (email, created_at, source) VALUES (?1, ?2, ?3) ON CONFLICT(email) DO NOTHING")
      .bind(e.email, e.createdAt, e.source)
      .run();
    return r.meta.changes > 0 ? ("added" as const) : ("exists" as const);
  }

  async hit(key: string, windowStart: number) {
    const [, counted] = await this.db.batch<{ hits: number }>([
      // older windows are no longer needed: hashed IPs never outlive their hour
      this.db.prepare("DELETE FROM waitlist_rate WHERE window_start < ?1").bind(windowStart),
      this.db
        .prepare(
          `INSERT INTO waitlist_rate (key, window_start, hits) VALUES (?1, ?2, 1)
           ON CONFLICT(key) DO UPDATE SET
             hits = CASE WHEN window_start = excluded.window_start THEN hits + 1 ELSE 1 END,
             window_start = excluded.window_start
           RETURNING hits`,
        )
        .bind(key, windowStart),
    ]);
    return counted.results[0]?.hits ?? 1;
  }
}

const SITEVERIFY = "https://challenges.cloudflare.com/turnstile/v0/siteverify";

export function turnstileCheck(secret: string): HumanCheck {
  return async (token, ip) => {
    const body = new URLSearchParams({ secret, response: token });
    if (ip) body.set("remoteip", ip);
    try {
      const res = await fetch(SITEVERIFY, { method: "POST", body });
      const out = (await res.json()) as { success?: boolean };
      return out.success === true;
    } catch {
      return false;
    }
  };
}
