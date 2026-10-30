// SPDX-License-Identifier: MIT
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { API_VERSION, RELAY_VERSION, healthResponse } from "../src/core/version.js";
import { SERVER_INFO } from "../src/surfaces/mcp.js";

const VERSION = readFileSync(new URL("../../../VERSION", import.meta.url), "utf8").trim();

describe("version", () => {
  it("reports the repo-root VERSION everywhere", async () => {
    expect(RELAY_VERSION).toBe(VERSION);
    expect(SERVER_INFO.version).toBe(VERSION);
    const res = healthResponse();
    expect(res.headers.get("x-molty-version")).toBe(VERSION);
    expect(await res.json()).toEqual({ ok: true, service: "molty-relay", version: VERSION, api_version: API_VERSION });
  });
  it("routes /v1/health before the REST surface swallows /v1/*", () => {
    // worker.ts imports cloudflare:workers (not loadable here): check the route statically.
    const worker = readFileSync(new URL("../src/worker.ts", import.meta.url), "utf8");
    const health = worker.indexOf('url.pathname === "/v1/health"');
    expect(health).toBeGreaterThan(0);
    expect(health).toBeLessThan(worker.indexOf('url.pathname.startsWith("/v1/")'));
    const releases = worker.indexOf("url.pathname === RELEASES_PATH");
    expect(releases).toBeGreaterThan(0);
    expect(releases).toBeLessThan(worker.indexOf('url.pathname.startsWith("/v1/")'));
  });
  it("tools/deploy.mjs bakes the version into the bundle", () => {
    expect(readFileSync(new URL("../tools/deploy.mjs", import.meta.url), "utf8")).toMatch(/--define[^\n]*__MOLTY_VERSION__/);
  });
});
