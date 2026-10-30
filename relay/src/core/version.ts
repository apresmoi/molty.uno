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

// ---- agent skill version (what /skill.md teaches) ----
// Single source of truth: bump by appending to WHATS_NEW. skill.md's header, the
// MCP instructions/serverInfo and the one-time "skill_update" ride-along notice
// all derive from it; test/skill-version.test.ts fails if they disagree.

/** Ordered oldest -> newest. v1 is the original skill (tokens with no recorded version saw v1). */
export const WHATS_NEW: ReadonlyArray<{ version: number; text: string }> = [
  { version: 2, text: "device events: shake, tap, button; inbox, molty_events, webhooks, molty events CLI; mood hold_s" },
  { version: 3, text: "molty_talk (one call per conversation turn: say, listen, transcript), faster streamed speech, automatic moods around talking, wait:true on say/listen" },
  { version: 4, text: "molty_ask (ask a question, the human answers with a tap on one of 1-5 short options)" },
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
