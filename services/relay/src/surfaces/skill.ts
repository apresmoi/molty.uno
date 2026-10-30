// GET /skill.md (and /skill): DEPRECATED. The relay no longer serves the agent
// skill; it lives in the repo (skills/molty/) and ships as the molty plugin for
// Claude Code and Codex. Agents set up before that (they were told to read
// <relay>/skill.md) get a permanent redirect to the skill's SKILL.md on GitHub,
// whose relative links lead to its references. Nothing else about the skill is
// the relay's business (docs/AGENT-LINK.md "Agent skill").

export const SKILL_PATHS = ["/skill.md", "/skill"];

/** The skill's entry point on the default branch, raw (agents read markdown, not HTML). */
export const SKILL_URL = "https://raw.githubusercontent.com/apresmoi/molty.uno/main/skills/molty/SKILL.md";

export function handleSkill(request: Request): Response {
  if (request.method !== "GET" && request.method !== "HEAD") return new Response("method not allowed", { status: 405, headers: { allow: "GET, HEAD" } });
  return new Response(null, { status: 301, headers: { location: SKILL_URL, "cache-control": "public, max-age=86400" } });
}
