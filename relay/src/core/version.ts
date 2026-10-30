// SPDX-License-Identifier: MIT
// Product version (repo-root VERSION, see scripts/version.mjs), baked in at bundle
// time: `pnpm run deploy` (tools/deploy.mjs) passes `--define __MOLTY_VERSION__:"X.Y.Z"`
// to wrangler and vitest.config.ts defines it for tests. A bundle built without it
// (plain `wrangler dev`/`deploy`) reports "dev" instead of failing.
declare const __MOLTY_VERSION__: string | undefined;

export const RELAY_VERSION: string = typeof __MOLTY_VERSION__ === "string" ? __MOLTY_VERSION__ : "dev";

/** GET /v1/health: liveness + version, for deploy checks and agents. */
export function healthResponse(): Response {
  return new Response(JSON.stringify({ ok: true, service: "molty-relay", version: RELAY_VERSION }), {
    headers: { "content-type": "application/json", "cache-control": "no-store", "x-molty-version": RELAY_VERSION },
  });
}

// ---- product versions (VERSION scheme: X.Y.Z[-pre]) ----

interface Semver {
  core: [number, number, number];
  pre: string[];
}

/** "1.2.3", "v1.2.3", "1.2.3-rc.1" (build metadata ignored); undefined for anything else ("dev", "test-fw"). */
export function parseVersion(v: unknown): Semver | undefined {
  if (typeof v !== "string") return undefined;
  const m = /^v?(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z.-]+)?$/.exec(v.trim());
  if (!m) return undefined;
  return { core: [Number(m[1]), Number(m[2]), Number(m[3])], pre: m[4] ? m[4].split(".") : [] };
}

/** Semver precedence: <0, 0, >0; undefined when either side is not a version. */
export function compareVersions(a: unknown, b: unknown): number | undefined {
  const x = parseVersion(a);
  const y = parseVersion(b);
  if (!x || !y) return undefined;
  for (let i = 0; i < 3; i++) if (x.core[i] !== y.core[i]) return x.core[i] - y.core[i];
  if (!x.pre.length || !y.pre.length) return y.pre.length - x.pre.length; // a release outranks its prereleases
  for (let i = 0; i < Math.max(x.pre.length, y.pre.length); i++) {
    const p = x.pre[i];
    const q = y.pre[i];
    if (p === undefined) return -1;
    if (q === undefined) return 1;
    if (p === q) continue;
    const pn = /^\d+$/.test(p);
    const qn = /^\d+$/.test(q);
    if (pn && qn) return Number(p) - Number(q);
    if (pn !== qn) return pn ? -1 : 1;
    return p < q ? -1 : 1;
  }
  return 0;
}

// ---- device features that need a firmware version ----
// The ONE place a feature's minimum firmware is stated. `cap` is what the device
// advertises in hello.caps when it can do it: caps stay the truth for "can this
// device do X"; min_fw only explains why not (firmware_too_old vs unsupported).

/** The firmware release that first ships the device side of ask + self-update. */
export const NEXT_FW = "0.2.0";

export interface Feature {
  /** Agent-facing tool. */
  tool: string;
  /** Capability the device lists in hello.caps once its firmware (and board) can do it. */
  cap: string;
  min_fw: string;
  /** Short plain words for the human. */
  what: string;
}

export const FEATURES = {
  ask: { tool: "molty_ask", cap: "ask", min_fw: NEXT_FW, what: "questions answered with a tap" },
  update: { tool: "molty_update", cap: "update", min_fw: NEXT_FW, what: "Molty updating its own firmware" },
} as const satisfies Record<string, Feature>;

export type FeatureId = keyof typeof FEATURES;

// ---- agent skill version (what /skill.md teaches) ----
// Single source of truth: bump by appending to WHATS_NEW. skill.md's header, the
// MCP instructions/serverInfo and the one-time "skill_update" ride-along notice
// all derive from it; test/skill-version.test.ts fails if they disagree.

/**
 * Ordered oldest -> newest. v1 is the original skill (tokens with no recorded version saw v1).
 * `min_fw`: the device firmware this entry's feature needs (from FEATURES), so agents can tell
 * their human which firmware update unlocks it.
 */
export const WHATS_NEW: ReadonlyArray<{ version: number; text: string; min_fw?: string }> = [
  { version: 2, text: "device events: shake, tap, button; inbox, molty_events, webhooks, molty events CLI; mood hold_s" },
  { version: 3, text: "molty_talk (one call per conversation turn: say, listen, transcript), faster streamed speech, automatic moods around talking, wait:true on say/listen" },
  { version: 4, text: "molty_ask (ask a question, the human answers with a tap on one of 1-5 short options)", min_fw: FEATURES.ask.min_fw },
  { version: 5, text: "molty_updates (check Molty for updates: skill and firmware, in one call), molty_update (Molty updates its own firmware after the human confirms), firmware_too_old errors" },
  { version: 6, text: "molty_ask options can carry colours; ring_color on listen/talk" },
  { version: 7, text: "listen windows up to 30 s, or 0 = unlimited (ends when the human finishes); omit ms / listen_ms for the human's own setting (30 s out of the box)" },
  { version: 8, text: "end of turn is automatic: with the owner's voice box its semantic end of turn decides (device VAD off, no_speech from the box too) — don't pass vad / end_silence_ms" },
  { version: 9, text: "check relay.caps (not firmware versions) to know what Molty can do; button accept/reject = acknowledge / dismiss, never an answer (answers come only from molty_ask); a broken listen is an error (listen_failed / device_offline), never no_speech; after a wake, meet no_speech with one short line or one more listen, not silence" },
  { version: 10, text: "Molty acknowledges the end of the human's turn by itself (a soft chirp + thinking, the human's setting) — go straight to your answer; for long work still say a quick ack or keep thinking up; ack:false|\"silent\" on listen/talk opts out" },
  { version: 11, text: "molty_activity (REST GET /v1/activity): debug yourself — your own calls, jobs (stage timings, voice provider, reason), Molty's log lines for your jobs and its disconnects, 24 h; failed talk/ask/update errors carry their job id" },
];
export const SKILL_VERSION: number = WHATS_NEW[WHATS_NEW.length - 1].version;
export const SKILL_PATH = "/skill.md";
/** Version assumed for an agent token that never recorded one (paired before skill versions existed). */
const BASELINE_SKILL_VERSION = 1;

/** What changed since `seen`, or undefined when the agent is current. The surface adds `url`. */
export interface SkillNotice {
  version: number;
  whats_new: string[];
}

export function skillNotice(seen: number | undefined): SkillNotice | undefined {
  const from = seen ?? BASELINE_SKILL_VERSION;
  if (from >= SKILL_VERSION) return undefined;
  return { version: SKILL_VERSION, whats_new: WHATS_NEW.filter((e) => e.version > from).map((e) => `v${e.version}: ${e.text}`) };
}

/** "Skill v2 — what's new: …" (skill.md header, MCP instructions). */
export const skillHeadline = (): string => `Skill v${SKILL_VERSION} — what's new: ${WHATS_NEW[WHATS_NEW.length - 1].text}`;

/** Additive response field: `{version, whats_new, url}`. */
export const skillUpdate = (n: SkillNotice, origin: string) => ({ version: n.version, whats_new: n.whats_new, url: `${origin.replace(/\/+$/, "")}${SKILL_PATH}` });

/** The one-line MCP text item. */
export const skillLine = (n: SkillNotice, origin: string): string => `Molty's skill was updated (${n.whats_new.join("; ")}). Re-read ${skillUpdate(n, origin).url}.`;
