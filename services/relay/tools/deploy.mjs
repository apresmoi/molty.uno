#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// `pnpm run deploy [extra wrangler args]`: wrangler deploy with the product
// version (repo-root VERSION) baked in as __MOLTY_VERSION__ (src/core/version.ts),
// so GET /v1/health and the MCP serverInfo report it.
// A real deploy needs CLOUDFLARE_ACCOUNT_ID set explicitly: wrangler's own login
// may belong to a different account than the one you mean, and it would deploy there.
// `pnpm run deploy --dry-run --outdir dist` builds without deploying (no account needed).
// pnpm forwards a literal `--`; it is dropped here so `--dry-run` reaches wrangler as a flag.
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';

const version = readFileSync(new URL('../../../VERSION', import.meta.url), 'utf8').trim();
const extra = process.argv.slice(2).filter((a) => a !== '--');
const dryRun = extra.includes('--dry-run');
if (!dryRun && !process.env.CLOUDFLARE_ACCOUNT_ID) {
  console.error('molty-relay deploy: set CLOUDFLARE_ACCOUNT_ID to the account you mean (refusing to deploy to whatever account wrangler is logged into).');
  process.exit(2);
}
const args = ['deploy', '--define', `__MOLTY_VERSION__:${JSON.stringify(version)}`, ...extra];
console.log(`molty-relay ${version}: wrangler ${args.join(' ')}`);
const r = spawnSync('wrangler', args, { stdio: 'inherit', shell: process.platform === 'win32' });
process.exit(r.status ?? 1);
