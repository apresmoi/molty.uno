import { describe, expect, it } from "vitest";
import { COMMANDS, MAX_ASK_LABEL, MOODS } from "../src/core/commands.js";
import { FEATURES } from "../src/core/version.js";
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
    // events: every receive mode, the signature contract, the honest limit, hold_s
    for (const fact of ["https://r/v1/devices/<id>/events?wait_s=0..25", "https://r/v1/webhook", "Molty-Signature: t=<unix ms>,v2=<hex hmac_sha256(secret, t \\n METHOD \\n path?query \\n body)>", "molty events", "--exec", "{{event.data.<field>}}", "nothing can wake an agent", "hold_s"]) expect(md).toContain(fact);
    for (const t of ["shake", "tap", "button", "answer"]) expect(md).toContain(`| \`${t}\` |`);
    // ask: the endpoint, the label limit stated from the constant, layouts, null answers, ask vs talk
    for (const fact of ["https://r/v1/devices/molty-xxxxxxxxxx/ask", `1–${MAX_ASK_LABEL} characters each`, "one centred button", "pie slices", "`timeout`", "`cancelled`", "`offline`", "use **talk** when you"]) expect(md).toContain(fact);

    // colours: the option object, every palette name, the defaults, meaningful use; ring_color on talk/listen
    for (const fact of ['{"label": "yes", "color": "green"}', "green, red, amber, blue, violet, pink, cyan, white", '`"#rrggbb"`', "green, red, amber, blue, violet by position", "green yes, red no", '"ring_color"?: c', '"ring_color"?}']) expect(md).toContain(fact);
    // updates: the trigger, the one call, ask-before-update, the error and the passive notice
    for (const fact of ["When your human asks to check Molty for updates (any wording", "https://r/v1/devices/<id>/updates", "https://r/v1/devices/<id>/update", "**ask before updating**", "`firmware_too_old`", `needs ${FEATURES.ask.min_fw}`, "`firmware_update`", "`can_self_update`"]) expect(md).toContain(fact);
  });

  it("rejects non-GET and supports HEAD", async () => {
    expect((await handleSkill(new Request("https://r/skill.md", { method: "POST" }))).status).toBe(405);
    const head = await handleSkill(new Request("https://r/skill.md", { method: "HEAD" }));
    expect(head.status).toBe(200);
    expect(await head.text()).toBe("");
  });
});

describe("skill v9 rules", () => {
  const md = skillMarkdown("https://relay.example.dev");
  it("capabilities come from relay.caps, not versions", () => {
    expect(md).toMatch(/read `relay\.caps`, not version numbers/);
    expect(md).toMatch(/`ask` in caps = `molty_ask` works/);
  });
  it("buttons mean one thing everywhere: acknowledge / dismiss, never an answer", () => {
    expect(md).not.toMatch(/accept\/reject = yes\/no|accept = yes/i);
    expect(md.match(/acknowledge \/ dismiss/g)!.length).toBeGreaterThanOrEqual(2);
    expect(md).toMatch(/Not an answer to a question/);
    const ev = TOOLS.find((t) => t.name === "molty_events")!;
    expect(ev.description).toMatch(/never an answer/);
  });
  it("a broken listen is an error; after a wake, no_speech gets a reaction", () => {
    expect(md).toMatch(/A listen that broke is an error, never `no_speech`/);
    expect(md).toMatch(/`listen_failed`/);
    expect(md).toMatch(/After a wake, never answer `no_speech` with silence/);
    expect(md).toContain(`{"say":"I'm here.","listen":false}`);
  });
});
