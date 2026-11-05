// The skill's newest version (core/skill-version.ts, skills/molty/SKILL.md "What's new"): read from
// CHANGES.md on GitHub behind the agent's back, never in the way of an utterance or status.
import { describe, expect, it } from "vitest";
import { CATALOG_LIMITS } from "../src/core/catalog.js";
import { SkillVersion } from "../src/core/skill-version.js";
import { realClock } from "../src/core/store.js";
import { DEVICE, LINK, connect, makeRoom } from "./helpers.js";

const CHANGES = "# Molty skill: what's new\n\n## 2026.11.05\n\n- keep: take over Molty's opener\n\n## 2026.10.30\n\n- older\n";
const tick = () => new Promise((r) => setTimeout(r, 0));

function github(answer: () => Response = () => new Response(CHANGES)) {
  const urls: string[] = [];
  return { urls, fetch: async (req: Request) => (urls.push(req.url), answer()) };
}

describe("skill version", () => {
  it("the first heading of CHANGES.md at the packages ref; read in the background, once while fresh", async () => {
    let t = 1_000_000;
    const gh = github();
    const v = new SkillVersion(gh.fetch, { ...realClock, now: () => t }, "main");
    expect(v.peek()).toBeUndefined(); // never waits on GitHub
    await tick();
    expect(v.peek()).toBe("2026.11.05");
    expect(gh.urls).toEqual(["https://raw.githubusercontent.com/apresmoi/molty.uno/main/skills/molty/CHANGES.md"]);
    t += CATALOG_LIMITS.freshMs;
    expect(v.peek()).toBe("2026.11.05"); // stale: served while it refreshes
    expect(gh.urls).toHaveLength(2);
  });

  it("GitHub down or the file without a version: nothing, retried after a pause; a later failure keeps the last", async () => {
    let t = 1_000_000, answer = () => new Response("404: Not Found", { status: 404 });
    const v = new SkillVersion(async () => answer(), { ...realClock, now: () => t }, "main");
    v.peek();
    await tick();
    expect(v.peek()).toBeUndefined();
    t += CATALOG_LIMITS.failRetryMs;
    answer = () => new Response("# no version here");
    v.peek();
    await tick();
    expect(v.peek()).toBeUndefined();
    t += CATALOG_LIMITS.failRetryMs;
    answer = () => new Response(CHANGES);
    v.peek();
    await tick();
    t += CATALOG_LIMITS.freshMs;
    answer = () => { throw new TypeError("network"); };
    v.peek();
    await tick();
    expect(v.peek()).toBe("2026.11.05");
  });

  it("in status as relay.skill once read", async () => {
    const ctx = makeRoom({ skillChanges: CHANGES, timings: { cmdTimeoutMs: 300, pairWindowMs: 400 } });
    const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: ["say"] });
    const orig = sock.sendText.bind(sock);
    sock.sendText = (t: string) => {
      orig(t);
      const f = JSON.parse(t);
      if (f.t === "pair.request") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
      if (f.t === "cmd") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: {} })), 1);
    };
    const out = (await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "a" })) as any;
    const status = async () => ((await ctx.room.command(out.result.agent_token, "status", {})) as any).result.relay.skill;
    expect(await status()).toBeNull();
    await tick();
    expect(await status()).toBe("2026.11.05");
  });
});
