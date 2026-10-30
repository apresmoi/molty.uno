import { DeviceError, OfflineError, UnsupportedError } from '../api/device';

export function uptime(ms: number) {
  const s = Math.floor(ms / 1000), d = Math.floor(s / 86400), hh = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return d ? `${d}d ${hh}h` : hh ? `${hh}h ${m}m` : `${m}m ${s % 60}s`;
}

/** RSSI as four braille bars. */
export function bars(rssi?: number) {
  if (rssi === undefined) return '';
  const n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
  return ['⣀', '⣤', '⣶', '⣿'].map((c, i) => (i < n ? c : '⠂')).join('');
}

export function errText(e: unknown): string {
  if (e instanceof UnsupportedError) return 'this firmware doesn\'t support that yet';
  if (e instanceof OfflineError) return 'can\'t reach molty — is it on and on this network?';
  if (e instanceof DeviceError) return `molty said: ${e.code}`;
  return (e as Error)?.message ?? String(e);
}

export function debounce<A extends unknown[]>(fn: (...a: A) => void, ms: number) {
  let t = 0;
  const d = (...a: A) => { clearTimeout(t); t = window.setTimeout(() => fn(...a), ms); };
  return d;
}
