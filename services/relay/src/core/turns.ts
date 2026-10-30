// SPDX-License-Identifier: MIT
// Shake to talk (docs/AGENT-LINK.md "Shake to talk"): the human shakes Molty, Molty starts
// listening by itself, the relay adopts that listen as a job with the normal voice path and,
// when the human said something, emits ONE `utterance` event (the turn). The first agent
// whose say / talk answers it claims the turn; for TURN_HOLD_MS other agents' say / talk get
// `turn_claimed`. Host-neutral and pure: room.ts keeps the record (KV `turn`) and calls these.

import type { JsonObject } from "./protocol.js";

/** Relay feature in the device's welcome: it adopts a listen the device began by itself. */
export const LISTEN_ADOPT_FEATURE = "listen_adopt";
/** Firmware cap: the device can start a listen on a shake (the human's setting decides each time). */
export const SHAKE_TALK_CAP = "shake_talk";
/** The device's local id for the listen it began (firmware: "shake-<n>"). */
export const LOCAL_LISTEN_RE = /^[A-Za-z0-9_-]{1,32}$/;
/** After an utterance, how long the first agent to answer owns Molty's voice (other agents: turn_claimed). */
export const TURN_HOLD_MS = 60000;

/** The latest human turn (KV `turn`): its id is the listen job's id. */
export interface TurnRecord {
  id: string;
  /** When its utterance was emitted (relay clock). */
  at: number;
  /** Token hash of the agent that claimed it (its first say / talk). */
  by?: string;
}

/** What a say / talk may do about the open turn. `turn`: the turn it answers (for timings). */
export type TurnClaim = { ok: true; turn?: { id: string; at: number }; next?: TurnRecord } | { ok: false; turn: string };

/**
 * A say / talk from agent `hash` (optionally `in_reply_to`) against the latest turn:
 * - no turn, or older than TURN_HOLD_MS: go ahead (nothing to claim);
 * - in_reply_to names an older turn: go ahead, uncorrelated (the human has moved on);
 * - unclaimed: this call claims it (`next`) and answers it;
 * - claimed by this agent: go ahead, answering it;
 * - claimed by another agent: refused (turn_claimed) — with or without in_reply_to.
 */
export function claimTurn(cur: TurnRecord | undefined, hash: string, inReplyTo: string | undefined, now: number): TurnClaim {
  if (!cur || now - cur.at > TURN_HOLD_MS) return { ok: true };
  if (inReplyTo !== undefined && inReplyTo !== cur.id) return { ok: true };
  const turn = { id: cur.id, at: cur.at };
  if (!cur.by) return { ok: true, turn, next: { ...cur, by: hash } };
  if (cur.by === hash) return { ok: true, turn };
  return { ok: false, turn: cur.id };
}

/** Timings of an answer to a turn: the call `reply` ms after the utterance, its first audio `first_audio` ms after it. */
export function turnTimings(turn: { id: string; at: number }, callAt: number, firstAudioAt?: number): JsonObject {
  return { id: turn.id, reply: callAt - turn.at, ...(firstAudioAt !== undefined ? { first_audio: firstAudioAt - turn.at } : {}) };
}

export interface UtteranceInput {
  turn_id: string;
  text: string;
  reason: string;
  speech_ms: number;
  stt_ms: number;
  trigger: "shake";
  caps: string[];
  /** The device's acknowledgement for this listen ("chirp" | "thinking" | "off"), absent on older firmware. */
  ack?: string;
  voice: string;
}

/** The `utterance` event's data: everything an agent needs to answer without another call. */
export function utteranceData(u: UtteranceInput): JsonObject {
  return {
    turn_id: u.turn_id,
    text: u.text,
    reason: u.reason,
    ms: { speech: u.speech_ms, stt: u.stt_ms },
    trigger: u.trigger,
    // what Molty shows now: thinking (its own acknowledgement, or the relay's) unless turned off
    device: { caps: u.caps, mood: u.ack === "off" ? "idle" : "thinking", voice: u.voice },
  };
}
