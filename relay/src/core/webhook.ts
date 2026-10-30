// SPDX-License-Identifier: MIT
// Agent-defined webhooks: validation, per-event request rendering, signing and
// the SSRF guard. Host-neutral (Fetch API + Web Crypto only).
//
// An agent registers a request template; for each event the relay fills the
// placeholders and sends it. With no body template the body is the relay's own
// `{device, event}` JSON. Every request carries
//   Molty-Signature: t=<unix ms>,v1=<hex hmac_sha256(secret, t + "." + body)>
// Header values may hold the agent's own secrets: they are stored, used, and
// never echoed back (see redactWebhook) nor logged.

import { b64urlEncode, randomBytes } from "./crypto.js";
import { isRecord } from "./protocol.js";
import { type AgentEvent, eventSentence } from "./events.js";

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
const PLACEHOLDER_RE = /\{\{\s*([a-z_][a-z0-9_.]*)\s*\}\}/gi;
const KNOWN = /^(event\.(type|id|ts|data(\.[a-z_][a-z0-9_]*)?)|device|text)$/i;
const bytes = (s: string) => new TextEncoder().encode(s).byteLength;

export type Check<T> = { ok: true; value: T } | { ok: false; message: string };
const bad = (message: string): { ok: false; message: string } => ({ ok: false, message });

/** Validates the agent's raw arguments into a WebhookSpec (no network). */
export function parseWebhookSpec(raw: Record<string, unknown>): Check<WebhookSpec> {
  const { url, method = "POST", headers = {}, body, content_type } = raw;
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
  const m = method.toUpperCase() as WebhookMethod;
  if (m === "GET" && body !== undefined) return bad("GET takes no body; put placeholders in the url");
  for (const tpl of [url, body ?? ""]) {
    for (const [, name] of tpl.matchAll(PLACEHOLDER_RE)) if (!KNOWN.test(name)) return bad(`unknown placeholder {{${name}}}`);
  }
  const spec: WebhookSpec = { url, method: m, headers: out };
  if (body !== undefined) spec.body = body;
  if (content_type !== undefined) spec.content_type = content_type;
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
 * text (raw in a JSON body: write them unquoted). Missing fields are empty.
 */
export function renderTemplate(tpl: string, ev: AgentEvent, device: string, esc: Escape): string {
  return tpl.replace(PLACEHOLDER_RE, (_m, name: string) => {
    const key = name.toLowerCase();
    const str = (s: string) => esc(s);
    if (key === "device") return str(device);
    if (key === "text") return str(eventSentence(ev));
    if (key === "event.type") return str(ev.type);
    if (key === "event.id") return str(ev.id);
    if (key === "event.ts") return String(ev.ts);
    const data = ev.data as Record<string, unknown>;
    if (key === "event.data") return esc === jsonEsc ? JSON.stringify(data) : esc(JSON.stringify(data));
    const v = data[name.slice("event.data.".length)];
    if (v === undefined || v === null) return "";
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

export async function signatureHeader(secret: string, body: string, nowMs: number): Promise<string> {
  return `t=${nowMs},v1=${await hmacHex(secret, `${nowMs}.${body}`)}`;
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

/** True for addresses a webhook must never reach (private, loopback, link-local, CGNAT, multicast, reserved). */
export function isPrivateAddress(ip: string): boolean {
  const v4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(ip);
  if (v4) {
    const [a, b] = [Number(v4[1]), Number(v4[2])];
    return (
      a === 0 || a === 10 || a === 127 || a >= 224 ||
      (a === 100 && b >= 64 && b <= 127) ||
      (a === 169 && b === 254) ||
      (a === 172 && b >= 16 && b <= 31) ||
      (a === 192 && b === 168) ||
      (a === 192 && b === 0) ||
      (a === 198 && (b === 18 || b === 19))
    );
  }
  const s = ip.toLowerCase();
  if (!s.includes(":")) return true; // not an address we understand: refuse
  if (s === "::" || s === "::1") return true;
  const mapped = /^::ffff:(\d+\.\d+\.\d+\.\d+)$/.exec(s);
  if (mapped) return isPrivateAddress(mapped[1]);
  const first = parseInt(s.split(":")[0] || "0", 16);
  return (first & 0xfe00) === 0xfc00 || (first & 0xffc0) === 0xfe80 || (first & 0xff00) === 0xff00 || s.startsWith("64:ff9b:") || s.startsWith("2001:db8:");
}

export type Resolver = (host: string) => Promise<string[]>;

/** Full guard: literal checks, then every resolved address must be public. */
export async function guardWebhookUrl(raw: string, resolve?: Resolver): Promise<Check<URL>> {
  const lit = checkWebhookUrl(raw);
  if (!lit.ok || !resolve) return lit;
  let addrs: string[];
  try {
    addrs = await resolve(lit.value.hostname);
  } catch {
    return bad("url host did not resolve");
  }
  if (addrs.length === 0) return bad("url host did not resolve");
  if (addrs.some(isPrivateAddress)) return bad("url host resolves to a private or reserved address");
  return lit;
}

/** DNS-over-HTTPS resolver (A + AAAA) for hosts without a socket-level resolver, e.g. Workers. */
export function dohResolver(fetcher: (req: Request) => Promise<Response>, endpoint = "https://cloudflare-dns.com/dns-query"): Resolver {
  return async (host) => {
    const out: string[] = [];
    for (const type of ["A", "AAAA"]) {
      const res = await fetcher(new Request(`${endpoint}?name=${encodeURIComponent(host)}&type=${type}`, { headers: { accept: "application/dns-json" } }));
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
  headers["molty-signature"] = "t=<unix ms>,v1=<hmac>";
  return { method: r.method, url: redactUrl(r.url), headers, body: r.body };
}
