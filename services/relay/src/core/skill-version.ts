// SPDX-License-Identifier: MIT
// The skill's newest version: the first "## <version>" heading of skills/molty/CHANGES.md at the
// packages ref, read from GitHub in the background and cached like the catalog. Agents compare it
// with the version their own copy states and re-read what changed (skills/molty/SKILL.md).

import { CATALOG_LIMITS, PACKAGES_RAW } from "./catalog.js";
import type { Clock } from "./store.js";

const VERSION_RE = /^## (\d{4}\.\d{2}\.\d{2})\s*$/m;

export class SkillVersion {
  private value: string | undefined;
  private at = -Infinity;
  private reading = false;

  constructor(
    private readonly fetcher: (req: Request) => Promise<Response>,
    private readonly clock: Clock,
    private readonly ref: string,
  ) {}

  get url(): string {
    return `${PACKAGES_RAW}${this.ref}/skills/molty/CHANGES.md`;
  }

  /** The last version read (undefined until one is); never waits on GitHub, refreshes behind. */
  peek(): string | undefined {
    const now = this.clock.now();
    if (!this.reading && now - this.at >= (this.value ? CATALOG_LIMITS.freshMs : CATALOG_LIMITS.failRetryMs)) {
      this.reading = true;
      this.at = now;
      void this.read().finally(() => (this.reading = false));
    }
    return this.value;
  }

  private async read(): Promise<void> {
    try {
      const res = await this.fetcher(new Request(this.url, { redirect: "manual", signal: AbortSignal.timeout(CATALOG_LIMITS.timeoutMs) }));
      const m = res.ok ? VERSION_RE.exec(await res.text()) : null;
      if (m) this.value = m[1];
    } catch {
      // unreachable: keep the last version, try again after failRetryMs
    }
  }
}
