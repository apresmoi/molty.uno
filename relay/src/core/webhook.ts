// SPDX-License-Identifier: MIT
// Agent-defined webhooks: validation, per-event request rendering, signing and
// the SSRF guard. Host-neutral (Fetch API + Web Crypto only).
//
// An agent registers a request template; for each event the relay fills the
// placeholders and sends it. With no body template the body is the relay's own
// `{device, event}` JSON. Every request carries
//   Molty-Signature: t=<unix ms>,v2=<hex hmac_sha256(secret, t \n METHOD \n path?query \n body)>
// (v2 covers the URL, where placeholders may carry event data).
// Header values may hold the agent's own secrets: they are stored, used, and
// never echoed back (see redactWebhook) nor logged.

import { b64urlEncode, randomBytes } from "./crypto.js";
import { isRecord } from "./protocol.js";
import { AGENT_EVENT_TYPES, type AgentEvent, SAMPLE_EVENTS, eventSentence } from "./events.js";

export const WEBHOOK_METHODS = ["POST", "GET", "PUT", "PATCH"] as const;
export type WebhookMethod = (typeof WEBHOOK_METHODS)[number];

export const WEBHOOK_LIMITS = {
  urlBytes: 2048,
  bodyBytes: 4096,
  headers: 16,
  headerBytes: 4096,
  timeoutMs: 5000,
  /** Retry delays after a failed attempt; a delivery is tried 1 + length times. */
  retryMs: [1000, 10000, 60000] as readonly number[],
  /** Consecutive failed attempts before the webhook is disabled. */
  maxFails: 20,
};

export interface WebhookSpec {
  url: string;
  method: WebhookMethod;
  headers: Record<string, string>;
  body?: string;
  content_type?: string;
  /** Event types this webhook wants; absent = all agent event types. */
  events?: string[];
}

export interface WebhookRecord extends WebhookSpec {
  secret: string;
  created: number;
  fails: number;
  disabled?: string;
  last_ok?: number;
  last_error?: string;
}

/** Headers the agent may not set: framing, routing and our own signature. */
const RESERVED_HEADERS = new Set(["host", "content-length", "transfer-encoding", "connection", "upgrade", "te", "trailer", "keep-alive", "proxy-authorization", "molty-signature", "cf-connecting-ip", "x-forwarded-for", "x-real-ip"]);
const HEADER_NAME_RE = /^[!#$%&'*+.^_`|~0-9A-Za-z-]{1,64}$/;
/** `{{name}}` or `{{name|default}}`; the default is literal text (escaped like a string value). */
const PLACEHOLDER_RE = /\{\{\s*([a-z_][a-z0-9_.]*)(?:\|([^{}|]{0,64}))?\s*\}\}/gi;
const KNOWN = /^(event\.(type|id|ts|data(\.[a-z_][a-z0-9_]*)?)|device|text)$/i;
const bytes = (s: string) => new TextEncoder().encode(s).byteLength;

export type Check<T> = { ok: true; value: T } | { ok: false; message: string };
const bad = (message: string): { ok: false; message: string } => ({ ok: false, message });

/** Validates the agent's raw arguments into a WebhookSpec (no network). */
export function parseWebhookSpec(raw: Record<string, unknown>): Check<WebhookSpec> {
  const { url, method = "POST", headers = {}, body, content_type, events } = raw;
  if (typeof url !== "string" || !url) return bad("url is required");
  if (bytes(url) > WEBHOOK_LIMITS.urlBytes) return bad(`url exceeds ${WEBHOOK_LIMITS.urlBytes} bytes`);
  if (typeof method !== "string" || !(WEBHOOK_METHODS as readonly string[]).includes(method.toUpperCase())) return bad(`method must be one of ${WEBHOOK_METHODS.join(", ")}`);
  if (!isRecord(headers)) return bad("headers must be an object of strings");
  const entries = Object.entries(headers);
  if (entries.length > WEBHOOK_LIMITS.headers) return bad(`at most ${WEBHOOK_LIMITS.headers} headers`);
  let hb = 0;
  const out: Record<string, string> = {};
  for (const [k, v] of entries) {
    if (!HEADER_NAME_RE.test(k)) return bad(`bad header name ${JSON.stringify(k.slice(0, 64))}`);
    const lower = k.toLowerCase();
    if (RESERVED_HEADERS.has(lower)) return bad(`header ${k} is reserved`);
    if (lower === "content-type") return bad("use content_type instead of a Content-Type header");
    if (typeof v !== "string" || /[\r\n\0]/.test(v)) return bad(`header ${k} must be a single-line string`);
    hb += bytes(k) + bytes(v);
    out[k] = v;
  }
  if (hb > WEBHOOK_LIMITS.headerBytes) return bad(`headers exceed ${WEBHOOK_LIMITS.headerBytes} bytes`);
  if (body !== undefined && (typeof body !== "string" || bytes(body) > WEBHOOK_LIMITS.bodyBytes)) return bad(`body must be a string of at most ${WEBHOOK_LIMITS.bodyBytes} bytes`);
  if (content_type !== undefined && (typeof content_type !== "string" || !/^[\w.+-]+\/[\w.+-]+(\s*;\s*[\w-]+=[\w."-]+)*$/.test(content_type) || content_type.length > 128)) {
    return bad("content_type must be a media type, e.g. application/json");
  }
  if (events !== undefined) {
    if (!Array.isArray(events) || events.length === 0 || !events.every((e) => typeof e === "string" && AGENT_EVENT_TYPES.includes(e))) {
      return bad(`events must be a non-empty list of ${AGENT_EVENT_TYPES.join(", ")}`);
    }
  }
  const m = method.toUpperCase() as WebhookMethod;
  if (m === "GET" && body !== undefined) return bad("GET takes no body; put placeholders in the url");
  for (const tpl of [url, body ?? ""]) {
    for (const [, name] of tpl.matchAll(PLACEHOLDER_RE)) if (!KNOWN.test(name)) return bad(`unknown placeholder {{${name}}}`);
  }
  const spec: WebhookSpec = { url, method: m, headers: out };
  if (body !== undefined) spec.body = body;
  if (content_type !== undefined) spec.content_type = content_type;
  if (events !== undefined) spec.events = [...new Set(events as string[])];
  return { ok: true, value: spec };
}

export const newWebhookSecret = () => `whsec_${b64urlEncode(randomBytes(24))}`;

type Escape = (s: string) => string;
const jsonEsc: Escape = (s) => JSON.stringify(s).slice(1, -1);
const urlEsc: Escape = (s) => encodeURIComponent(s);
const rawEsc: Escape = (s) => s;
export const ESCAPE = { json: jsonEsc, url: urlEsc, raw: rawEsc } as const;

function escaperFor(contentType: string | undefined): Escape {
  const ct = (contentType ?? "").toLowerCase();
  if (/[/+]json\b/.test(ct)) return jsonEsc;
  if (ct.startsWith("application/x-www-form-urlencoded")) return urlEsc;
  return rawEsc;
}

/**
 * Fills placeholders. Strings are escaped for the target (JSON string body,
 * form body, URL). `{{event.data}}` and non-string data fields insert JSON
 * text (raw in a JSON body: write them unquoted). A missing field renders its
 * `|default` (escaped like a string), else `null` in a JSON body and empty
 * elsewhere -- so an unquoted numeric placeholder stays valid JSON for every
 * event type.
 */
export function renderTemplate(tpl: string, ev: AgentEvent, device: string, esc: Escape): string {
  return tpl.replace(PLACEHOLDER_RE, (_m, name: string, dflt: string | undefined) => {
    const key = name.toLowerCase();
    const str = (s: string) => esc(s);
    if (key === "device") return str(device);
    if (key === "text") return str(eventSentence(ev));
    if (key === "event.type") return str(ev.type);
    if (key === "event.id") return str(ev.id);
    if (key === "event.ts") return String(ev.ts);
    const data = ev.data as Record<string, unknown>;
    if (key === "event.data") return esc === jsonEsc ? JSON.stringify(data) : esc(JSON.stringify(data));
    const field = name.slice("event.data.".length);
    const v = Object.hasOwn(data, field) ? data[field] : undefined;
    if (v === undefined || v === null) return dflt !== undefined ? str(dflt) : esc === jsonEsc ? "null" : "";
    if (typeof v === "string") return str(v);
    return esc === jsonEsc ? JSON.stringify(v) : esc(JSON.stringify(v));
  });
}

export interface RenderedRequest {
  url: string;
  method: WebhookMethod;
  headers: Record<string, string>;
  body: string;
}

export function defaultBody(device: string, ev: AgentEvent): string {
  return JSON.stringify({ device, event: ev });
}

/** Builds the unsigned request for one event. Throws on a template that renders to an invalid URL/JSON. */
export function renderRequest(spec: WebhookSpec, device: string, ev: AgentEvent): RenderedRequest {
  const url = renderTemplate(spec.url, ev, device, urlEsc);
  const headers: Record<string, string> = { ...spec.headers, "user-agent": "molty-relay-webhook" };
  let body = "";
  if (spec.method !== "GET") {
    if (spec.body === undefined) {
      body = defaultBody(device, ev);
      headers["content-type"] = spec.content_type ?? "application/json";
    } else {
      body = renderTemplate(spec.body, ev, device, escaperFor(spec.content_type));
      headers["content-type"] = spec.content_type ?? "text/plain; charset=utf-8";
      if (escaperFor(spec.content_type) === jsonEsc) {
        try {
          JSON.parse(body);
        } catch {
          throw new Error("body template does not render to valid JSON (write {{event.data}} and numeric fields unquoted)");
        }
      }
    }
  }
  return { url, method: spec.method, headers, body };
}

export async function hmacHex(secret: string, message: string): Promise<string> {
  const enc = new TextEncoder();
  const key = await crypto.subtle.importKey("raw", enc.encode(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const sig = new Uint8Array(await crypto.subtle.sign("HMAC", key, enc.encode(message)));
  return Array.from(sig, (b) => b.toString(16).padStart(2, "0")).join("");
}

/**
 * Signature v2 canonical string: t, METHOD, request target (path + query as
 * sent) and body, joined by "\n" (a URL target cannot contain a raw newline,
 * so the split is unambiguous). Covers the URL, where placeholders put event
 * data for GET (and any method).
 */
export function signedPayload(nowMs: number, method: string, url: string, body: string): string {
  const u = new URL(url);
  return `${nowMs}\n${method.toUpperCase()}\n${u.pathname}${u.search}\n${body}`;
}

export async function signatureHeader(secret: string, method: string, url: string, body: string, nowMs: number): Promise<string> {
  return `t=${nowMs},v2=${await hmacHex(secret, signedPayload(nowMs, method, url, body))}`;
}

/** Fixed failure categories: what is stored, logged and shown. Never raw exception text. */
export type DeliveryError = "timeout" | "dns_failed" | "blocked_address" | "blocked_url" | "render_failed" | "network_error" | `http_${number}`;

export class WebhookFailure extends Error {
  constructor(readonly category: DeliveryError) {
    super(category);
  }
}

/** Maps anything thrown during a delivery to a category; unknown errors are `network_error`. */
export function categorize(e: unknown): DeliveryError {
  return e instanceof WebhookFailure ? e.category : "network_error";
}

// ---------------------------------------------------------------- SSRF guard

const BLOCKED_SUFFIXES = [".local", ".localhost", ".internal", ".lan", ".home", ".home.arpa", ".corp", ".intranet", ".localdomain"];

/** Literal checks on the URL itself (no DNS). Returns the hostname when acceptable. */
export function checkWebhookUrl(raw: string): Check<URL> {
  let u: URL;
  try {
    u = new URL(raw);
  } catch {
    return bad("url is not a valid absolute URL");
  }
  if (u.protocol !== "https:") return bad("url must be https");
  if (u.username || u.password) return bad("url must not carry credentials; use headers");
  const host = u.hostname.toLowerCase().replace(/\.$/, "");
  if (!host) return bad("url has no host");
  if (host.startsWith("[") || /^[0-9.]+$/.test(host) || /^0x/i.test(host) || isIpLiteral(host)) return bad("url host must be a DNS name, not an IP address");
  if (!host.includes(".")) return bad("url host must be a public DNS name");
  if (host === "localhost" || BLOCKED_SUFFIXES.some((s) => host.endsWith(s))) return bad("url host is not public");
  return { ok: true, value: u };
}

function isIpLiteral(host: string): boolean {
  return /^[0-9a-f:]+$/i.test(host) && host.includes(":");
}

/** Strict dotted-quad IPv4 (no leading zeros, no short forms). */
export function parseIPv4(s: string): number[] | undefined {
  const parts = s.split(".");
  if (parts.length !== 4) return undefined;
  const out: number[] = [];
  for (const p of parts) {
    if (!/^(0|[1-9][0-9]{0,2})$/.test(p) || Number(p) > 255) return undefined;
    out.push(Number(p));
  }
  return out;
}

/** IPv6 text (any compression, optional trailing dotted IPv4) to 16 bytes. Zone ids and malformed text: undefined. */
export function parseIPv6(input: string): number[] | undefined {
  let s = input.toLowerCase();
  if (!s.includes(":") || s.includes("%")) return undefined;
  const last = s.lastIndexOf(":");
  if (s.slice(last + 1).includes(".")) {
    const v4 = parseIPv4(s.slice(last + 1));
    if (!v4) return undefined;
    s = `${s.slice(0, last + 1)}${((v4[0] << 8) | v4[1]).toString(16)}:${((v4[2] << 8) | v4[3]).toString(16)}`;
  }
  const halves = s.split("::");
  if (halves.length > 2) return undefined;
  const head = halves[0] ? halves[0].split(":") : [];
  const tail = halves.length === 2 && halves[1] ? halves[1].split(":") : [];
  if (halves.length === 1 ? head.length !== 8 : head.length + tail.length > 7) return undefined;
  const groups = [...head, ...Array(8 - head.length - tail.length).fill("0"), ...tail];
  const out: number[] = [];
  for (const g of groups) {
    if (!/^[0-9a-f]{1,4}$/.test(g)) return undefined;
    const n = parseInt(g, 16);
    out.push(n >> 8, n & 0xff);
  }
  return out;
}

function privateV4([a, b, c]: number[]): boolean {
  return (
    a === 0 || a === 10 || a === 127 || a >= 224 || // this-net, private, loopback, multicast + reserved + broadcast
    (a === 100 && b >= 64 && b <= 127) || // CGNAT
    (a === 169 && b === 254) || // link-local
    (a === 172 && b >= 16 && b <= 31) ||
    (a === 192 && b === 168) ||
    (a === 192 && b === 0 && (c === 0 || c === 2)) || // IETF assignments, TEST-NET-1
    (a === 192 && b === 88 && c === 99) || // 6to4 relay anycast
    (a === 198 && (b === 18 || b === 19)) || // benchmarking
    (a === 198 && b === 51 && c === 100) ||
    (a === 203 && b === 0 && c === 113)
  );
}

const zeros = (b: number[], from: number, to: number) => b.slice(from, to).every((x) => x === 0);

function privateV6(b: number[]): boolean {
  if (zeros(b, 0, 10) && b[10] === 0xff && b[11] === 0xff) return privateV4(b.slice(12)); // ::ffff:a.b.c.d mapped
  if (zeros(b, 0, 12)) return true; // ::, ::1, deprecated IPv4-compatible ::a.b.c.d
  if (b[0] === 0x00 && b[1] === 0x64 && b[2] === 0xff && b[3] === 0x9b) {
    // NAT64: well-known 64:ff9b::/96 maps an embedded IPv4; 64:ff9b:1::/48 is local-use
    return zeros(b, 4, 12) ? privateV4(b.slice(12)) : true;
  }
  if ((b[0] & 0xe0) !== 0x20) return true; // only global unicast 2000::/3 is acceptable
  if (b[0] === 0x20 && b[1] === 0x02) return privateV4(b.slice(2, 6)); // 6to4 embeds the IPv4
  if (b[0] === 0x20 && b[1] === 0x01 && b[2] < 0x02) return true; // 2001::/23 IETF protocol assignments (Teredo, ORCHID, benchmarking)
  if (b[0] === 0x20 && b[1] === 0x01 && b[2] === 0x0d && b[3] === 0xb8) return true; // documentation
  if (b[0] === 0x3f && b[1] === 0xff && b[2] < 0x10) return true; // 3fff::/20 documentation
  return false;
}

/**
 * True for addresses a webhook must never reach: private, loopback,
 * link-local, CGNAT, multicast, documentation and reserved ranges in either
 * family, including IPv4 embedded in IPv6 (mapped, compatible, NAT64, 6to4)
 * in any textual form. Anything that does not parse is refused.
 */
export function isPrivateAddress(ip: string): boolean {
  const v4 = parseIPv4(ip);
  if (v4) return privateV4(v4);
  const v6 = parseIPv6(ip);
  return v6 ? privateV6(v6) : true;
}

/** Resolves a host to addresses. Must honour `signal` (the delivery deadline). */
export type Resolver = (host: string, signal?: AbortSignal) => Promise<string[]>;

export type GuardResult = { ok: true; value: URL } | { ok: false; message: string; category: DeliveryError };

/** Full guard: literal checks, then every resolved address must be public. */
export async function guardWebhookUrl(raw: string, resolve?: Resolver, signal?: AbortSignal): Promise<GuardResult> {
  const lit = checkWebhookUrl(raw);
  if (!lit.ok) return { ...lit, category: "blocked_url" };
  if (!resolve) return lit;
  let addrs: string[];
  try {
    addrs = await resolve(lit.value.hostname, signal);
  } catch {
    if (signal?.aborted) return { ok: false, message: "url host did not resolve in time", category: "timeout" };
    return { ok: false, message: "url host did not resolve", category: "dns_failed" };
  }
  if (addrs.length === 0) return { ok: false, message: "url host did not resolve", category: "dns_failed" };
  if (addrs.some(isPrivateAddress)) return { ok: false, message: "url host resolves to a private or reserved address", category: "blocked_address" };
  return lit;
}

/** DNS-over-HTTPS resolver (A + AAAA) for hosts without a socket-level resolver, e.g. Workers. */
export function dohResolver(fetcher: (req: Request) => Promise<Response>, endpoint = "https://cloudflare-dns.com/dns-query"): Resolver {
  return async (host, signal) => {
    const out: string[] = [];
    for (const type of ["A", "AAAA"]) {
      const res = await fetcher(new Request(`${endpoint}?name=${encodeURIComponent(host)}&type=${type}`, { headers: { accept: "application/dns-json" }, signal }));
      if (!res.ok) throw new Error(`doh ${res.status}`);
      const body = (await res.json()) as { Answer?: Array<{ type: number; data: string }> };
      for (const a of body.Answer ?? []) if (a.type === 1 || a.type === 28) out.push(a.data);
    }
    return out;
  };
}

// ---------------------------------------------------------------- redaction

/** Origin-only URL: paths and queries often carry bot tokens. */
export function redactUrl(raw: string): string {
  try {
    const u = new URL(raw);
    return u.pathname === "/" && !u.search ? u.origin : `${u.origin}/…`;
  } catch {
    return "…";
  }
}

/** What an agent (or a log) may see of a stored webhook: never header values, the secret or the full URL. */
export function redactWebhook(w: WebhookRecord) {
  return {
    url: redactUrl(w.url),
    method: w.method,
    headers: Object.keys(w.headers),
    content_type: w.content_type ?? (w.body === undefined ? "application/json" : "text/plain; charset=utf-8"),
    custom_body: w.body !== undefined,
    events: w.events ?? AGENT_EVENT_TYPES,
    active: !w.disabled,
    consecutive_failures: w.fails,
    ...(w.disabled ? { disabled: w.disabled } : {}),
    ...(w.last_ok ? { last_ok_at: w.last_ok } : {}),
    ...(w.last_error ? { last_error: w.last_error } : {}),
  };
}

/** A rendered request with secrets removed, for the registration dry-run. */
export function redactRendered(r: RenderedRequest) {
  const headers: Record<string, string> = {};
  for (const [k, v] of Object.entries(r.headers)) headers[k] = k === "content-type" || k === "user-agent" ? v : "[redacted]";
  headers["molty-signature"] = "t=<unix ms>,v2=<hmac>";
  return { method: r.method, url: redactUrl(r.url), headers, body: r.body };
}
