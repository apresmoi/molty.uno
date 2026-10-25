import { describe, expect, it } from "vitest";
import { COMMANDS, MOODS } from "../src/core/commands.js";
import { TOOLS } from "../src/surfaces/mcp.js";
import { SKILL_PATHS, handleSkill, skillMarkdown } from "../src/surfaces/skill.js";
import { readFileSync } from "node:fs";

describe("GET /skill.md", () => {
  it("serves markdown at /skill.md and /skill with URLs built from the request origin", async () => {
    expect(SKILL_PATHS).toEqual(["/skill.md", "/skill"]);
    for (const path of SKILL_PATHS) {
      const res = handleSkill(new Request(`https://relay.example.dev${path}`));
      expect(res.status).toBe(200);
      expect(res.headers.get("content-type")).toMatch(/^text\/markdown/);
      const md = await res.text();
      expect(md).toContain("curl -sS -X POST https://relay.example.dev/v1/pair");
      expect(md).toContain("https://relay.example.dev/v1/devices/molty-xxxxxxxxxx/cmd");
      expect(md).toContain("https://relay.example.dev/v1/jobs/");
      expect(md).toContain("claude mcp add --transport http molty https://relay.example.dev/mcp --header");
      // No other origin leaks in (never hard-coded).
      const origins = new Set(md.match(/https?:\/\/[a-z0-9.:-]+/gi));
      expect([...origins]).toEqual(["https://relay.example.dev"]);
    }
    // worker.ts imports cloudflare:workers (not loadable here): check the route statically.
    expect(readFileSync(new URL("../src/worker.ts", import.meta.url), "utf8")).toMatch(/SKILL_PATHS\.includes\(url\.pathname\)\) return handleSkill\(request\)/);
    const local = await handleSkill(new Request("http://127.0.0.1:8787/skill.md")).text();
    expect(local).toContain("http://127.0.0.1:8787/v1/pair");
  });

  it("documents every command, mood and MCP tool the code actually has", () => {
    const md = skillMarkdown("https://r");
    for (const c of COMMANDS) expect(md).toContain(`| \`${c}\` |`);
    for (const m of MOODS) expect(md).toContain(m);
    for (const t of TOOLS) expect(md).toContain(`\`${t.name}\``);
    for (const fact of ["pair_id", "agent_token", "wait_s", "120 s", "Authorization: Bearer", "audio_busy", "secret"]) expect(md).toContain(fact);
  });

  it("rejects non-GET and supports HEAD", async () => {
    expect((await handleSkill(new Request("https://r/skill.md", { method: "POST" }))).status).toBe(405);
    const head = await handleSkill(new Request("https://r/skill.md", { method: "HEAD" }));
    expect(head.status).toBe(200);
    expect(await head.text()).toBe("");
  });
});
