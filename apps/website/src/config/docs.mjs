// Docs that are SYNCED from the repo (the repo file stays the source of truth).
// scripts/sync-docs.mjs reads each `source`, drops its H1, rewrites repo-relative links
// and writes src/content/docs/repo/<slug>.md with `slug` frontmatter, so the page is
// served at /<slug>/. The generated folder is gitignored; never edit it by hand.
//
// `optional: true` entries are synced (and listed in the sidebar) only when the source file
// exists, so a guide that lands on another branch appears without touching this list.
export const SYNCED_DOCS = [
  {
    source: 'docs/HARDWARE.md',
    slug: 'getting-started/hardware',
    title: 'Build and flash',
    description: 'The boards, the toolchain, building the firmware and the first flash.',
  },
  {
    source: 'docs/PORTING.md',
    slug: 'getting-started/porting',
    title: 'Porting to your board',
    description: 'Bake molty.UNO for your own screen: one board folder, optional parts, build, flash, submit.',
  },
  {
    source: 'docs/AGENT-LINK.md',
    slug: 'agents/protocol',
    title: 'Agent link protocol',
    description: 'Device, relay and agent: pairing, the agent skill, REST, remote MCP and the device wire.',
  },
  {
    source: 'docs/CLI.md',
    slug: 'agents/cli',
    title: 'CLI and local MCP',
    description: 'The molty CLI and stdio MCP server, direct over USB/Wi-Fi or through the relay.',
  },
  {
    source: 'creatures/README.md',
    slug: 'moltys/overview',
    title: 'Moltys and the viewers',
    description: 'Where the creatures live, the browser viewers and the .molty exporter.',
  },
  {
    source: 'creatures/js/cute/README.md',
    slug: 'moltys/making-your-own',
    title: 'Make your own Molty',
    description: 'The cute kit: a creature is a small data spec plus optional hooks.',
  },
  {
    source: 'creatures/MOLTY-FORMAT.md',
    slug: 'moltys/format',
    title: 'The .molty format',
    description: 'The ~1 KB binary file that carries one creature to the device.',
  },
  {
    source: 'docs/ARCHITECTURE.md',
    slug: 'architecture',
    title: 'Architecture',
    description: 'How the device, console, relay, CLI and creature engine fit together.',
  },
  {
    source: 'CONTRIBUTING.md',
    slug: 'contributing',
    title: 'Contributing',
    description: 'Tests to run, conventions, and how to send a pull request.',
  },
];
