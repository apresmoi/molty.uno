#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// `pnpm run deploy [-- extra wrangler args]`: wrangler deploy with the product
// version (repo-root VERSION) baked in as __MOLTY_VERSION__ (src/core/version.ts),
// so GET /v1/health and the MCP serverInfo report it. Credentials come from
// wrangler's own login or CLOUDFLARE_API_TOKEN + CLOUDFLARE_ACCOUNT_ID.
// `pnpm run deploy -- --dry-run --outdir dist` builds without deploying.
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';

const version = readFileSync(new URL('../../VERSION', import.meta.url), 'utf8').trim();
const args = ['deploy', '--define', `__MOLTY_VERSION__:${JSON.stringify(version)}`, ...process.argv.slice(2)];
console.log(`molty-relay ${version}: wrangler ${args.join(' ')}`);
const r = spawnSync('wrangler', args, { stdio: 'inherit', shell: process.platform === 'win32' });
process.exit(r.status ?? 1);
