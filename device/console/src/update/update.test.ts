import { describe, expect, it } from 'vitest';
import { compareVersions, installOutcome, latestRelease, notesText, updateView, type PublicRelease } from './update';

const OTA = { url: 'https://github.com/apresmoi/molty.uno/releases/download/v0.2.0/molty-0.2.0-b-ota.bin', size: 1234, sha256: 'd'.repeat(64) };
const rel = (o: Partial<PublicRelease> = {}): PublicRelease => ({
  ok: true, release_check: 'fresh',
  latest: { version: '0.2.0', tag: 'v0.2.0', prerelease: false, published_at: null, notes_url: 'https://github.com/x/y/releases/tag/v0.2.0', notes: '## v0.2.0 — 2026-11-01\n- ask on the device' },
  board: { name: 'b', ota: OTA }, features: [], ...o,
});

describe('compareVersions (mirrors the relay)', () => {
  it('orders releases and prereleases; anything else is not comparable', () => {
    expect(compareVersions('0.1.0', '0.2.0')).toBeLessThan(0);
    expect(compareVersions('0.10.0', '0.9.9')).toBeGreaterThan(0);
    expect(compareVersions('0.2.0-rc.1', '0.2.0')).toBeLessThan(0);
    expect(compareVersions('0.2.0-rc.2', '0.2.0-rc.10')).toBeLessThan(0);
    expect(compareVersions('v0.2.0', '0.2.0')).toBe(0);
    expect(compareVersions('dev', '0.2.0')).toBeUndefined();
    expect(compareVersions(undefined, '0.2.0')).toBeUndefined();
  });
});

describe('updateView', () => {
  it('available: the version, notes and the install target taken from the relay answer', () => {
    expect(updateView('0.1.0', rel())).toEqual({
      kind: 'available', latest: '0.2.0', notes: rel().latest!.notes, notesUrl: 'https://github.com/x/y/releases/tag/v0.2.0',
      ota: OTA, target: { version: '0.2.0', url: OTA.url, sha256: OTA.sha256, size: 1234 },
    });
  });
  it('no checksum: no one-click target (manual only); no image for this board: no ota', () => {
    expect(updateView('0.1.0', rel({ board: { name: 'b', ota: { ...OTA, sha256: null } } }))).toMatchObject({ kind: 'available', target: null, ota: { sha256: null } });
    expect(updateView('0.1.0', rel({ board: null }))).toMatchObject({ kind: 'available', ota: null, target: null });
  });
  it('current, no release, dev build, relay unreachable', () => {
    expect(updateView('0.2.0', rel())).toEqual({ kind: 'current', latest: '0.2.0' });
    expect(updateView('0.3.0', rel())).toEqual({ kind: 'current', latest: '0.2.0' });
    expect(updateView('0.1.0', rel({ latest: null, board: null }))).toEqual({ kind: 'none' });
    expect(updateView('dev', rel())).toEqual({ kind: 'unknown', why: 'dev' });
    expect(updateView('0.1.0', null)).toEqual({ kind: 'unknown', why: 'unreachable' });
    expect(updateView('0.1.0', rel({ release_check: 'unavailable', latest: null }))).toEqual({ kind: 'unknown', why: 'unreachable' });
  });
  it('notes drop their own heading', () => {
    expect(notesText(rel().latest!.notes)).toBe('- ask on the device');
    expect(notesText('- just a line')).toBe('- just a line');
  });
});

describe('latestRelease', () => {
  it('asks the relay for this board and returns its answer; failures are null', async () => {
    const seen: string[] = [];
    const ok = (async (u: string) => { seen.push(u); return new Response(JSON.stringify(rel())); }) as unknown as typeof fetch;
    expect(await latestRelease('https://relay.example/', 'waveshare-esp32-s3-touch-lcd-1.46', ok)).toEqual(rel());
    expect(seen).toEqual(['https://relay.example/v1/releases/latest?board=waveshare-esp32-s3-touch-lcd-1.46']);
    const down = (async () => { throw new TypeError('network'); }) as unknown as typeof fetch;
    expect(await latestRelease('https://r', 'b', down)).toBeNull();
    const limited = (async () => new Response('{"ok":false}', { status: 429 })) as unknown as typeof fetch;
    expect(await latestRelease('https://r', 'b', limited)).toBeNull();
    const hang = ((_: string, i: RequestInit) => new Promise((_r, rej) => i.signal!.addEventListener('abort', () => rej(new Error('abort'))))) as unknown as typeof fetch;
    expect(await latestRelease('https://r', 'b', hang, 20)).toBeNull();
  });
});

describe('installOutcome', () => {
  const before = { fw: '0.1.0', boot_count: 74, uptime_ms: 3_725_000 };
  it('updated on the target version', () => {
    expect(installOutcome('0.2.0', before, { fw: '0.2.0', boot_count: 75, uptime_ms: 2000 })).toBe('updated');
  });
  it('restarted on another version: rolled back (new boot count, or the uptime went backwards)', () => {
    expect(installOutcome('0.2.0', before, { fw: '0.1.0', boot_count: 75, uptime_ms: 9_000_000 })).toBe('rolled_back');
    expect(installOutcome('0.2.0', { fw: '0.1.0', uptime_ms: 3_725_000 }, { fw: '0.1.0', uptime_ms: 2000 })).toBe('rolled_back');
  });
  it('same boot, old version: still pending (downloading, installing, or not restarted yet)', () => {
    expect(installOutcome('0.2.0', before, { fw: '0.1.0', boot_count: 74, uptime_ms: 3_730_000 })).toBe('pending');
    expect(installOutcome('0.2.0', null, { fw: '0.1.0', boot_count: 75, uptime_ms: 1 })).toBe('pending');
  });
});
