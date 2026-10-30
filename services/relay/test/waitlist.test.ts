// SPDX-License-Identifier: MIT
import { describe, expect, it } from "vitest";
import { MemoryWaitlistStore, WAITLIST_RATE, normalizeEmail } from "../src/core/waitlist.js";
import { handleWaitlist, type WaitlistDeps } from "../src/surfaces/waitlist.js";

const SITE = "https://molty.uno";
const T0 = Date.UTC(2026, 9, 29, 12, 0, 0);

function post(body: unknown, opts: { origin?: string | null; form?: boolean; method?: string } = {}) {
  const origin = opts.origin === undefined ? SITE : opts.origin;
  const headers: Record<string, string> = {
    "content-type": opts.form ? "application/x-www-form-urlencoded" : "application/json",
  };
  if (origin) headers.origin = origin;
  return new Request("https://relay.test/waitlist", {
    method: opts.method ?? "POST",
    headers,
    body: opts.form ? new URLSearchParams(body as Record<string, string>).toString() : JSON.stringify(body),
  });
}

function setup(over: Partial<WaitlistDeps> = {}) {
  const store = new MemoryWaitlistStore();
  let now = T0;
  const deps: WaitlistDeps = { store, ip: "203.0.113.7", now: () => now, ...over };
  return { store, deps, advance: (ms: number) => (now += ms) };
}

describe("waitlist signup", () => {
  it("stores a valid address, trimmed and lower-cased, with its source and time", async () => {
    const { store, deps } = setup();
    const res = await handleWaitlist(post({ email: "  Ada@Example.ORG ", source: "build" }), deps);
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({ ok: true });
    expect([...store.entries.values()]).toEqual([{ email: "ada@example.org", createdAt: new Date(T0).toISOString(), source: "build" }]);
  });

  it("is idempotent on a duplicate address and answers the same as a new one", async () => {
    const { store, deps, advance } = setup();
    await handleWaitlist(post({ email: "ada@example.org" }), deps);
    advance(1000);
    const res = await handleWaitlist(post({ email: "ADA@example.org" }), deps);
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({ ok: true });
    expect(store.entries.size).toBe(1);
    expect(store.entries.get("ada@example.org")!.createdAt).toBe(new Date(T0).toISOString());
  });

  it("rejects malformed addresses", async () => {
    for (const email of ["", "ada", "ada@", "@example.org", "ada@example", "a da@example.org", "ada@@example.org", "ada..x@example.org", `${"a".repeat(250)}@example.org`, 42, null]) {
      const { store, deps } = setup();
      const res = await handleWaitlist(post({ email }), deps);
      expect(res.status, String(email)).toBe(400);
      expect(await res.json()).toEqual({ ok: false, error: "bad_email" });
      expect(store.entries.size).toBe(0);
    }
    expect(normalizeEmail("ada.lovelace+molty@mail.example.co.uk")).toBe("ada.lovelace+molty@mail.example.co.uk");
  });

  it("answers a filled honeypot like a signup and stores nothing", async () => {
    const { store, deps } = setup();
    const res = await handleWaitlist(post({ email: "bot@example.org", website: "http://spam.example" }), deps);
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({ ok: true });
    expect(store.entries.size).toBe(0);
    // an empty honeypot is a human
    await handleWaitlist(post({ email: "ada@example.org", website: "" }), deps);
    expect(store.entries.size).toBe(1);
  });

  it("rate limits per IP: 5 an hour, 429 with Retry-After, other IPs unaffected, reset next hour", async () => {
    const { store, deps, advance } = setup();
    for (let i = 0; i < WAITLIST_RATE.max; i++) expect((await handleWaitlist(post({ email: `a${i}@example.org` }), deps)).status).toBe(200);
    const limited = await handleWaitlist(post({ email: "late@example.org" }), deps);
    expect(limited.status).toBe(429);
    expect(await limited.json()).toEqual({ ok: false, error: "rate_limited" });
    expect(Number(limited.headers.get("retry-after"))).toBe(3600);
    expect(store.entries.has("late@example.org")).toBe(false);
    // bad requests count too (a bot cannot probe for free)
    expect((await handleWaitlist(post({ email: "x" }), deps)).status).toBe(429);
    // another IP has its own budget
    expect((await handleWaitlist(post({ email: "b@example.org" }), { ...deps, ip: "198.51.100.1" })).status).toBe(200);
    advance(WAITLIST_RATE.windowMs);
    expect((await handleWaitlist(post({ email: "late@example.org" }), deps)).status).toBe(200);
    // the store keeps a hash, never the IP itself
    for (const key of store.hits.keys()) expect(key).toMatch(/^[0-9a-f]{64}$/);
  });

  it("verifies the human check only when one is configured", async () => {
    const seen: Array<[string, string | undefined]> = [];
    const humanCheck = async (token: string, ip: string | undefined) => (seen.push([token, ip]), token === "good");
    const { store, deps } = setup({ humanCheck });
    expect((await handleWaitlist(post({ email: "a@example.org" }), deps)).status).toBe(403);
    expect((await handleWaitlist(post({ email: "a@example.org", turnstile: "bad" }), deps)).status).toBe(403);
    expect((await handleWaitlist(post({ email: "a@example.org", "cf-turnstile-response": "good" }), deps)).status).toBe(200);
    expect(seen).toEqual([
      ["bad", "203.0.113.7"],
      ["good", "203.0.113.7"],
    ]);
    expect(store.entries.size).toBe(1);
  });
});

describe("waitlist CORS", () => {
  it("answers the preflight for the site and local dev origins only", async () => {
    for (const origin of [SITE, "https://www.molty.uno", "http://localhost:4330", "http://127.0.0.1:4402"]) {
      const res = await handleWaitlist(new Request("https://relay.test/waitlist", { method: "OPTIONS", headers: { origin } }), setup().deps);
      expect(res.status, origin).toBe(204);
      expect(res.headers.get("access-control-allow-origin")).toBe(origin);
      expect(res.headers.get("access-control-allow-methods")).toContain("POST");
      expect(res.headers.get("access-control-allow-headers")).toContain("content-type");
    }
    for (const origin of ["https://evil.example", "https://molty.uno.evil.example", "http://molty.uno", "null"]) {
      const res = await handleWaitlist(new Request("https://relay.test/waitlist", { method: "OPTIONS", headers: { origin } }), setup().deps);
      expect(res.status, origin).toBe(403);
      expect(res.headers.get("access-control-allow-origin")).toBeNull();
    }
  });

  it("echoes an allowed origin on POST and refuses a foreign one without storing", async () => {
    const { store, deps } = setup();
    const ok = await handleWaitlist(post({ email: "a@example.org" }, { origin: "http://localhost:4330" }), deps);
    expect(ok.headers.get("access-control-allow-origin")).toBe("http://localhost:4330");
    const bad = await handleWaitlist(post({ email: "b@example.org" }, { origin: "https://evil.example" }), deps);
    expect(bad.status).toBe(403);
    expect(bad.headers.get("access-control-allow-origin")).toBeNull();
    expect([...store.entries.keys()]).toEqual(["a@example.org"]);
  });

  it("accepts requests without an Origin (scripts) and rejects other methods and media types", async () => {
    const { deps } = setup();
    expect((await handleWaitlist(post({ email: "a@example.org" }, { origin: null }), deps)).status).toBe(200);
    expect((await handleWaitlist(new Request("https://relay.test/waitlist", { method: "GET", headers: { origin: SITE } }), deps)).status).toBe(405);
    const text = new Request("https://relay.test/waitlist", { method: "POST", headers: { origin: SITE, "content-type": "text/plain" }, body: "email=a@example.org" });
    expect((await handleWaitlist(text, deps)).status).toBe(415);
  });
});

describe("waitlist without JavaScript (form POST)", () => {
  it("redirects back to the page with the outcome", async () => {
    const { store, deps } = setup();
    const res = await handleWaitlist(post({ email: "ada@example.org", website: "", source: "build", return: "/build/" }, { form: true }), deps);
    expect(res.status).toBe(303);
    expect(res.headers.get("location")).toBe(`${SITE}/build/?waitlist=ok#waitlist-ok`);
    expect(store.entries.has("ada@example.org")).toBe(true);
    const bad = await handleWaitlist(post({ email: "nope" }, { form: true, origin: "http://localhost:4402" }), deps);
    expect(bad.status).toBe(303);
    expect(bad.headers.get("location")).toBe("http://localhost:4402/build/?waitlist=error&reason=bad_email#waitlist-error");
  });

  it("never redirects off the site", async () => {
    const { deps } = setup();
    for (const ret of ["//evil.example/", "https://evil.example/", "/a/../\\evil"]) {
      const res = await handleWaitlist(post({ email: "ada@example.org", return: ret }, { form: true }), deps);
      expect(res.headers.get("location"), ret).toBe(`${SITE}/build/?waitlist=ok#waitlist-ok`);
    }
    const noOrigin = await handleWaitlist(post({ email: "b@example.org", return: "/" }, { form: true, origin: null }), deps);
    expect(noOrigin.headers.get("location")).toBe(`${SITE}/?waitlist=ok#waitlist-ok`);
  });
});
