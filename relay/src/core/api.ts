// Surface-neutral agent API. Surfaces (REST, MCP, later OpenClaw node) speak
// only this; hosts (Durable Object, Node) implement RelayBackend.

import type { JsonObject } from "./protocol.js";

export type ErrorCode =
  | "unauthorized"
  | "invalid_args"
  | "unknown_command"
  | "device_offline"
  | "device_timeout"
  | "audio_busy"
  | "unsupported"
  | "not_found"
  | "bad_link_code"
  | "pair_busy"
  | "rejected"
  | "timeout"
  | "internal"
  | (string & {}); // device-reported codes pass through verbatim

export type Outcome<T = JsonObject> = { ok: true; result: T } | { ok: false; error: ErrorCode; message?: string };

export type JobStatus = "queued" | "running" | "done" | "failed";
export const TERMINAL: readonly JobStatus[] = ["done", "failed"];

export interface JobView {
  job: string;
  kind: "say" | "listen";
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
}

export type PairResult =
  | { status: "approved"; agent_token: string; device_id: string; agent_name: string; code: string }
  | { status: "pending"; pair_id: string; code: string; expires_in_s: number };

export interface AgentApi {
  pair(req: PairRequest): Promise<Outcome<PairResult>>;
  command(token: string, name: string, args: unknown): Promise<Outcome>;
  job(token: string, jobId: string, waitMs: number): Promise<Outcome<JobView>>;
  revoke(token: string): Promise<Outcome>;
}

export interface RelayBackend {
  /** `id` is already validated against DEVICE_ID_RE by the caller. */
  device(id: string): AgentApi;
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
