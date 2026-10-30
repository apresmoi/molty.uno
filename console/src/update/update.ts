// "Is there newer firmware for this Molty?": the relay's public latest-release answer
// (GET <relay>/v1/releases/latest?board=, docs/AGENT-LINK.md "Firmware updates") vs the
// version the device reports. Pure functions; the status page renders the result.

/** GET /v1/releases/latest (public, no device data). */
export interface PublicRelease {
  ok: true;
  release_check: 'fresh' | 'stale' | 'unavailable';
  latest: null | { version: string; tag: string; prerelease: boolean; published_at: string | null; notes_url: string; notes: string };
  board: null | { name: string; ota: OtaAsset };
  features: Array<{ feature: string; cap: string; needs: string }>;
}

export interface OtaAsset { url: string; size: number; sha256: string | null }

/** What the device installs (POST /api/update) — taken from the relay's answer, never typed by hand. */
export interface UpdateTarget { version: string; url: string; sha256: string; size: number }

export type UpdateView =
  | { kind: 'available'; latest: string; notes: string; notesUrl: string; ota: OtaAsset | null; target: UpdateTarget | null }
  | { kind: 'current'; latest: string }
  | { kind: 'none' }
  | { kind: 'unknown'; why: 'dev' | 'unreachable' };

const SEMVER = /^v?(\d{1,9})\.(\d{1,9})\.(\d{1,9})(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/;

/** Semver precedence (<0, 0, >0); undefined when either side is not X.Y.Z[-pre]. Mirrors relay/src/core/version.ts. */
export function compareVersions(a: string | null | undefined, b: string | null | undefined): number | undefined {
  const x = a ? SEMVER.exec(a.trim()) : null;
  const y = b ? SEMVER.exec(b.trim()) : null;
  if (!x || !y) return undefined;
  for (let i = 1; i <= 3; i++) if (+x[i] !== +y[i]) return +x[i] - +y[i];
  const p = x[4] ? x[4].split('.') : [], q = y[4] ? y[4].split('.') : [];
  if (!p.length || !q.length) return q.length - p.length;
  for (let i = 0; i < Math.max(p.length, q.length); i++) {
    if (p[i] === undefined) return -1;
    if (q[i] === undefined) return 1;
    if (p[i] === q[i]) continue;
    const pn = /^\d+$/.test(p[i]), qn = /^\d+$/.test(q[i]);
    if (pn && qn) return +p[i] - +q[i];
    if (pn !== qn) return pn ? -1 : 1;
    return p[i] < q[i] ? -1 : 1;
  }
  return 0;
}

export function updateView(fw: string | null | undefined, r: PublicRelease | null): UpdateView {
  if (!r || r.release_check === 'unavailable') return { kind: 'unknown', why: 'unreachable' };
  if (!r.latest) return { kind: 'none' };
  const cmp = compareVersions(r.latest.version, fw);
  if (cmp === undefined) return { kind: 'unknown', why: 'dev' };
  if (cmp <= 0) return { kind: 'current', latest: r.latest.version };
  const ota = r.board?.ota ?? null;
  const target = ota && ota.sha256 ? { version: r.latest.version, url: ota.url, sha256: ota.sha256, size: ota.size } : null;
  return { kind: 'available', latest: r.latest.version, notes: r.latest.notes, notesUrl: r.latest.notes_url, ota, target };
}

/** Release notes as plain lines: the CHANGELOG section minus its "## vX.Y.Z" heading. */
export function notesText(notes: string): string {
  return notes.replace(/\r/g, '').split('\n').filter((l, i) => !(i === 0 && /^#+\s/.test(l))).join('\n').trim();
}

/** The relay's answer, or null when it can't be reached in time (the page then says nothing). */
export async function latestRelease(relay: string, board: string, fetchImpl: typeof fetch = fetch, timeoutMs = 6000): Promise<PublicRelease | null> {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const res = await fetchImpl(`${relay.replace(/\/+$/, '')}/v1/releases/latest?board=${encodeURIComponent(board)}`, { signal: ctl.signal });
    if (!res.ok) return null;
    const b = (await res.json()) as PublicRelease;
    return b && b.ok === true ? b : null;
  } catch {
    return null;
  } finally { clearTimeout(t); }
}
