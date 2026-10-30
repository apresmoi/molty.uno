// REST surface (docs/AGENT-LINK.md "REST envelopes"). Fetch-API only.
//
//   POST   /v1/pair                     {device_id, link_code, agent_name}  (no token; long-polls <= 120 s)
//   POST   /v1/devices/<id>/cmd         {name, args, wait?}   -> {ok:true,result} | {ok:false,error}
//                                       (say/listen + wait:true: blocks <= 24 s -> result {job, status:"done", ...} | 202 {job, status:"pending"})
//   POST   /v1/devices/<id>/talk        {say?, listen?, listen_ms?, voice?, end_silence_ms?, no_speech_ms?, vad?}
//                                       -> {ok:true, result:{job, status:"done", heard, said?, reason, ms, timings}} | 202 pending
//   POST   /v1/devices/<id>/ask         {say?, options[1..5], timeout_s?, voice?}
//                                       -> {ok:true, result:{job, status:"done", answer, index, reason?, said?, ms, timings}} | 202 pending
//   GET    /v1/jobs/<job>[?wait_s=0..8] -> {job, kind, status, result?, error?}
//   DELETE /v1/token                    revoke the calling token
//   GET    /v1/devices/<id>/events[?since=e41&wait_s=0..25] -> {ok, events, next, more}
//   GET|PUT|DELETE /v1/webhook          read / register (secret once) / remove the caller's webhook
//
// Any authenticated response may carry the caller's unread device events as an
// additive top-level `events` array (+ `more_events`); reading them marks them read.
// Likewise, once per agent per skill version, an additive top-level
// `skill_update: {version, whats_new, url}` (core/version.ts).

import { type Inbox, type JobView, type Outcome, type RelayBackend, bearer, jobDevice, tokenDevice } from "../core/api.js";
import { TALK_BUDGET_MS } from "../core/commands.js";
import { INBOX_LIMITS, seqOf } from "../core/events.js";
import { DEVICE_ID_RE, isRecord } from "../core/protocol.js";
import { skillUpdate } from "../core/version.js";

const STATUS: Record<string, number> = {
  unauthorized: 401,
  invalid_args: 400,
  unknown_command: 400,
  bad_request: 400,
  not_found: 404,
  bad_link_code: 403,
  rejected: 403,
  timeout: 408,
  device_offline: 409,
  audio_busy: 409,
  unsupported: 409,
  pair_busy: 409,
  device_timeout: 504,
  playback_timeout: 504,
  too_many_polls: 429,
  conflict: 409,
  cancelled: 499,
  internal: 500,
};

const json = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } });

const inboxExtra = (inbox?: Inbox) => (inbox && inbox.events.length ? { events: inbox.events, ...(inbox.more ? { more_events: true } : {}) } : {});

function failure(out: { error: string; message?: string; inbox?: Inbox }, rideAlong: Record<string, unknown> = {}) {
  // Unknown codes are device-reported errors (e.g. mic_busy): 502.
  return json({ ok: false, error: out.error, ...(out.message ? { message: out.message } : {}), ...inboxExtra(out.inbox), ...rideAlong }, STATUS[out.error] ?? 502);
}

async function body(request: Request): Promise<Record<string, unknown> | undefined> {
  const text = await request.text();
  if (text.length > 64 * 1024) return undefined;
  try {
    const v: unknown = text ? JSON.parse(text) : {};
    return isRecord(v) ? v : undefined;
  } catch {
    return undefined;
  }
}

/** Two outcomes' ride-alongs as one (events concatenated, the later notice). */
function merged(a: Pick<Outcome, "inbox" | "notice">, b: Pick<Outcome, "inbox" | "notice">): Pick<Outcome, "inbox" | "notice"> {
  const events = [...(a.inbox?.events ?? []), ...(b.inbox?.events ?? [])];
  const inbox: Inbox | undefined = events.length ? { events, more: !!(a.inbox?.more || b.inbox?.more), now: Math.max(a.inbox?.now ?? 0, b.inbox?.now ?? 0) } : undefined;
  return { inbox, notice: b.notice ?? a.notice };
}

/**
 * A waited job as a REST reply: done -> 200 result, failed -> error, still running -> 202 pending.
 * `ride` is what the starting call already consumed (events marked read, skill notice marked
 * seen): it reaches the caller on EVERY outcome, failures included, or it is lost for good.
 */
function jobResponse(
  out: Outcome<JobView>,
  ride: Pick<Outcome, "inbox" | "notice">,
  extra: (o: Pick<Outcome, "inbox" | "notice">) => Record<string, unknown>,
): Response {
  const all = extra(merged(ride, out));
  if (!out.ok) return failure({ error: out.error, message: out.message }, all);
  const j = out.result;
  if (j.status === "failed") return failure({ error: j.error ?? "failed", message: `job ${j.job} failed` }, all);
  if (j.status === "done") return json({ ok: true, result: { ...(j.result ?? {}), job: j.job, status: "done" }, ...all });
  return json({ ok: true, result: { job: j.job, status: "pending", state: j.status }, ...all }, 202);
}

export async function handleRest(request: Request, backend: RelayBackend): Promise<Response> {
  const url = new URL(request.url);
  const path = url.pathname;
  const token = bearer(request.headers.get("authorization"));
  const extra = (out: Pick<Outcome, "inbox" | "notice">) => ({ ...inboxExtra(out.inbox), ...(out.notice ? { skill_update: skillUpdate(out.notice, url.origin) } : {}) });
  // A failed outcome renders its ride-alongs too (events already marked read, skill notice
  // already marked seen), like MCP's toolResult: dropping them here would lose them for good.
  const failed = (out: { error: string; message?: string } & Pick<Outcome, "inbox" | "notice">) => failure(out, extra(out));

  if (path === "/v1/pair") {
    if (request.method !== "POST") return json({ ok: false, error: "method_not_allowed" }, 405);
    const b = await body(request);
    if (!b) return failure({ error: "bad_request", message: "body must be a JSON object" });
    if (typeof b.device_id !== "string" || !DEVICE_ID_RE.test(b.device_id)) return failure({ error: "invalid_args", message: "device_id must look like molty-xxxxxxxxxx" });
    let waitMs: number | undefined;
    if (b.wait_s !== undefined) {
      if (!Number.isInteger(b.wait_s) || (b.wait_s as number) < 0 || (b.wait_s as number) > 120) return failure({ error: "invalid_args", message: "wait_s must be 0..120" });
      waitMs = (b.wait_s as number) * 1000;
    }
    const out = await backend.device(b.device_id).pair({
      device_id: b.device_id,
      link_code: b.link_code as string | undefined,
      agent_name: b.agent_name as string | undefined,
      pair_id: b.pair_id as string | undefined,
      wait_ms: waitMs,
    });
    if (!out.ok) return failed(out);
    if (out.result.status === "approved") {
      const { agent_token, device_id, agent_name, code } = out.result;
      return json({ ok: true, agent_token, device_id, agent_name, code });
    }
    return json({ ok: true, ...out.result }, 202);
  }

  const cmd = /^\/v1\/devices\/([^/]+)\/cmd$/.exec(path);
  if (cmd) {
    if (request.method !== "POST") return json({ ok: false, error: "method_not_allowed" }, 405);
    const id = decodeURIComponent(cmd[1]);
    if (!DEVICE_ID_RE.test(id)) return failure({ error: "not_found" });
    if (tokenDevice(token) !== id) return failure({ error: "unauthorized" });
    const b = await body(request);
    if (!b) return failure({ error: "bad_request", message: "body must be a JSON object" });
    if (b.wait !== undefined && typeof b.wait !== "boolean") return failure({ error: "invalid_args", message: "wait must be a boolean" });
    const api = backend.device(id);
    const t0 = Date.now();
    const out: Outcome = await api.command(token!, b.name as string, b.args);
    if (!out.ok) return failed(out);
    if (b.wait !== true || (b.name !== "say" && b.name !== "listen")) return json({ ok: true, result: out.result, ...extra(out) });
    // Block on the job. Both outcomes may carry ride-alongs (already marked read): hand over both,
    // whatever the job's outcome.
    const waited = await api.job(token!, out.result.job as string, Math.max(0, TALK_BUDGET_MS - (Date.now() - t0)));
    return jobResponse(waited, out, extra);
  }

  const turn = /^\/v1\/devices\/([^/]+)\/(talk|ask)$/.exec(path);
  if (turn) {
    if (request.method !== "POST") return json({ ok: false, error: "method_not_allowed" }, 405);
    const id = decodeURIComponent(turn[1]);
    if (!DEVICE_ID_RE.test(id)) return failure({ error: "not_found" });
    if (tokenDevice(token) !== id) return failure({ error: "unauthorized" });
    const b = await body(request);
    if (!b) return failure({ error: "bad_request", message: "body must be a JSON object" });
    const api = backend.device(id);
    const out = await (turn[2] === "ask" ? api.ask(token!, b) : api.talk(token!, b));
    if (!out.ok) return failed(out);
    return json({ ok: true, result: out.result, ...extra(out) }, out.result.status === "pending" ? 202 : 200);
  }

  const events = /^\/v1\/devices\/([^/]+)\/events$/.exec(path);
  if (events) {
    if (request.method !== "GET") return json({ ok: false, error: "method_not_allowed" }, 405);
    const id = decodeURIComponent(events[1]);
    if (!DEVICE_ID_RE.test(id)) return failure({ error: "not_found" });
    if (tokenDevice(token) !== id) return failure({ error: "unauthorized" });
    const w = url.searchParams.get("wait_s");
    const waitS = w === null ? 0 : Number(w);
    if (!Number.isInteger(waitS) || waitS < 0 || waitS > INBOX_LIMITS.restWaitS) return failure({ error: "invalid_args", message: `wait_s must be 0..${INBOX_LIMITS.restWaitS}` });
    const sinceRaw = url.searchParams.get("since");
    const since = sinceRaw === null || sinceRaw === "" ? undefined : seqOf(sinceRaw);
    if (sinceRaw && since === undefined) return failure({ error: "invalid_args", message: "since must be an event id like e41" });
    const out = await backend.device(id).events(token!, { since, wait_ms: waitS * 1000, signal: request.signal });
    return out.ok ? json({ ok: true, ...out.result, ...extra(out) }) : failed(out);
  }

  if (path === "/v1/webhook") {
    const dev = tokenDevice(token);
    if (!dev) return failure({ error: "unauthorized" });
    let out: Outcome;
    if (request.method === "GET") out = await backend.device(dev).webhook(token!, { action: "get" });
    else if (request.method === "DELETE") out = await backend.device(dev).webhook(token!, { action: "delete" });
    else if (request.method === "PUT") {
      const b = await body(request);
      if (!b) return failure({ error: "bad_request", message: "body must be a JSON object" });
      out = await backend.device(dev).webhook(token!, { action: "set", spec: b });
    } else return json({ ok: false, error: "method_not_allowed" }, 405);
    return out.ok ? json({ ok: true, result: out.result, ...extra(out) }) : failed(out);
  }

  const job = /^\/v1\/jobs\/([^/]+)$/.exec(path);
  if (job) {
    if (request.method !== "GET") return json({ ok: false, error: "method_not_allowed" }, 405);
    const jobId = decodeURIComponent(job[1]);
    const dev = jobDevice(jobId);
    if (!dev) return failure({ error: "not_found" });
    if (tokenDevice(token) !== dev) return failure({ error: "unauthorized" });
    const w = url.searchParams.get("wait_s");
    const waitS = w === null ? 0 : Number(w);
    if (!Number.isInteger(waitS) || waitS < 0 || waitS > 8) return failure({ error: "invalid_args", message: "wait_s must be 0..8" });
    const out = await backend.device(dev).job(token!, jobId, waitS * 1000);
    return out.ok ? json({ ...out.result, ...extra(out) }) : failed(out);
  }

  if (path === "/v1/token") {
    if (request.method !== "DELETE") return json({ ok: false, error: "method_not_allowed" }, 405);
    const dev = tokenDevice(token);
    if (!dev) return failure({ error: "unauthorized" });
    const out = await backend.device(dev).revoke(token!);
    return out.ok ? json({ ok: true, result: out.result }) : failed(out);
  }

  return json({ ok: false, error: "not_found" }, 404);
}
