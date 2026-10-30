// "Give Molty a brain": the ONE message the human pastes into their agent, the
// plugin install lines for Claude Code / Codex, and the advanced fallbacks (MCP URL,
// CLI pair command). Pure functions of the link context.
// Pairing protocol: docs/AGENT-LINK.md (molty_pair → tap to accept → agent_token).

export interface LinkContext {
  relay: string;     // https://… base of molty-relay (no trailing slash)
  deviceId: string;  // molty-k7m2w9c4ya (random, from GET /api/link)
  linkCode: string;  // 8 chars [A-Z2-9]
}

// The project's hosted relay, used until the device reports its own relay_url.
// Same value as the firmware's CONFIG_MOLTY_DEFAULT_RELAY and the CLI's DefaultRelay —
// see docs/AGENT-LINK.md "Default relay".
export const DEFAULT_RELAY = 'https://molty-relay.alicenet.workers.dev';

/** The agent skill: one folder in the repo, shipped as the molty plugin (the relay does not serve it). */
export const SKILL_HOME = 'https://github.com/apresmoi/molty.uno/tree/main/skills/molty';

/** Install the skill through the git plugin marketplaces (the only distribution). */
export const PLUGIN_INSTALL = {
  claude: 'claude plugin marketplace add apresmoi/molty.uno && claude plugin install molty@molty',
  codex: 'codex plugin marketplace add apresmoi/molty.uno && codex plugin add molty@molty',
} as const;

const base = (relay: string) => relay.replace(/\/+$/, '');
export const mcpUrl = (relay: string) => base(relay) + '/mcp';

/** The `molty` CLI pairing command (bridge CLI with the relay transport). */
export const pairCommand = (c: LinkContext, agentName = 'my-agent') =>
  `molty pair --relay ${c.relay} --device ${c.deviceId} --code ${c.linkCode} --name '${agentName}'`;

/**
 * The single message for any agent (Claude, Grok, OpenClaw…): the skill does the rest.
 * The relay is named only when it is not the official one (a private relay).
 */
export const agentMessage = (c: LinkContext) => {
  const relay = base(c.relay) === DEFAULT_RELAY ? '' : `\nRelay: ${base(c.relay)}`;
  return `Hi! Please connect yourself to my Molty.\n\nSkill: ${SKILL_HOME}\nDevice: ${c.deviceId}\nCode: ${c.linkCode}${relay}\n\nI'll tap Molty to accept you.`;
};
