// REST surface (docs/AGENT-LINK.md "REST envelopes"). Fetch-API only.
//
//   POST   /v1/pair                     {device_id, link_code, agent_name}  (no token; long-polls <= 120 s)
//   POST   /v1/devices/<id>/cmd         {name, args, wait?}   -> {ok:true,result} | {ok:false,error}
//                                       (say/listen + wait:true: blocks <= 24 s -> result {job, status:"done", ...} | 202 {job, status:"pending"})
//   POST   /v1/devices/<id>/talk        {say?, listen?, listen_ms?, voice?, end_silence_ms?, no_speech_ms?, vad?, ring_color?, ack?, in_reply_to?, client_timing?}
//                                       (in_reply_to / a say's: an utterance's turn_id; another agent answered it first -> 409 turn_claimed)
//                                       -> {ok:true, result:{job, status:"done", heard, said?, reason, ms, timings}} | 202 pending
//   POST   /v1/devices/<id>/ask         {say?, options[1..5], timeout_s?, voice?}
//                                       -> {ok:true, result:{job, status:"done", answer, index, reason?, said?, ms, timings}} | 202 pending
//   GET    /v1/devices/<id>/updates     -> {ok:true, result:{api_version, firmware, actions}} ("check Molty for updates")
//   POST   /v1/devices/<id>/update      {} -> job: {ok:true, result:{job, status:"done", outcome, ...}} | 202 {job, status:"pending", phase}
//                                       | {ok:true, result:{status:"up_to_date", current, latest}}
//   GET    /v1/jobs/<job>[?wait_s=0..8] -> {job, kind, status, phase?, result?, error?, detail?}
//   DELETE /v1/token                    revoke the calling token
//   GET    /v1/devices/<id>/events[?since=e41&wait_s=0..25] -> {ok, events, next, more}
//   GET|PUT|DELETE /v1/webhook          read / register (secret once) / remove the caller's webhook
//   GET|POST|DELETE /v1/devices/<id>/voice  owner voice-box settings (header X-Molty-Link-Code: the
//                                       device's current link code; CORS, for the device-served console):
//                                       read / {mode?, voice?, reflex?, reflex_compare?, liveliness?} / unpair the voice box
//   GET    /v1/devices/<id>/activity/audio/<job>  owner only (link code): a kept listen's audio as audio/wav
//   GET|POST /v1/devices/<id>/activity   owner activity log (same link-code auth + CORS):
//                                       ?since=a42|<unix ms>&limit&agent&kind=a,b&job&errors=1 -> {entries, next, more,
//                                       transcripts, limits}; POST {transcripts: bool} sets "log transcripts"
//   GET    /v1/activity[?since&limit&kind&job&errors]  the CALLING agent's own activity -> {ok, result:{entries, next, more}}
//   POST   /v1/devices/<id>/draft       {turn_id, based_on, opener, max_new_words?, voice?}  while the human talks (an open
//                                       shake turn): hold an opener -> {ok, result:{turn_id, based_on, status:"held", max_new_words}}
//   POST   /v1/devices/<id>/react       {turn_id, mood|action}  while the human talks: screen only -> {ok, result:{shown, for_ms}}
//                                       (say / talk / ask / mood / show meanwhile: 409 human_speaking; react > 1 per 3 s: 429 rate_limited)
//   GET    /v1/notes[?key=k]            the caller's private notes: list, or one key -> {key, value, updated}
//   PUT    /v1/notes {key, value}       store (value: any JSON, <= 8 KB; <= 64 keys / 32 KB per token: 413 too_large)
//   DELETE /v1/notes?key=k              remove one key
//   GET|DELETE /v1/devices/<id>/speech  owner (link code, CORS): the human's speech habits + calibration; DELETE resets
//   GET|POST|DELETE /v1/devices/<id>/phrases  the caller's quick phrases (core/phrases.ts): list / {phrases:[{id,text}]}
//                                       (upsert, audio synthesised now) / ?id=a,b (or ?id=* = all) -> {ok, result:{phrases, builtins, limits, voice}}
//                                       (play one: POST .../cmd {name:"quick", args:{id, in_reply_to?, force?}})
//
// A voice box token (pair role "voice") is refused on every agent route: 403 forbidden.
//
// Any authenticated response may carry the caller's unread device events as an
// additive top-level `events` array (+ `more_events`); reading them marks them read.
// Likewise, once per agent per newer firmware release `firmware_update: {current, latest, board, notes_url,
// can_self_update, how}` (core/updates.ts). An error's `details` (e.g.
// firmware_too_old {feature, current, needed, latest, how}) are top-level fields next to `error`.

import { type ActivityOwnerRequest, type Inbox, type JobView, type Outcome, type RelayBackend, type VoiceOwnerRequest, bearer, jobDevice, tokenDevice } from "../core/api.js";
import { TALK_BUDGET_MS } from "../core/commands.js";
import { INBOX_LIMITS, seqOf } from "../core/events.js";
import { DEVICE_ID_RE, isRecord } from "../core/protocol.js";

const STATUS: Record<string, number> = {
  unauthorized: 401,
  forbidden: 403,
  voice_box_offline: 503,
  invalid_args: 400,
  unknown_command: 400,
  bad_request: 400,
  not_found: 404,
  bad_link_code: 403,
  rejected: 403,
  timeout: 408,
  device_offline: 409,
  audio_busy: 409,
  turn_claimed: 409,
  human_speaking: 409,
  turn_closed: 409,
  rate_limited: 429,
  too_large: 413,
  unsupported: 409,
  firmware_too_old: 409,
  no_build: 409,
  no_checksum: 409,
  unknown_version: 409,
  release_unavailable: 503,
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

function failure(out: { error: string; message?: string; details?: Record<string, unknown>; inbox?: Inbox }, rideAlong: Record<string, unknown> = {}) {
  // Unknown codes are device-reported errors (e.g. mic_busy): 502.
  return json({ ok: false, error: out.error, ...(out.message ? { message: out.message } : {}), ...(out.details ?? {}), ...inboxExtra(out.inbox), ...rideAlong }, STATUS[out.error] ?? 502);
}

type RideAlong = Pick<Outcome, "inbox" | "fw_notice">;

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

/** Two outcomes' ride-alongs as one (events concatenated, the later notices). */
function merged(a: RideAlong, b: RideAlong): RideAlong {
  const events = [...(a.inbox?.events ?? []), ...(b.inbox?.events ?? [])];
  const inbox: Inbox | undefined = events.length ? { events, more: !!(a.inbox?.more || b.inbox?.more), now: Math.max(a.inbox?.now ?? 0, b.inbox?.now ?? 0) } : undefined;
  return { inbox, fw_notice: b.fw_notice ?? a.fw_notice };
}

/**
 * A waited job as a REST reply: done -> 200 result, failed -> error, still running -> 202 pending.
 * `ride` is what the starting call already consumed (events marked read, firmware notice marked
 * seen): it reaches the caller on EVERY outcome, failures included, or it is lost for good.
 */
function jobResponse(
  out: Outcome<JobView>,
  ride: RideAlong,
  extra: (o: RideAlong) => Record<string, unknown>,
): Response {
  const all = extra(merged(ride, out));
  if (!out.ok) return failure({ error: out.error, message: out.message }, all);
  const j = out.result;
  if (j.status === "failed") return failure({ error: j.error ?? "failed", message: typeof j.detail === "string" ? j.detail : `job ${j.job} failed` }, all);
  if (j.status === "done") return json({ ok: true, result: { ...(j.result ?? {}), job: j.job, status: "done" }, ...all });
  return json({ ok: true, result: { job: j.job, status: "pending", state: j.status, ...(j.phase ? { phase: j.phase } : {}) }, ...all }, 202);
}

export async function handleRest(request: Request, backend: RelayBackend): Promise<Response> {
  const url = new URL(request.url);
  const path = url.pathname;
  const token = bearer(request.headers.get("authorization"));
  const extra = (out: RideAlong) => ({
    ...inboxExtra(out.inbox),
    ...(out.fw_notice ? { firmware_update: out.fw_notice } : {}),
  });
  // A failed outcome renders its ride-alongs too (events already marked read, firmware notice
  // already marked seen), like MCP's toolResult: dropping them here would lose them for good.
  const failed = (out: { error: string; message?: string; details?: Record<string, unknown> } & RideAlong) => failure(out, extra(out));

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
      ...(b.role !== undefined ? { role: b.role as "agent" | "voice" } : {}),
    });
    if (!out.ok) return failed(out);
    if (out.result.status === "approved") {
      const { agent_token, device_id, agent_name, code, api_version, role } = out.result;
      return json({ ok: true, agent_token, device_id, agent_name, code, api_version, ...(role ? { role } : {}) });
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

  const turn = /^\/v1\/devices\/([^/]+)\/(talk|ask|draft|react)$/.exec(path);
  if (turn) {
    if (request.method !== "POST") return json({ ok: false, error: "method_not_allowed" }, 405);
    const id = decodeURIComponent(turn[1]);
    if (!DEVICE_ID_RE.test(id)) return failure({ error: "not_found" });
    if (tokenDevice(token) !== id) return failure({ error: "unauthorized" });
    const b = await body(request);
    if (!b) return failure({ error: "bad_request", message: "body must be a JSON object" });
    const api = backend.device(id);
    const out = await (turn[2] === "ask" ? api.ask(token!, b) : turn[2] === "draft" ? api.draft(token!, b) : turn[2] === "react" ? api.react(token!, b) : api.talk(token!, b));
    if (!out.ok) return failed(out);
    return json({ ok: true, result: out.result, ...extra(out) }, out.result.status === "pending" ? 202 : 200);
  }

  const upd = /^\/v1\/devices\/([^/]+)\/(updates|update)$/.exec(path);
  if (upd) {
    const want = upd[2] === "updates" ? "GET" : "POST";
    if (request.method !== want) return json({ ok: false, error: "method_not_allowed" }, 405);
    const id = decodeURIComponent(upd[1]);
    if (!DEVICE_ID_RE.test(id)) return failure({ error: "not_found" });
    if (tokenDevice(token) !== id) return failure({ error: "unauthorized" });
    const api = backend.device(id);
    let out: Outcome;
    if (want === "GET") out = await api.updates(token!);
    else {
      const b = await body(request);
      if (!b) return failure({ error: "bad_request", message: "body must be a JSON object (empty: {})" });
      out = await api.update(token!, b);
    }
    if (!out.ok) return failed(out);
    return json({ ok: true, result: out.result, ...extra(out) }, out.result.status === "pending" ? 202 : 200);
  }

  const phrases = /^\/v1\/devices\/([^/]+)\/phrases$/.exec(path);
  if (phrases) {
    const id = decodeURIComponent(phrases[1]);
    if (!DEVICE_ID_RE.test(id)) return failure({ error: "not_found" });
    if (tokenDevice(token) !== id) return failure({ error: "unauthorized" });
    let req: Record<string, unknown>;
    if (request.method === "GET") req = { action: "list" };
    else if (request.method === "POST" || request.method === "PUT") {
      const b = await body(request);
      if (!b) return failure({ error: "bad_request", message: "body must be a JSON object: {phrases:[{id, text}]}" });
      req = { action: "set", ...b };
    } else if (request.method === "DELETE") {
      const ids = (url.searchParams.get("id") ?? "").split(",").filter(Boolean);
      req = { action: "delete", ids };
    } else return json({ ok: false, error: "method_not_allowed" }, 405);
    const out = await backend.device(id).phrases(token!, req);
    return out.ok ? json({ ok: true, result: out.result, ...extra(out) }) : failed(out);
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

  if (path === "/v1/notes") {
    const dev = tokenDevice(token);
    if (!dev) return failure({ error: "unauthorized" });
    const key = url.searchParams.get("key");
    let out: Outcome;
    if (request.method === "GET") out = await backend.device(dev).notes(token!, key === null ? { action: "list" } : { action: "get", key });
    else if (request.method === "DELETE") out = await backend.device(dev).notes(token!, { action: "delete", key: key ?? undefined });
    else if (request.method === "PUT") {
      const b = await body(request);
      if (!b) return failure({ error: "bad_request", message: "body must be a JSON object {key, value}" });
      out = await backend.device(dev).notes(token!, { action: "put", key: b.key, value: b.value });
    } else return json({ ok: false, error: "method_not_allowed" }, 405);
    return out.ok ? json({ ok: true, result: out.result, ...extra(out) }) : failed(out);
  }

  const speech = /^\/v1\/devices\/([^/]+)\/speech$/.exec(path);
  if (speech) return speechOwner(request, speech[1], backend);

  const voice = /^\/v1\/devices\/([^/]+)\/voice$/.exec(path);
  if (voice) return voiceOwner(request, voice[1], backend);

  const audio = /^\/v1\/devices\/([^/]+)\/activity\/audio\/([^/]+)$/.exec(path);
  if (audio) return activityAudio(request, audio[1], audio[2], backend);
  const activity = /^\/v1\/devices\/([^/]+)\/activity$/.exec(path);
  if (activity) return activityOwner(request, activity[1], backend);

  if (path === "/v1/activity") {
    if (request.method !== "GET") return json({ ok: false, error: "method_not_allowed" }, 405);
    const dev = tokenDevice(token);
    if (!dev) return failure({ error: "unauthorized" });
    const out = await backend.device(dev).activityLog(token!, queryParams(url));
    return out.ok ? json({ ok: true, result: out.result }) : failed(out);
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

// The setup console runs on the device's own origin (http://<molty>), so this route answers CORS.
const OWNER_CORS = {
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET, POST, DELETE, OPTIONS",
  "access-control-allow-headers": "content-type, x-molty-link-code",
  "access-control-max-age": "86400",
};

/** The activity query params, raw (core/activity.ts parses and checks them). */
function queryParams(url: URL): Record<string, unknown> {
  const q: Record<string, unknown> = {};
  for (const k of ["since", "limit", "agent", "kind", "job", "errors"]) {
    const v = url.searchParams.get(k);
    if (v !== null) q[k] = v;
  }
  return q;
}

/** Owner activity log: the device's current link code, like the voice settings (OwnerApi). */
async function activityOwner(request: Request, rawId: string, backend: RelayBackend): Promise<Response> {
  const cors = (r: Response) => {
    for (const [k, v] of Object.entries(OWNER_CORS)) r.headers.set(k, v);
    return r;
  };
  if (request.method === "OPTIONS") return cors(new Response(null, { status: 204 }));
  const id = decodeURIComponent(rawId);
  if (!DEVICE_ID_RE.test(id)) return cors(failure({ error: "not_found" }));
  const api = backend.device(id);
  if (!api.ownerActivity) return cors(failure({ error: "not_found" }));
  const code = request.headers.get("x-molty-link-code") ?? "";
  let req: ActivityOwnerRequest;
  if (request.method === "GET") req = { action: "get", query: queryParams(new URL(request.url)) };
  else if (request.method === "POST") {
    const b = await body(request);
    if (!b) return cors(failure({ error: "bad_request", message: "body must be a JSON object" }));
    req = { action: "set", transcripts: b.transcripts, keep_audio: b.keep_audio };
  } else return cors(json({ ok: false, error: "method_not_allowed" }, 405));
  const out = await api.ownerActivity(code, req);
  return cors(out.ok ? json({ ok: true, result: out.result }) : failure(out));
}

/**
 * Owner: one kept listen's audio as WAV (core/audio-keep.ts), by the device's current link code
 * only — no bearer token is read here, so an agent can never fetch it.
 */
async function activityAudio(request: Request, rawId: string, rawJob: string, backend: RelayBackend): Promise<Response> {
  const cors = (r: Response) => {
    for (const [k, v] of Object.entries(OWNER_CORS)) r.headers.set(k, v);
    return r;
  };
  if (request.method === "OPTIONS") return cors(new Response(null, { status: 204 }));
  if (request.method !== "GET") return cors(json({ ok: false, error: "method_not_allowed" }, 405));
  const id = decodeURIComponent(rawId);
  const job = decodeURIComponent(rawJob);
  if (!DEVICE_ID_RE.test(id) || jobDevice(job) !== id) return cors(failure({ error: "not_found" }));
  const api = backend.device(id);
  if (!api.ownerAudio) return cors(failure({ error: "not_found" }));
  const out = await api.ownerAudio(request.headers.get("x-molty-link-code") ?? "", job);
  if (!out.ok) return cors(failure(out));
  const name = `${job.replace(/[^A-Za-z0-9_.-]/g, "_")}.wav`;
  return cors(new Response(out.result.wav, { status: 200, headers: { "content-type": "audio/wav", "content-disposition": `attachment; filename="${name}"`, "cache-control": "no-store" } }));
}

/** Owner speech stats + calibration (core/calibration.ts): the device's current link code, like the voice settings. */
async function speechOwner(request: Request, rawId: string, backend: RelayBackend): Promise<Response> {
  const cors = (r: Response) => {
    for (const [k, v] of Object.entries(OWNER_CORS)) r.headers.set(k, v);
    return r;
  };
  if (request.method === "OPTIONS") return cors(new Response(null, { status: 204 }));
  const id = decodeURIComponent(rawId);
  if (!DEVICE_ID_RE.test(id)) return cors(failure({ error: "not_found" }));
  const api = backend.device(id);
  if (!api.speechStats) return cors(failure({ error: "not_found" }));
  if (request.method !== "GET" && request.method !== "DELETE") return cors(json({ ok: false, error: "method_not_allowed" }, 405));
  const out = await api.speechStats(request.headers.get("x-molty-link-code") ?? "", { action: request.method === "GET" ? "get" : "reset" });
  return cors(out.ok ? json({ ok: true, result: out.result }) : failure(out));
}

/** Owner voice-box settings: authenticated by the device's current link code (OwnerApi). */
async function voiceOwner(request: Request, rawId: string, backend: RelayBackend): Promise<Response> {
  const cors = (r: Response) => {
    for (const [k, v] of Object.entries(OWNER_CORS)) r.headers.set(k, v);
    return r;
  };
  if (request.method === "OPTIONS") return cors(new Response(null, { status: 204 }));
  const id = decodeURIComponent(rawId);
  if (!DEVICE_ID_RE.test(id)) return cors(failure({ error: "not_found" }));
  const api = backend.device(id);
  if (!api.voiceSettings) return cors(failure({ error: "not_found" }));
  const code = request.headers.get("x-molty-link-code") ?? "";
  let req: VoiceOwnerRequest;
  if (request.method === "GET") req = { action: "get" };
  else if (request.method === "DELETE") req = { action: "unpair" };
  else if (request.method === "POST") {
    const b = await body(request);
    if (!b) return cors(failure({ error: "bad_request", message: "body must be a JSON object" }));
    req = { action: "set", mode: b.mode, voice: b.voice, reflex: b.reflex, reflex_compare: b.reflex_compare, liveliness: b.liveliness };
  } else return cors(json({ ok: false, error: "method_not_allowed" }, 405));
  const out = await api.voiceSettings(code, req);
  return cors(out.ok ? json({ ok: true, result: out.result }) : failure(out));
}
