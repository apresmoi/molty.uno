// SPDX-License-Identifier: MIT
// POST /waitlist: the website's "want one built?" form. Fetch-API only.
//
//   POST    /waitlist  JSON {email, website?, source?, turnstile?}        -> {ok:true} | {ok:false,error}
//                      form-encoded, same fields (+ return, cf-turnstile-response)
//                                                                         -> 303 back to the page
//   OPTIONS /waitlist  CORS preflight
//
// `website` is a honeypot: humans never see it, so a filled one is a bot. It gets the
// same answer as a signup and nothing is stored. A new address, a known one and a bot
// all read {ok:true}: the answer never tells whether an address is on the list.
// Without JavaScript the page posts the form and lands back on
// `<origin><return>?waitlist=ok#waitlist-ok` (or `?waitlist=error&reason=…#waitlist-error`).
// Deleting an address (GDPR) is an admin command, not an endpoint: `make waitlist-delete`.

import { sha256Hex } from "../core/crypto.js";
import { isRecord } from "../core/protocol.js";
import {
  type HumanCheck,
  type WaitlistStore,
  WAITLIST_RATE,
  normalizeEmail,
  normalizeSource,
  windowStart,
} from "../core/waitlist.js";

export const WAITLIST_PATH = "/waitlist";
export const SITE_ORIGIN = "https://molty.uno";
const ALLOWED_ORIGIN = /^(?:https:\/\/(?:www\.)?molty\.uno|http:\/\/(?:localhost|127\.0\.0\.1)(?::\d{1,5})?)$/;
const RETURN_RE = /^\/[a-z0-9/_-]{0,64}$/i;
const BODY_MAX = 8 * 1024;

export interface WaitlistDeps {
  store: WaitlistStore;
  /** client IP as the host sees it (the rate-limit key, hashed before it is stored) */
  ip?: string;
  /** set when a human check is configured (a Turnstile secret); absent = skipped */
  humanCheck?: HumanCheck;
  now?: () => number;
}

export const isAllowedOrigin = (origin: string | null): origin is string => !!origin && ALLOWED_ORIGIN.test(origin);

function corsHeaders(origin: string | null): Record<string, string> {
  return isAllowedOrigin(origin) ? { "access-control-allow-origin": origin, vary: "origin" } : { vary: "origin" };
}

type Fields = Record<string, unknown>;

async function readFields(request: Request, form: boolean): Promise<Fields | undefined> {
  const text = await request.text();
  if (text.length > BODY_MAX) return undefined;
  if (form) return Object.fromEntries(new URLSearchParams(text));
  try {
    const v: unknown = text ? JSON.parse(text) : {};
    return isRecord(v) ? v : undefined;
  } catch {
    return undefined;
  }
}

export async function handleWaitlist(request: Request, deps: WaitlistDeps): Promise<Response> {
  const origin = request.headers.get("origin");
  const cors = corsHeaders(origin);

  if (request.method === "OPTIONS") {
    if (!isAllowedOrigin(origin)) return new Response(null, { status: 403, headers: cors });
    return new Response(null, {
      status: 204,
      headers: {
        ...cors,
        "access-control-allow-methods": "POST, OPTIONS",
        "access-control-allow-headers": "content-type",
        "access-control-max-age": "86400",
      },
    });
  }

  const json = (body: Record<string, unknown>, status = 200, extra: Record<string, string> = {}) =>
    new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json", ...cors, ...extra } });

  if (request.method !== "POST") return json({ ok: false, error: "method_not_allowed" }, 405, { allow: "POST, OPTIONS" });
  // Browsers always send Origin on a cross-site POST: only the site's own pages may sign
  // people up. No Origin (curl, scripts) is allowed and still rate limited.
  if (origin !== null && !isAllowedOrigin(origin)) return json({ ok: false, error: "forbidden_origin" }, 403);

  const type = (request.headers.get("content-type") ?? "").split(";")[0].trim().toLowerCase();
  const form = type === "application/x-www-form-urlencoded";
  if (!form && type !== "application/json") return json({ ok: false, error: "unsupported_media_type" }, 415);

  const fields = await readFields(request, form);
  const back = (outcome: "ok" | { error: string }) => {
    const ret = typeof fields?.return === "string" && RETURN_RE.test(fields.return) && !fields.return.includes("//") ? fields.return : "/build/";
    const base = isAllowedOrigin(origin) ? origin : SITE_ORIGIN;
    const query = outcome === "ok" ? "?waitlist=ok#waitlist-ok" : `?waitlist=error&reason=${encodeURIComponent(outcome.error)}#waitlist-error`;
    return new Response(null, { status: 303, headers: { location: `${base}${ret}${query}`, ...cors } });
  };
  const fail = (error: string, status: number, extra: Record<string, string> = {}) => (form ? back({ error }) : json({ ok: false, error }, status, extra));
  const ok = () => (form ? back("ok") : json({ ok: true }));

  if (!fields) return fail("bad_request", 400);

  const now = (deps.now ?? Date.now)();
  const start = windowStart(now);
  const key = await sha256Hex(`waitlist:${deps.ip ?? "unknown"}`);
  let hits: number;
  try {
    hits = await deps.store.hit(key, start);
  } catch {
    return fail("unavailable", 503);
  }
  if (hits > WAITLIST_RATE.max) {
    const retry = Math.max(1, Math.ceil((start + WAITLIST_RATE.windowMs - now) / 1000));
    return fail("rate_limited", 429, { "retry-after": String(retry) });
  }

  // honeypot: answer like a signup, store nothing
  if (fields.website !== undefined && fields.website !== null && String(fields.website).trim() !== "") return ok();

  if (deps.humanCheck) {
    const token = fields["cf-turnstile-response"] ?? fields.turnstile;
    if (typeof token !== "string" || !token || !(await deps.humanCheck(token, deps.ip))) return fail("human_check_failed", 403);
  }

  const email = normalizeEmail(fields.email);
  if (!email) return fail("bad_email", 400);

  try {
    await deps.store.add({ email, createdAt: new Date(now).toISOString(), source: normalizeSource(fields.source) });
  } catch {
    return fail("unavailable", 503);
  }
  return ok();
}
