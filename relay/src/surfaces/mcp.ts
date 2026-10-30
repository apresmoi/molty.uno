// Remote MCP surface: Streamable HTTP transport (spec 2025-03-26 .. 2025-11-25)
// at POST /mcp, stateless, tools only, JSON responses (no SSE stream: every
// tool answers within ~8 s, or ~24 s for the blocking talk/wait calls, so the
// server never needs to stream).
// Tool names/arguments mirror the local stdio server in cli/
// (internal/molty/mcp.go); relay-specific differences are stated in each
// description. Authorization: Bearer <agent_token>; molty_pair needs none.
//
// Uses only the Fetch API (Request/Response) -- no Cloudflare imports -- so a
// Node host can serve it unchanged.

import { type AgentApi, type FirmwareNotice, type Inbox, type JobView, type Outcome, type RelayBackend, bearer, tokenDevice } from "../core/api.js";
import { COLORS, DEFAULT_ASK_COLORS, DEFAULT_ASK_TIMEOUT_S, DEFAULT_LISTEN_MS, MAX_ASK_LABEL, MAX_ASK_OPTIONS, MAX_ASK_TIMEOUT_S, MAX_LISTEN_MS, MAX_MOOD_HOLD_S, MOODS, TALK_BUDGET_MS, TALK_DEFAULT_LISTEN_MS, THINK_HOLD_S, VAD_LIMITS } from "../core/commands.js";
import { AGENT_EVENT_TYPES, type AgentEvent, INBOX_LIMITS, eventsLine, seqOf } from "../core/events.js";
import { DEVICE_ID_RE, isRecord, type JsonObject } from "../core/protocol.js";
import { RELAY_VERSION, SKILL_VERSION, type SkillNotice, skillHeadline, skillLine, skillUpdate } from "../core/version.js";

export const SUPPORTED_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26"];
export const SERVER_INFO = { name: "molty-relay", title: `Molty relay (skill v${SKILL_VERSION})`, version: RELAY_VERSION };
const MAX_BODY = 256 * 1024;
const INSTRUCTIONS =
  `Tools operate a physical Molty device through the molty-relay. Pair first with molty_pair (no token needed), then send the returned agent_token as 'Authorization: Bearer <token>'. For conversation use molty_talk: ONE call per turn speaks your reply, then listens and returns what the human said ({heard, reason}); moods (listening, thinking, speaking) are automatic, so don't call molty_mood around talking. Keep spoken replies to 1-2 sentences. For a decision the human can answer with a tap use molty_ask: say the question, offer 1-${MAX_ASK_OPTIONS} options of at most ${MAX_ASK_LABEL} characters ("yes", "no", "later"); answer is null on timeout/cancelled/offline. molty_say / molty_listen return a job immediately (wait:true blocks until done); poll a job with molty_wait (<= 8 s per call). ` +
  `Molty also emits events (${AGENT_EVENT_TYPES.join(", ")}) into your own inbox: unread ones ride along on every tool result as 'events' plus a 'Molty: ...' line; molty_events pulls/waits for them; molty_webhook registers an HTTPS request the relay sends you per event. React briefly, once per moment. Full guide: GET /skill.md on this relay. ${skillHeadline()}. When the skill changes again, your next successful tool result carries 'skill_update' plus a 'Molty's skill was updated' line once: re-read /skill.md then. When your human asks to check Molty for updates (any wording), call molty_updates and follow its 'actions'; ask before molty_update.`;

type Prop = { type: string; description: string; enum?: string[]; items?: { type: "string"; enum?: string[] } | { anyOf: JsonObject[] } };
type Schema = { type: "object"; properties: Record<string, Prop>; required: string[]; additionalProperties: false };
interface ToolDef {
  name: string;
  description: string;
  inputSchema: Schema;
  auth: boolean;
  run(api: AgentApi, token: string, args: Record<string, unknown>): Promise<Outcome<JsonObject>>;
}

const items = (p: Prop) => (p.items && "type" in p.items ? p.items : undefined);

const VAD_PROPS: Schema["properties"] = {
  end_silence_ms: { type: "integer", description: `Device end-of-speech: silence that ends the turn, ${VAD_LIMITS.end_silence_ms[0]}..${VAD_LIMITS.end_silence_ms[1]} ms (device default 700).` },
  no_speech_ms: { type: "integer", description: `Device end-of-speech: give up when nobody starts talking within ${VAD_LIMITS.no_speech_ms[0]}..${VAD_LIMITS.no_speech_ms[1]} ms (device default 4000).` },
  vad: { type: "boolean", description: "Device end-of-speech detection on (default) or off (record the whole window)." },
  ring_color: { type: "string", description: `Colour of the listening ring on Molty's screen: ${Object.keys(COLORS).join(", ")} or "#rrggbb". Default: the device's setting.` },
};

/** say/listen with `wait: true` block on their job (bounded), returning what molty_wait would. */
async function commandWait(api: AgentApi, t: string, name: "say" | "listen", a: Record<string, unknown>): Promise<Outcome<JsonObject>> {
  const { wait, ...args } = a;
  const start = Date.now();
  const out = await api.command(t, name, args);
  if (!out.ok || wait !== true) return out;
  return jobOutcome(await api.job(t, out.result.job as string, Math.max(0, TALK_BUDGET_MS - (Date.now() - start))));
}

/** Shapes a job view like molty_wait: done -> result + {job, status}, failed -> error, else pending. */
function jobOutcome(out: Outcome<JobView>): Outcome<JsonObject> {
  if (!out.ok) return out;
  const j = out.result;
  if (j.status === "done") return { ok: true, result: { ...(j.result ?? {}), job: j.job, status: "done" } as JsonObject };
  if (j.status === "failed") return { ok: false, error: j.error ?? "failed", message: typeof j.detail === "string" ? j.detail : `job ${j.job} failed` };
  return { ok: true, result: { job: j.job, status: "pending", state: j.status, ...(typeof j.phase === "string" ? { phase: j.phase } : {}) } };
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
    description: `Show ${MOODS.join(", ")}. Replaces the current display. Quick moods (happy, surprised) revert on their own; listening/thinking/speaking persist. hold_s sets how long it stays (0..${MAX_MOOD_HOLD_S} s; 0 = until the next mood).`,
    inputSchema: obj(
      {
        mood: { type: "string", description: `One of ${MOODS.join(", ")}.` },
        hold_s: { type: "number", description: `Optional seconds to hold the mood before reverting, 0..${MAX_MOOD_HOLD_S}; 0 = until the next mood.` },
      },
      ["mood"],
    ),
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
      "Speak text through the device speaker using the relay's TTS (streamed: audio starts after the first sentence is synthesised). Returns {job} immediately; poll molty_wait until done ({ms, text, timings}), or pass wait:true to block until done. The device shows the speaking mood and the text while audio plays. One audio job (say/listen/talk) at a time; audio_busy otherwise. For a conversation turn use molty_talk instead.",
    inputSchema: obj(
      {
        text: { type: "string", description: "Text to synthesize, at most 4096 bytes." },
        voice: { type: "string", description: "Optional relay TTS voice (provider-specific, e.g. luna for Workers AI Aura)." },
        wait: { type: "boolean", description: `true: block until spoken (at most ${TALK_BUDGET_MS / 1000} s, then {job, status:"pending"} for molty_wait).` },
      },
      ["text"],
    ),
    auth: true,
    run: (api, t, a) => commandWait(api, t, "say", a),
  },
  {
    name: "molty_listen",
    description: `Record the device microphone (window default ${DEFAULT_LISTEN_MS} ms, maximum ${MAX_LISTEN_MS}; devices with end-of-speech detection stop early) and transcribe it with the relay's STT. Returns {job} immediately; molty_wait returns {text, duration_ms, peak, rms, reason, speech_ms?, timings}; wait:true blocks until done. The device shows listening while recording and thinking after speech ends. No audio file is returned.`,
    inputSchema: obj({ ms: { type: "integer", description: `Maximum capture in milliseconds (1..${MAX_LISTEN_MS}); default ${DEFAULT_LISTEN_MS}.` }, ...VAD_PROPS, wait: { type: "boolean", description: `true: block until transcribed (at most ${TALK_BUDGET_MS / 1000} s).` } }),
    auth: true,
    run: (api, t, a) => commandWait(api, t, "listen", a),
  },
  {
    name: "molty_talk",
    description: `One conversation turn in ONE call: speaks 'say' (if given), then listens and returns what the human said: {heard, said?, reason: silence|turn_end|no_speech|max|ended|error (turn_end = they finished, like silence), speech_ms?, ms:{tts_first_audio, speech, listen, stt, total}}. Moods are automatic (speaking, listening, then thinking until your next command, ${THINK_HOLD_S} s max): don't call molty_mood around it. Blocks at most ${TALK_BUDGET_MS / 1000} s; a longer turn returns {job, status:"pending"}: finish it with molty_wait. Keep 'say' to 1-2 sentences.`,
    inputSchema: obj({
      say: { type: "string", description: "What to say first (at most 4096 bytes). Omit to just listen." },
      listen: { type: "boolean", description: "Listen after speaking; default true. false = just speak (and wait until spoken)." },
      listen_ms: { type: "integer", description: `Maximum listen window in ms (1..${MAX_LISTEN_MS}); default ${TALK_DEFAULT_LISTEN_MS}. The device stops early when speech ends.` },
      voice: { type: "string", description: "Optional relay TTS voice." },
      ...VAD_PROPS,
    }),
    auth: true,
    run: (api, t, a) => api.talk(t, a),
  },
  {
    name: "molty_ask",
    description: `Ask the human a question they answer with a TAP on Molty's round screen: speaks 'say' (if given; put the question there), then shows 1-${MAX_ASK_OPTIONS} options and returns which one they tapped: {answer: label|null, index: int|null, reason?: timeout|cancelled|offline, said?, ms:{tts_first_audio?, speech?, answer, total}}. Options must be SHORT: 1-${MAX_ASK_LABEL} printable ASCII characters each, unique (good: "yes", "no", "later", "idk", "ok"; bad: "Yes, please do it", "maybe later"). 1 option = one centred button ("ok", "done"); 2 = halves; 3-${MAX_ASK_OPTIONS} = pie slices. answer null: nobody tapped within timeout_s (timeout), another command replaced the question (cancelled), or Molty went offline. Use molty_talk instead for open-ended answers. Blocks at most ${TALK_BUDGET_MS / 1000} s; longer returns {job, status:"pending"}: finish it with molty_wait.`,
    inputSchema: obj(
      {
        say: { type: "string", description: "The question, spoken first (at most 4096 bytes). Recommended." },
        options: {
          type: "array",
          items: {
            anyOf: [
              { type: "string" },
              { type: "object", properties: { label: { type: "string" }, color: { type: "string" } }, required: ["label"], additionalProperties: false },
            ],
          },
          description: `1..${MAX_ASK_OPTIONS} answers: a label (1..${MAX_ASK_LABEL} printable ASCII characters, unique ignoring case) or {label, color} with color one of ${Object.keys(COLORS).join(", ")} or "#rrggbb", e.g. [{"label":"yes","color":"green"},{"label":"no","color":"red"}]. Default colours by position: ${DEFAULT_ASK_COLORS.join(", ")}.`,
        },
        timeout_s: { type: "integer", description: `Seconds the options stay up waiting for a tap, 1..${MAX_ASK_TIMEOUT_S}; default ${DEFAULT_ASK_TIMEOUT_S}.` },
        voice: { type: "string", description: "Optional relay TTS voice for 'say'." },
      },
      ["options"],
    ),
    auth: true,
    run: (api, t, a) => api.ask(t, a),
  },
  {
    name: "molty_updates",
    description:
      "Check Molty for updates, in one call: use it whenever your human asks to check Molty for updates (any wording). Returns {skill:{yours, latest, whats_new:[{version, text, min_fw?}], url}, firmware:{current, board, online, latest|null, status: current|update_available|no_build_for_board|no_release|unknown, update_available, notes_url, needed_for:[{feature, what, needs, released}], can_self_update, release_check}, actions:[plain-language steps]}. Follow 'actions' in order: re-read the skill when it is newer, tell your human what's new in plain words, and when firmware is behind ASK them before calling molty_update. Marks the skill and the reported release as seen for you.",
    inputSchema: obj({}),
    auth: true,
    run: (api, t) => api.updates(t),
  },
  {
    name: "molty_update",
    description: `Ask Molty to install the latest official firmware release itself. Only after your human said yes: Molty also asks them on its screen (the version, e.g. "v0.2.0", or "later"), then downloads, verifies (sha256), installs and restarts (about a minute; it rolls back on failure). No arguments: the relay picks the release's image for this board. Returns {status:"up_to_date", current, latest} when there is nothing newer; else a job: done {outcome:"updated", from, to} or {outcome:"declined", reason?}; errors update_failed, rolled_back, update_timeout. Blocks at most ${TALK_BUDGET_MS / 1000} s, then {job, status:"pending", phase: confirm|accepted|downloading|flashing|rebooting}: keep calling molty_wait. Firmware without the 'update' cap: unsupported, with 'how' (the console's Update).`,
    inputSchema: obj({}),
    auth: true,
    run: (api, t) => api.update(t, {}),
  },
  {
    name: "molty_wait",
    description: "Wait at most timeout_s (0..8, default 8) for a say/listen/talk/ask/update job. Returns the job result with status done, status pending if still queued/running (call again), or an error if it failed.",
    inputSchema: obj(
      {
        job: { type: "string", description: "Job id returned by molty_say, molty_listen, molty_talk, molty_ask or molty_update." },
        timeout_s: { type: "integer", description: "Polling budget, integer 0..8 seconds; default 8." },
      },
      ["job"],
    ),
    auth: true,
    async run(api, t, a): Promise<Outcome<JsonObject>> {
      const s = a.timeout_s === undefined ? 8 : (a.timeout_s as number);
      if (!Number.isInteger(s) || s < 0 || s > 8) return { ok: false, error: "invalid_args", message: "timeout_s must be 0..8" };
      return jobOutcome(await api.job(t, a.job as string, s * 1000));
    },
  },
  {
    name: "molty_events",
    description: `Your unread Molty events (${AGENT_EVENT_TYPES.join(", ")}), oldest first: {events:[{id, type, ts, data}], next, more}. Default since = your read cursor (each event is returned to you once; it advances). wait_s (0..${INBOX_LIMITS.mcpWaitS}, default 0) long-polls until an event arrives. since = an event id (e.g. e41) re-reads after it. shake {peak_g, reversals, ms}; tap {x, y} in 0..1; button {id: accept|reject}; answer {job, index, label, reason?} (how a molty_ask ended).`,
    inputSchema: obj({
      since: { type: "string", description: "Event id to read after, e.g. e41; default your cursor." },
      wait_s: { type: "integer", description: `Seconds to wait for a new event, 0..${INBOX_LIMITS.mcpWaitS}; default 0.` },
    }),
    auth: true,
    async run(api, t, a) {
      const wait = a.wait_s === undefined ? 0 : (a.wait_s as number);
      if (!Number.isInteger(wait) || wait < 0 || wait > INBOX_LIMITS.mcpWaitS) return { ok: false, error: "invalid_args", message: `wait_s must be 0..${INBOX_LIMITS.mcpWaitS}` };
      const since = a.since === undefined ? undefined : seqOf(a.since);
      if (a.since !== undefined && since === undefined) return { ok: false, error: "invalid_args", message: "since must be an event id like e41" };
      return api.events(t, { since, wait_ms: wait * 1000 });
    },
  },
  {
    name: "molty_webhook",
    description:
      "Have the relay call YOU on each event: an HTTPS request you define. url (https, public host); method POST (default) | GET | PUT | PATCH; headers (your own auth; stored, never shown again); body template + content_type. Placeholders in url and body: {{event.type}} {{event.id}} {{event.ts}} {{event.data}} (JSON) {{event.data.<field>}} {{device}} {{text}} (e.g. 'Molty was shaken'), escaped for JSON / form / URL. No body = {\"device\",\"event\"} JSON. A missing field renders its {{event.data.x|default}}, else null in JSON / empty elsewhere. events (optional) limits the types sent. Every request is signed: Molty-Signature: t=<ms>,v2=hex(hmac_sha256(secret, t+'\\n'+METHOD+'\\n'+path?query+'\\n'+body)); the secret is returned once. Returns a dry run; the template is validated against every event type it will receive. Omit url to read the current webhook; remove:true deletes it. Retries 1 s/10 s/60 s; disabled after 20 consecutive failures (re-register to re-enable).",
    inputSchema: obj({
      url: { type: "string", description: "https URL the relay requests per event; may contain placeholders (URL-encoded)." },
      method: { type: "string", description: "POST (default), GET, PUT or PATCH.", enum: ["POST", "GET", "PUT", "PATCH"] },
      headers: { type: "object", description: "Extra request headers, e.g. your API key. Values are stored and never echoed." },
      body: { type: "string", description: "Body template (max 4096 bytes); default is the relay's signed {device, event} JSON." },
      content_type: { type: "string", description: "Body media type; application/json escapes placeholders as JSON strings." },
      events: { type: "array", items: { type: "string", enum: AGENT_EVENT_TYPES }, description: `Event types to send (default all): ${AGENT_EVENT_TYPES.join(", ")}.` },
      remove: { type: "boolean", description: "true removes your webhook." },
    }),
    auth: true,
    run(api, t, a) {
      if (a.remove === true) return api.webhook(t, { action: "delete" });
      if (a.url === undefined) return api.webhook(t, { action: "get" });
      const { remove: _r, ...spec } = a;
      return api.webhook(t, { action: "set", spec });
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
      prop.type === "string"
        ? typeof v === "string"
        : prop.type === "integer"
          ? Number.isInteger(v)
          : prop.type === "number"
            ? typeof v === "number" && Number.isFinite(v)
          : prop.type === "object"
            ? isRecord(v)
            : prop.type === "boolean"
              ? typeof v === "boolean"
              : prop.type === "array"
                ? Array.isArray(v) && ((prop.items !== undefined && "anyOf" in prop.items) || v.every((x) => typeof x === "string" && (!items(prop)?.enum || items(prop)!.enum!.includes(x))))
                : true;
    // anyOf items (molty_ask options): the command's own validation checks each one
    if (!ok && prop.type === "array") return `${k} must be a list of ${items(prop)?.enum ? items(prop)!.enum!.join(", ") : prop.items && "anyOf" in prop.items ? "items" : "strings"}`;
    if (!ok) return `${k} must be ${prop.type === "integer" ? "an integer" : `a${prop.type === "object" ? "n" : ""} ${prop.type}`}`;
    if (prop.enum && !prop.enum.includes(v as string)) return `${k} must be one of ${prop.enum.join(", ")}`;
  }
  for (const k of schema.required) if (!(k in args)) return `missing required argument ${k}`;
  return undefined;
}

type RpcMessage = { jsonrpc?: unknown; id?: unknown; method?: unknown; params?: unknown };
const rpcError = (id: unknown, code: number, message: string) => ({ jsonrpc: "2.0", id: id ?? null, error: { code, message } });
const rpcResult = (id: unknown, result: unknown) => ({ jsonrpc: "2.0", id, result });

/**
 * Unread events ride along additively: `events` (+ `more_events`) in the
 * structured result and one extra text item ("Molty: was shaken ... 2 min ago").
 * Results without unread events are byte-for-byte what they were before.
 */
function toolResult(out: Outcome<JsonObject>, origin = "") {
  const value: JsonObject = out.ok ? { ok: true, ...out.result } : { ok: false, error: out.error, ...(out.message ? { message: out.message } : {}), ...(out.details ?? {}) };
  let line: string | undefined;
  if (out.inbox && out.inbox.events.length > 0 && !("events" in value)) {
    value.events = out.inbox.events as unknown as JsonObject[];
    if (out.inbox.more) value.more_events = true;
    line = eventsLine(out.inbox.events, out.inbox.now, out.inbox.more);
  } else if (out.ok && Array.isArray(value.events) && value.events.length > 0 && "next" in value) {
    line = eventsLine(value.events as unknown as AgentEvent[], Date.now(), value.more === true); // molty_events itself
  }
  // One-time skill notice (already marked seen by the backend): structured field + one line.
  if (out.notice && !("skill_update" in value)) value.skill_update = skillUpdate(out.notice, origin) as unknown as JsonObject;
  // One-time firmware release notice (already marked seen): structured field + one line.
  if (out.fw_notice && !("firmware_update" in value)) value.firmware_update = out.fw_notice as unknown as JsonObject;
  const content = [{ type: "text", text: JSON.stringify(value) }];
  if (line) content.push({ type: "text", text: line });
  if (out.notice) content.push({ type: "text", text: skillLine(out.notice, origin) });
  if (out.fw_notice) content.push({ type: "text", text: firmwareLine(out.fw_notice) });
  return { content, structuredContent: value, isError: !out.ok };
}

/** The one-line MCP text item for a new firmware release. */
export const firmwareLine = (n: FirmwareNotice): string =>
  `Molty firmware ${n.latest} is out (Molty runs ${n.current}). Mention it to your human once and ask before updating; molty_updates has the details.`;

async function callTool(backend: RelayBackend, token: string | undefined, params: unknown, origin: string) {
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
    // Ride-along events were already marked read by the backend: whatever a
    // tool does to reshape the outcome (molty_wait builds new ones), the
    // inbox it received must reach the caller.
    // Same for the skill notice.
    const { api, taken, notices, fwNotices } = capturingInbox(backend.device(deviceId));
    let out = await tool.run(api, token ?? "", args);
    if (taken.length && !out.inbox) out = { ...out, inbox: mergeInbox(taken) };
    if (notices.length && !out.notice) out = { ...out, notice: notices[notices.length - 1] };
    if (fwNotices.length && !out.fw_notice) out = { ...out, fw_notice: fwNotices[fwNotices.length - 1] };
    return { result: toolResult(out, origin) };
  } catch (e) {
    return { result: toolResult({ ok: false, error: "internal", message: e instanceof Error ? e.message : String(e) }) };
  }
}

/** Wraps an AgentApi so every inbox and notice any call returns is recorded. */
function capturingInbox(api: AgentApi): { api: AgentApi; taken: Inbox[]; notices: SkillNotice[]; fwNotices: FirmwareNotice[] } {
  const taken: Inbox[] = [];
  const notices: SkillNotice[] = [];
  const fwNotices: FirmwareNotice[] = [];
  const keep = async <T extends Outcome<any>>(p: Promise<T>) => {
    const out = await p;
    if (out.inbox && out.inbox.events.length) taken.push(out.inbox);
    if (out.notice) notices.push(out.notice);
    if (out.fw_notice) fwNotices.push(out.fw_notice);
    return out;
  };
  return {
    taken,
    notices,
    fwNotices,
    api: {
      pair: (req) => keep(api.pair(req)),
      command: (t, n, a) => keep(api.command(t, n, a)),
      job: (t, j, w) => keep(api.job(t, j, w)),
      revoke: (t) => keep(api.revoke(t)),
      events: (t, r) => keep(api.events(t, r)),
      webhook: (t, r) => keep(api.webhook(t, r)),
      talk: (t, r) => keep(api.talk(t, r)),
      ask: (t, r) => keep(api.ask(t, r)),
      updates: (t) => keep(api.updates(t)),
      update: (t, r) => keep(api.update(t, r)),
    },
  };
}

function mergeInbox(list: Inbox[]): Inbox {
  return { events: list.flatMap((i) => i.events), more: list.some((i) => i.more), now: Math.max(...list.map((i) => i.now)) };
}

async function handleOne(msg: unknown, backend: RelayBackend, token: string | undefined, origin: string): Promise<object | undefined> {
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
      const out = await callTool(backend, token, m.params, origin);
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
  const origin = new URL(request.url).origin;
  if (Array.isArray(body)) {
    // Batches were allowed in 2025-03-26 (removed later); accept for older clients.
    if (body.length === 0) return json(rpcError(null, -32600, "Invalid Request"), 400);
    const replies = (await Promise.all(body.map((m) => handleOne(m, backend, token, origin)))).filter(Boolean);
    return replies.length ? json(replies) : new Response(null, { status: 202 });
  }
  const reply = await handleOne(body, backend, token, origin);
  return reply ? json(reply) : new Response(null, { status: 202 });
}
