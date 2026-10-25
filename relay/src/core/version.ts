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
