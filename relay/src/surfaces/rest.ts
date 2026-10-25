// REST surface (docs/AGENT-LINK.md "REST envelopes"). Fetch-API only.
//
//   POST   /v1/pair                     {device_id, link_code, agent_name}  (no token; long-polls <= 120 s)
//   POST   /v1/devices/<id>/cmd         {name, args}   -> {ok:true,result} | {ok:false,error}
//   GET    /v1/jobs/<job>[?wait_s=0..8] -> {job, kind, status, result?, error?}
//   DELETE /v1/token                    revoke the calling token

import { type Outcome, type RelayBackend, bearer, jobDevice, tokenDevice } from "../core/api.js";
import { DEVICE_ID_RE, isRecord } from "../core/protocol.js";

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
  pair_busy: 409,
  device_timeout: 504,
  internal: 500,
};

const json = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } });

function failure(out: { error: string; message?: string }) {
  // Unknown codes are device-reported errors (e.g. mic_busy): 502.
  return json({ ok: false, error: out.error, ...(out.message ? { message: out.message } : {}) }, STATUS[out.error] ?? 502);
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

export async function handleRest(request: Request, backend: RelayBackend): Promise<Response> {
  const url = new URL(request.url);
  const path = url.pathname;
  const token = bearer(request.headers.get("authorization"));

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
    if (!out.ok) return failure(out);
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
    const out: Outcome = await backend.device(id).command(token!, b.name as string, b.args);
    return out.ok ? json({ ok: true, result: out.result }) : failure(out);
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
    return out.ok ? json(out.result) : failure(out);
  }

  if (path === "/v1/token") {
    if (request.method !== "DELETE") return json({ ok: false, error: "method_not_allowed" }, 405);
    const dev = tokenDevice(token);
    if (!dev) return failure({ error: "unauthorized" });
    const out = await backend.device(dev).revoke(token!);
    return out.ok ? json({ ok: true, result: out.result }) : failure(out);
  }

  return json({ ok: false, error: "not_found" }, 404);
}
