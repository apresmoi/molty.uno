// "Give Molty a brain": the ONE message the human pastes into their agent, plus the
// advanced fallbacks (MCP URL, CLI pair command). Pure functions of the link context.
// Pairing protocol: docs/AGENT-LINK.md (molty_pair → tap to accept → agent_token).

export interface LinkContext {
  relay: string;     // https://… base of molty-relay (no trailing slash)
  deviceId: string;  // molty-k7m2w9c4ya (random, from GET /api/link)
  linkCode: string;  // 8 chars [A-Z2-9]
}

const base = (relay: string) => relay.replace(/\/+$/, '');
export const mcpUrl = (relay: string) => base(relay) + '/mcp';
export const skillUrl = (relay: string) => base(relay) + '/skill.md';

/** The `molty` CLI pairing command (bridge CLI with the relay transport). */
export const pairCommand = (c: LinkContext, agentName = 'my-agent') =>
  `molty pair --relay ${c.relay} --device ${c.deviceId} --code ${c.linkCode} --name '${agentName}'`;

/** The single message for any agent (Claude, Grok, OpenClaw…): the skill URL does the rest. */
export const agentMessage = (c: LinkContext) =>
  `Hi! Please connect yourself to my Molty.\n\nInstructions: ${skillUrl(c.relay)}\nDevice: ${c.deviceId}\nCode: ${c.linkCode}\n\nI'll tap Molty to accept you.`;
