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
    source: 'docs/PACKAGES.md',
    slug: 'moltys/packages',
    title: 'Packages and scenes',
    description: 'Data-only packages that teach a Molty new scenes and creatures: the format, the commands, the limits.',
  },
  {
    source: 'docs/CREATURES.md',
    slug: 'moltys/creature-packages',
    title: 'Creature packages',
    description: 'The duck, the cat and the octopus as data: channel creatures, the rigged cat with baked clips, limits.',
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

/** Addresses that used to serve other pages: they land on the Moltys group's first page. */
export const DOC_REDIRECTS = {
  '/moltys/overview/': '/moltys/packages/',
  '/moltys/making-your-own/': '/moltys/packages/',
  '/moltys/format/': '/moltys/packages/',
};
