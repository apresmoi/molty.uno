// SPDX-License-Identifier: MIT
// Per-agent notes (docs/AGENT-LINK.md "Notes"): a small private key-value store per agent token,
// so an agent without memory of its own can keep its behaviour ledger (what it tried — a react, an
// opener — and how the human responded) across sessions. One KV document per token, `note:<token
// hash>`: only that token reads or writes it; revoking the token or forgetting the device deletes
// it. Never logged (the activity log keeps the op and the key, not the value). Host-neutral.

import { isRecord, type JsonObject } from "./protocol.js";
import type { KV } from "./store.js";

export const NOTES_LIMITS = {
  keys: 64,
  /** One value, as JSON. */
  valueBytes: 8 * 1024,
  /** Every value of one token together, as JSON. */
  totalBytes: 32 * 1024,
};
export const NOTE_KEY_RE = /^[A-Za-z0-9._:-]{1,64}$/;
/** Every notes key starts with this (revoke / forget delete it). */
export const NOTES_PREFIX = "note:";
export const notesKey = (hash: string) => `${NOTES_PREFIX}${hash}`;

type JsonValue = JsonObject[string];
interface Note {
  v: JsonValue;
  at: number;
  bytes: number;
}
type Doc = Record<string, Note>;

export type NotesRequest = { action: "list" } | { action: "get"; key: unknown } | { action: "put"; key: unknown; value: unknown } | { action: "delete"; key: unknown };
export type NotesResult = { ok: true; result: JsonObject } | { ok: false; error: string; message: string };

const enc = new TextEncoder();
const bad = (message: string, error = "invalid_args"): NotesResult => ({ ok: false, error, message });

function checkKey(key: unknown): string | NotesResult {
  if (typeof key !== "string" || !NOTE_KEY_RE.test(key)) return bad("key must be 1..64 characters of letters, digits and . _ : -");
  return key;
}

/** JSON-able only (no undefined / functions / non-finite numbers), at most 8 levels deep. */
function jsonValue(v: unknown, depth = 0): boolean {
  if (v === null || typeof v === "string" || typeof v === "boolean") return true;
  if (typeof v === "number") return Number.isFinite(v);
  if (depth >= 8) return false;
  if (Array.isArray(v)) return v.every((x) => jsonValue(x, depth + 1));
  if (isRecord(v)) return Object.values(v).every((x) => jsonValue(x, depth + 1));
  return false;
}

const usage = (doc: Doc) => Object.values(doc).reduce((n, x) => n + x.bytes, 0);

function listing(doc: Doc): JsonObject {
  return {
    keys: Object.entries(doc)
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([key, n]) => ({ key, bytes: n.bytes, updated: n.at })),
    bytes: usage(doc),
    limits: { keys: NOTES_LIMITS.keys, value_bytes: NOTES_LIMITS.valueBytes, total_bytes: NOTES_LIMITS.totalBytes },
  };
}

/** One notes operation for the token `hash` (already authenticated). */
export async function notesOp(kv: KV, hash: string, req: NotesRequest, now: number): Promise<NotesResult> {
  const k = notesKey(hash);
  if (req.action === "list") return { ok: true, result: listing((await kv.get<Doc>(k)) ?? {}) };
  const key = checkKey(req.key);
  if (typeof key !== "string") return key;
  if (req.action === "get") {
    const n = ((await kv.get<Doc>(k)) ?? {})[key];
    return { ok: true, result: n ? { key, value: n.v, updated: n.at } : { key, value: null, updated: null } };
  }
  if (req.action === "delete") {
    const removed = await kv.update<Doc, boolean>(k, (doc) => {
      if (!doc || !Object.hasOwn(doc, key)) return { result: false };
      const next = { ...doc };
      delete next[key];
      return { next: Object.keys(next).length ? next : null, result: true };
    });
    return { ok: true, result: { key, removed } };
  }
  const value = req.value;
  if (value === undefined || !jsonValue(value)) return bad("value must be JSON (a string, number, boolean, null, list or object)");
  const bytes = enc.encode(JSON.stringify(value)).byteLength;
  if (bytes > NOTES_LIMITS.valueBytes) return bad(`value is ${bytes} bytes; at most ${NOTES_LIMITS.valueBytes} per key`, "too_large");
  // read-check-write in one KV.update: two concurrent puts can never overshoot the caps together
  const out = await kv.update<Doc, NotesResult>(k, (doc) => {
    const cur = doc ?? {};
    const others = Object.entries(cur).filter(([key2]) => key2 !== key);
    if (!Object.hasOwn(cur, key) && others.length >= NOTES_LIMITS.keys) return { result: bad(`at most ${NOTES_LIMITS.keys} keys: delete one first`, "too_large") };
    const total = others.reduce((n, [, x]) => n + x.bytes, 0) + bytes;
    if (total > NOTES_LIMITS.totalBytes) return { result: bad(`your notes would hold ${total} bytes; at most ${NOTES_LIMITS.totalBytes} in all: delete or shorten some`, "too_large") };
    const next: Doc = { ...cur, [key]: { v: value as JsonValue, at: now, bytes } };
    return { next, result: { ok: true, result: { key, bytes, total_bytes: total } } };
  });
  return out;
}
