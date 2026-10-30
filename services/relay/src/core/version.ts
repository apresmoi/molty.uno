// SPDX-License-Identifier: MIT
// Product version (repo-root VERSION, see tools/scripts/version.mjs), baked in at bundle
// time: `pnpm run deploy` (tools/deploy.mjs) passes `--define __MOLTY_VERSION__:"X.Y.Z"`
// to wrangler and vitest.config.ts defines it for tests. A bundle built without it
// (plain `wrangler dev`/`deploy`) reports "dev" instead of failing.
declare const __MOLTY_VERSION__: string | undefined;

export const RELAY_VERSION: string = typeof __MOLTY_VERSION__ === "string" ? __MOLTY_VERSION__ : "dev";

/**
 * The agent API contract this relay speaks: an integer, bumped only when an agent
 * built for the previous one would break or miss something it must handle. It is
 * stated where agents already look (pair result, status `relay`, GET /v1/health,
 * molty_updates, MCP serverInfo). The agent skill is versioned and distributed
 * separately (the molty plugin, skills/molty/ in the repo): the relay never serves
 * it and never tells agents to re-read it. What a Molty can do is `relay.caps`.
 */
export const API_VERSION = 1;

/** GET /v1/health: liveness + version, for deploy checks and agents. */
export function healthResponse(): Response {
  return new Response(JSON.stringify({ ok: true, service: "molty-relay", version: RELAY_VERSION, api_version: API_VERSION }), {
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
  act: { tool: "molty_act", cap: "act", min_fw: NEXT_FW, what: "gestures (nod, wiggle, …)" },
} as const satisfies Record<string, Feature>;

export type FeatureId = keyof typeof FEATURES;
