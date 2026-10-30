import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { SKILL_PATHS, SKILL_URL, handleSkill } from "../src/surfaces/skill.js";

// The relay no longer serves the skill (skills/molty/ in the repo, the molty plugin).
// Agents set up before that still fetch <relay>/skill.md: a permanent redirect keeps them working.
describe("GET /skill.md (deprecated)", () => {
  it("301s /skill.md and /skill to the raw SKILL.md on GitHub main", async () => {
    expect(SKILL_PATHS).toEqual(["/skill.md", "/skill"]);
    expect(SKILL_URL).toBe("https://raw.githubusercontent.com/apresmoi/molty.uno/main/skills/molty/SKILL.md");
    for (const path of SKILL_PATHS) {
      for (const method of ["GET", "HEAD"]) {
        const res = handleSkill(new Request(`https://relay.example.dev${path}`, { method }));
        expect(res.status).toBe(301);
        expect(res.headers.get("location")).toBe(SKILL_URL);
        expect(await res.text()).toBe("");
      }
    }
    // The skill file the redirect names exists in this repo.
    expect(readFileSync(new URL("../../../skills/molty/SKILL.md", import.meta.url), "utf8")).toMatch(/^---\nname: molty\n/);
    // worker.ts imports cloudflare:workers (not loadable here): check the route statically.
    expect(readFileSync(new URL("../src/worker.ts", import.meta.url), "utf8")).toMatch(/SKILL_PATHS\.includes\(url\.pathname\)\) return handleSkill\(request\)/);
  });

  it("rejects other methods", () => {
    expect(handleSkill(new Request("https://r/skill.md", { method: "POST" })).status).toBe(405);
  });
});
