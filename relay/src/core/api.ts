// Surface-neutral agent API. Surfaces (REST, MCP, later OpenClaw node) speak
// only this; hosts (Durable Object, Node) implement RelayBackend.

import type { AgentEvent } from "./events.js";
import type { JsonObject } from "./protocol.js";
import type { SkillNotice } from "./version.js";

export type ErrorCode =
  | "unauthorized"
  | "invalid_args"
  | "unknown_command"
  | "device_offline"
  | "device_timeout"
  | "audio_busy"
  | "turn_claimed"
  | "unsupported"
  | "not_found"
  | "bad_link_code"
  | "pair_busy"
  | "rejected"
  | "timeout"
  | "firmware_too_old"
  | "release_unavailable"
  | "no_build"
  | "no_checksum"
  | "unknown_version"
  | "forbidden"
  | "voice_box_offline"
  | "internal"
  | (string & {}); // device-reported codes pass through verbatim

/** Unread device events riding along on a response (surfaces render them additively). */
export interface Inbox {
  events: AgentEvent[];
  /** More unread events remain than were attached. */
  more: boolean;
  /** Relay time the inbox was read, unix ms (for "2 min ago"). */
  now: number;
}

/** One-time "newer firmware is released" notice (core/updates.ts), once per agent token per release. */
export interface FirmwareNotice {
  current: string;
  latest: string;
  board: string;
  notes_url: string;
  can_self_update: boolean;
  how: string;
}

/**
 * `notice`: a one-time "the skill changed" notice riding along on a successful
 * authenticated outcome (already marked seen); `fw_notice` likewise for a new
 * firmware release. Surfaces render both additively. A failure's `details` are
 * extra structured fields of that error (e.g. firmware_too_old {current, needed,
 * latest, how}), rendered next to `error`.
 */
export type Outcome<T = JsonObject> = ({ ok: true; result: T } | { ok: false; error: ErrorCode; message?: string; details?: JsonObject }) & {
  inbox?: Inbox;
  notice?: SkillNotice;
  fw_notice?: FirmwareNotice;
};

export interface EventsRequest {
  /** Event id or sequence to read after; default = this agent's cursor. */
  since?: number;
  /** Long-poll budget (ms); the surface caps it. */
  wait_ms: number;
  /** The caller went away: free the waiter now. Not serializable, so DO RPC hosts drop it. */
  signal?: AbortSignal;
}

export type WebhookRequest = { action: "get" } | { action: "delete" } | { action: "set"; spec: Record<string, unknown> };

export type JobStatus = "queued" | "running" | "done" | "failed";
export const TERMINAL: readonly JobStatus[] = ["done", "failed"];

export interface JobView {
  job: string;
  kind: "say" | "listen" | "talk" | "ask" | "update";
  status: JobStatus;
  result?: JsonObject;
  error?: string;
  [k: string]: unknown;
}

export interface PairRequest {
  device_id: string;
  link_code?: string;
  agent_name?: string;
  /** Resume waiting on an earlier request that returned `pending`. */
  pair_id?: string;
  /** How long to hold the call open (ms). Capped at the pairing window. */
  wait_ms?: number;
  /**
   * What the token will be for: `agent` (default) drives the device through the agent
   * surfaces; `voice` is a voice box (docs/AGENT-LINK.md "Voice box"): its token opens
   * only the voice socket and is refused everywhere else. Same tap on the device.
   */
  role?: "agent" | "voice";
}

export type PairResult =
  | { status: "approved"; agent_token: string; device_id: string; agent_name: string; code: string; role?: "voice" }
  | { status: "pending"; pair_id: string; code: string; expires_in_s: number };

export interface AgentApi {
  pair(req: PairRequest): Promise<Outcome<PairResult>>;
  command(token: string, name: string, args: unknown): Promise<Outcome>;
  job(token: string, jobId: string, waitMs: number): Promise<Outcome<JobView>>;
  revoke(token: string): Promise<Outcome>;
  /** Pull this agent's events: `{events, next, more}`; advances the agent's cursor. */
  events(token: string, req: EventsRequest): Promise<Outcome>;
  /** Read, register (returns the signing secret once) or remove this agent's webhook. */
  webhook(token: string, req: WebhookRequest): Promise<Outcome>;
  /**
   * One conversational turn: speak `say` (optional), then listen, and block
   * (bounded, TALK_BUDGET_MS) for the transcript. Done: `{job, status:"done",
   * heard, said?, reason, ms:{…}, timings}`; over budget: `{job,
   * status:"pending"}` to finish with job(). Args as validateTalk (commands.ts).
   */
  talk(token: string, req: unknown): Promise<Outcome>;
  /**
   * A question with tap answers: speak `say` (optional), show 1..5 short
   * options, wait for the human's tap (bounded, TALK_BUDGET_MS). Done: `{job,
   * status:"done", answer: label|null, index: int|null, reason?:
   * "timeout"|"cancelled"|"offline", said?, ms, timings}`; over budget: `{job,
   * status:"pending"}` to finish with job(). Args as validateAsk (commands.ts).
   */
  ask(token: string, req: unknown): Promise<Outcome>;
  /**
   * "Check Molty for updates": `{skill:{yours, latest, whats_new, url}, firmware:
   * FirmwareBlock, actions:[…]}` (core/updates.ts). Marks the skill (and the
   * reported firmware release) as seen for this token.
   */
  updates(token: string): Promise<Outcome>;
  /**
   * Ask the device to install the latest release (official asset of the configured
   * repo only; args must be empty). A job: done `{outcome:"updated", from, to}` or
   * `{outcome:"declined", reason?}`; failed `update_failed` / `rolled_back` /
   * `update_timeout`; blocks <= TALK_BUDGET_MS, then `{job, status:"pending", phase}`.
   * Already current: `{status:"up_to_date", current, latest}` (no job).
   */
  update(token: string, req: unknown): Promise<Outcome>;
}

export type VoiceMode = "auto" | "box" | "cloud";
export const VOICE_MODES: readonly VoiceMode[] = ["auto", "box", "cloud"];

/** Owner settings for the voice box: read, change (mode / default box voice), or unpair the box. */
export type VoiceOwnerRequest = { action: "get" } | { action: "set"; mode?: unknown; voice?: unknown } | { action: "unpair" };

/**
 * The device owner's calls (the setup console). Authenticated by the device's CURRENT
 * link code: the console reads it from the device over the LAN (GET /api/link), the
 * same "on Molty's network = owner" model as every local device setting.
 */
export interface OwnerApi {
  /** get / set: `{mode, voice, cloud, box:{paired, name, connected, …}}`; unpair: `{unpaired: n}`. */
  voiceSettings(linkCode: string, req: VoiceOwnerRequest): Promise<Outcome>;
}

export interface RelayBackend {
  /** `id` is already validated against DEVICE_ID_RE by the caller. */
  device(id: string): AgentApi & Partial<OwnerApi>;
}

/** `agent_token` = `<device_id>.<base64url(32 random bytes)>`; the prefix is a public routing hint. */
export function tokenDevice(token: string | undefined): string | undefined {
  if (!token) return undefined;
  const m = /^(molty-[a-z2-9]{10})\.[A-Za-z0-9_-]{43}$/.exec(token);
  return m?.[1];
}

/** job id = `<device_id>.j_<random>` so it routes without a lookup. */
export function jobDevice(job: string | undefined): string | undefined {
  if (!job) return undefined;
  const m = /^(molty-[a-z2-9]{10})\.j_[A-Za-z0-9_-]{8,32}$/.exec(job);
  return m?.[1];
}

export function bearer(header: string | null | undefined): string | undefined {
  if (!header) return undefined;
  const m = /^Bearer\s+(\S+)\s*$/i.exec(header);
  return m?.[1];
}
