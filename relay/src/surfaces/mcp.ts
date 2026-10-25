// Remote MCP surface: Streamable HTTP transport (spec 2025-03-26 .. 2025-11-25)
// at POST /mcp, stateless, tools only, JSON responses (no SSE stream: every
// tool answers within ~8 s by design, so the server never needs to stream).
// Tool names/arguments mirror the local stdio server in cli/
// (internal/molty/mcp.go); relay-specific differences are stated in each
// description. Authorization: Bearer <agent_token>; molty_pair needs none.
//
// Uses only the Fetch API (Request/Response) -- no Cloudflare imports -- so a
// Node host can serve it unchanged.

import { type AgentApi, type Outcome, type RelayBackend, bearer, tokenDevice } from "../core/api.js";
import { DEFAULT_LISTEN_MS, MOODS } from "../core/commands.js";
import { DEVICE_ID_RE, isRecord, type JsonObject } from "../core/protocol.js";
import { RELAY_VERSION } from "../core/version.js";

export const SUPPORTED_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26"];
export const SERVER_INFO = { name: "molty-relay", version: RELAY_VERSION };
const MAX_BODY = 256 * 1024;
const INSTRUCTIONS =
  "Tools operate a physical Molty device through the molty-relay. Pair first with molty_pair (no token needed), then send the returned agent_token as 'Authorization: Bearer <token>'. molty_say and molty_listen return a job immediately; poll it with molty_wait (<= 8 s per call).";

type Schema = { type: "object"; properties: Record<string, { type: string; description: string }>; required: string[]; additionalProperties: false };
interface ToolDef {
  name: string;
  description: string;
  inputSchema: Schema;
  auth: boolean;
  run(api: AgentApi, token: string, args: Record<string, unknown>): Promise<Outcome<JsonObject>>;
}

const obj = (properties: Schema["properties"], required: string[] = []): Schema => ({ type: "object", properties, required, additionalProperties: false });

export const TOOLS: ToolDef[] = [
  {
    name: "molty_pair",
    description:
      "Pair with a Molty device: no token needed. Give device_id, link_code (8 chars from the device console's 'Connect an agent' block) and agent_name; the device shows agent_name plus a 4-digit code and its owner taps to accept. Waits up to wait_s (default 8, max 110); if still undecided returns status pending with pair_id and code: call again with device_id and pair_id to keep waiting (the request lives 120 s). On approval returns agent_token (shown once; store it) for all other tools.",
    inputSchema: obj(
      {
        device_id: { type: "string", description: "Device id, e.g. molty-k7m2q9xa4c." },
        link_code: { type: "string", description: "Eight-character link code [A-Z2-9] shown in the device console." },
        agent_name: { type: "string", description: "Agent name shown on the device for approval (1..64 chars)." },
        pair_id: { type: "string", description: "Resume a pending pairing returned by an earlier call (instead of link_code/agent_name)." },
        wait_s: { type: "integer", description: "Seconds to wait for the on-device decision, 0..110; default 8." },
      },
      ["device_id"],
    ),
    auth: false,
    async run(api, _token, a) {
      const wait = a.wait_s === undefined ? 8 : (a.wait_s as number);
      if (!Number.isInteger(wait) || wait < 0 || wait > 110) return { ok: false, error: "invalid_args", message: "wait_s must be 0..110" };
      if (a.pair_id === undefined && (a.link_code === undefined || a.agent_name === undefined)) {
        return { ok: false, error: "invalid_args", message: "give link_code and agent_name, or pair_id" };
      }
      const out = await api.pair({ device_id: a.device_id as string, link_code: a.link_code as string | undefined, agent_name: a.agent_name as string | undefined, pair_id: a.pair_id as string | undefined, wait_ms: wait * 1000 });
      if (!out.ok) return out;
      const r = out.result;
      if (r.status === "approved") return { ok: true, result: { ...r, usage: "Send 'Authorization: Bearer <agent_token>' on every MCP request." } };
      return { ok: true, result: { ...r, hint: "Not decided yet: call molty_pair again with device_id and pair_id." } };
    },
  },
  {
    name: "molty_status",
    description: "Return the device's status (fw, uptime, creature, state, mood, fps, rssi) plus relay info: online, connected_since, caps, voice provider and the latest device events.",
    inputSchema: obj({}),
    auth: true,
    run: (api, t) => api.command(t, "status", {}),
  },
  {
    name: "molty_mood",
    description: `Show ${MOODS.join(", ")}. Replaces the current display.`,
    inputSchema: obj({ mood: { type: "string", description: `One of ${MOODS.join(", ")}.` } }, ["mood"]),
    auth: true,
    run: (api, t, a) => api.command(t, "mood", a),
  },
  {
    name: "molty_show",
    description: "Display up to 16 KB of text with the speed reader at WPM (default 300). Returns after starting, not after reading completes.",
    inputSchema: obj(
      {
        text: { type: "string", description: "Nonempty UTF-8 text, at most 16384 bytes, to display using the RSVP reader." },
        wpm: { type: "integer", description: "Words per minute (100..800); default 300." },
      },
      ["text"],
    ),
    auth: true,
    run: (api, t, a) => api.command(t, "show", a),
  },
  {
    name: "molty_say",
    description:
      "Speak text through the device speaker using the relay's TTS. Returns {job} immediately; poll molty_wait until done ({ms, text}). The device shows the speaking mood and the text while audio plays. One audio job (say or listen) at a time; audio_busy otherwise. No WAV/path argument on the relay.",
    inputSchema: obj(
      {
        text: { type: "string", description: "Text to synthesize, at most 4096 bytes." },
        voice: { type: "string", description: "Optional relay TTS voice (provider-specific, e.g. luna for Workers AI Aura)." },
      },
      ["text"],
    ),
    auth: true,
    run: (api, t, a) => api.command(t, "say", a),
  },
  {
    name: "molty_listen",
    description: `Record the device microphone (default ${DEFAULT_LISTEN_MS} ms, maximum 10000) and transcribe it with the relay's STT. Returns {job} immediately; molty_wait returns {text, duration_ms, peak, rms}. The device shows the listening mood while recording. No audio file is returned.`,
    inputSchema: obj({ ms: { type: "integer", description: "Capture duration in milliseconds (1..10000); default 4000." } }),
    auth: true,
    run: (api, t, a) => api.command(t, "listen", a),
  },
  {
    name: "molty_wait",
    description: "Wait at most timeout_s (0..8, default 8) for a say/listen job. Returns the job result with status done, status pending if still queued/running (call again), or an error if it failed.",
    inputSchema: obj(
      {
        job: { type: "string", description: "Job id returned by molty_say or molty_listen." },
        timeout_s: { type: "integer", description: "Polling budget, integer 0..8 seconds; default 8." },
      },
      ["job"],
    ),
    auth: true,
    async run(api, t, a): Promise<Outcome<JsonObject>> {
      const s = a.timeout_s === undefined ? 8 : (a.timeout_s as number);
      if (!Number.isInteger(s) || s < 0 || s > 8) return { ok: false, error: "invalid_args", message: "timeout_s must be 0..8" };
      const out = await api.job(t, a.job as string, s * 1000);
      if (!out.ok) return out;
      const j = out.result;
      if (j.status === "done") return { ok: true, result: { ...(j.result ?? {}), job: j.job, status: "done" } as JsonObject };
      if (j.status === "failed") return { ok: false, error: j.error ?? "failed", message: `job ${j.job} failed` };
      return { ok: true, result: { job: j.job, status: "pending", state: j.status } };
    },
  },
  {
    name: "molty_state",
    description: "Read creature state (omit state) or patch it (state = object of creature fields, same semantics as the device's /api/creature/state).",
    inputSchema: obj({ state: { type: "object", description: "State patch; omit to read state." } }),
    auth: true,
    run: (api, t, a) => api.command(t, "state", a.state === undefined ? {} : { state: a.state }),
  },
  {
    name: "molty_volume",
    description: "Set the physical speaker volume from 0 (mute) to 21 (maximum).",
    inputSchema: obj({ volume: { type: "integer", description: "Speaker volume, integer 0..21." } }, ["volume"]),
    auth: true,
    run: (api, t, a) => api.command(t, "volume", a),
  },
  {
    name: "molty_imu",
    description: "Read one IMU snapshot: ax/ay/az acceleration in g, gx/gy/gz angular velocity in degrees/second, ms device sample timestamp.",
    inputSchema: obj({}),
    auth: true,
    run: (api, t) => api.command(t, "imu", {}),
  },
  {
    name: "molty_diag",
    description:
      "Device health: sim/panel frame-time histograms (fps, p50/p95/p99/max ms, frames over 50 ms), touch-to-panel latency, per-task CPU %, internal/PSRAM heap free and low-water, allocation failures, stall probes, Wi-Fi/gateway ping and link state. timeline (0..180) adds that many seconds of the 1 Hz timeline.",
    inputSchema: obj({ timeline: { type: "integer", description: "Seconds of 1 Hz timeline to include, 0..180 (default 0)." } }),
    auth: true,
    run: (api, t, a) => api.command(t, "diag", a),
  },
  {
    name: "molty_log",
    description:
      "Tail the device log ring (ESP_LOG lines + structured EVT events: link, audio, stalls, heap, Wi-Fi). Returns {next, text}; pass next back as since to continue. max is 1024..32768 bytes (default 12288).",
    inputSchema: obj({
      since: { type: "integer", description: "First log sequence number to return (default 0 = oldest kept)." },
      max: { type: "integer", description: "Maximum bytes of text, 1024..32768." },
    }),
    auth: true,
    run: (api, t, a) => api.command(t, "log", a),
  },
];

const TOOL_MAP = new Map(TOOLS.map((t) => [t.name, t]));

/** Same checks as the Go addTool: object, no unknown/null keys, required present, primitive types. */
export function checkArgs(schema: Schema, raw: unknown): string | undefined {
  const args = raw === undefined ? {} : raw;
  if (!isRecord(args)) return "arguments must be an object";
  for (const [k, v] of Object.entries(args)) {
    const prop = schema.properties[k];
    if (!prop) return `unknown argument ${k}`;
    if (v === null) return `${k} must not be null`;
    const ok =
      prop.type === "string" ? typeof v === "string" : prop.type === "integer" ? Number.isInteger(v) : prop.type === "object" ? isRecord(v) : true;
    if (!ok) return `${k} must be ${prop.type === "integer" ? "an integer" : `a${prop.type === "object" ? "n" : ""} ${prop.type}`}`;
  }
  for (const k of schema.required) if (!(k in args)) return `missing required argument ${k}`;
  return undefined;
}

type RpcMessage = { jsonrpc?: unknown; id?: unknown; method?: unknown; params?: unknown };
const rpcError = (id: unknown, code: number, message: string) => ({ jsonrpc: "2.0", id: id ?? null, error: { code, message } });
const rpcResult = (id: unknown, result: unknown) => ({ jsonrpc: "2.0", id, result });

function toolResult(out: Outcome<JsonObject>) {
  const value: JsonObject = out.ok ? { ok: true, ...out.result } : { ok: false, error: out.error, ...(out.message ? { message: out.message } : {}) };
  return { content: [{ type: "text", text: JSON.stringify(value) }], structuredContent: value, isError: !out.ok };
}

async function callTool(backend: RelayBackend, token: string | undefined, params: unknown) {
  if (!isRecord(params) || typeof params.name !== "string") return { error: [-32602, "Invalid tool parameters"] as const };
  const tool = TOOL_MAP.get(params.name);
  if (!tool) return { error: [-32602, `Unknown tool ${params.name}`] as const };
  const problem = checkArgs(tool.inputSchema, params.arguments);
  if (problem) return { result: toolResult({ ok: false, error: "invalid_args", message: problem }) };
  const args = (params.arguments ?? {}) as Record<string, unknown>;
  let deviceId: string | undefined;
  if (tool.auth) {
    deviceId = tokenDevice(token);
    if (!deviceId) return { result: toolResult({ ok: false, error: "unauthorized", message: "missing or malformed bearer agent_token; pair with molty_pair first" }) };
  } else {
    deviceId = args.device_id as string;
    if (!DEVICE_ID_RE.test(deviceId)) return { result: toolResult({ ok: false, error: "invalid_args", message: "device_id must look like molty-xxxxxxxxxx" }) };
  }
  try {
    return { result: toolResult(await tool.run(backend.device(deviceId), token ?? "", args)) };
  } catch (e) {
    return { result: toolResult({ ok: false, error: "internal", message: e instanceof Error ? e.message : String(e) }) };
  }
}

async function handleOne(msg: unknown, backend: RelayBackend, token: string | undefined): Promise<object | undefined> {
  if (!isRecord(msg)) return rpcError(null, -32600, "Invalid Request");
  const m = msg as RpcMessage;
  const isRequest = "id" in m && m.id !== null && m.id !== undefined;
  if (typeof m.method !== "string") {
    // A JSON-RPC response from the client (we never send requests) -- ignore.
    return isRequest || !("result" in m || "error" in m) ? rpcError(m.id, -32600, "Invalid Request") : undefined;
  }
  if (m.jsonrpc !== "2.0") return isRequest ? rpcError(m.id, -32600, "Invalid Request") : undefined;
  if (!isRequest) return undefined; // notifications (initialized, cancelled, ...) need no reply
  if (typeof m.id !== "string" && typeof m.id !== "number") return rpcError(null, -32600, "Invalid request ID");
  switch (m.method) {
    case "initialize": {
      const requested = isRecord(m.params) ? m.params.protocolVersion : undefined;
      if (typeof requested !== "string" || !requested) return rpcError(m.id, -32602, "protocolVersion required");
      const version = SUPPORTED_VERSIONS.includes(requested) ? requested : SUPPORTED_VERSIONS[0];
      return rpcResult(m.id, { protocolVersion: version, capabilities: { tools: { listChanged: false } }, serverInfo: SERVER_INFO, instructions: INSTRUCTIONS });
    }
    case "ping":
      return rpcResult(m.id, {});
    case "tools/list":
      return rpcResult(m.id, { tools: TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })) });
    case "tools/call": {
      const out = await callTool(backend, token, m.params);
      return "error" in out && out.error ? rpcError(m.id, out.error[0], out.error[1]) : rpcResult(m.id, out.result);
    }
    default:
      return rpcError(m.id, -32601, "Method not found");
  }
}

const json = (body: unknown, status = 200, extra: Record<string, string> = {}) =>
  new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json", ...extra } });

export async function handleMcp(request: Request, backend: RelayBackend): Promise<Response> {
  if (request.method === "GET" || request.method === "DELETE") {
    // No server-initiated SSE stream and no sessions in this stateless server.
    return new Response("Method Not Allowed", { status: 405, headers: { allow: "POST" } });
  }
  if (request.method !== "POST") return new Response("Method Not Allowed", { status: 405, headers: { allow: "POST" } });
  const version = request.headers.get("mcp-protocol-version");
  if (version && !SUPPORTED_VERSIONS.includes(version)) return json(rpcError(null, -32600, `Unsupported MCP-Protocol-Version ${version}`), 400);
  const text = await request.text();
  if (text.length > MAX_BODY) return json(rpcError(null, -32600, "Request too large"), 413);
  let body: unknown;
  try {
    body = JSON.parse(text);
  } catch {
    return json(rpcError(null, -32700, "Parse error"), 400);
  }
  const token = bearer(request.headers.get("authorization"));
  if (Array.isArray(body)) {
    // Batches were allowed in 2025-03-26 (removed later); accept for older clients.
    if (body.length === 0) return json(rpcError(null, -32600, "Invalid Request"), 400);
    const replies = (await Promise.all(body.map((m) => handleOne(m, backend, token)))).filter(Boolean);
    return replies.length ? json(replies) : new Response(null, { status: 202 });
  }
  const reply = await handleOne(body, backend, token);
  return reply ? json(reply) : new Response(null, { status: 202 });
}
