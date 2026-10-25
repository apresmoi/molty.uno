// DeviceApi — the ONLY module that talks to the device. Every page goes through it.
//
// Base URL: same origin (the device serves this console at "/"); dev override with
// ?device=http://<device-ip> (remembered per browser until ?device= is cleared).
//
// Two device facts shape this module:
//  1. The device's esp_http_server answers one request at a time, and a framebuffer
//     snapshot (~509 KB) holds it for ~0.6 s. So requests are serialized here, user
//     actions jump ahead of queued preview frames, and preview frames are dropped
//     (not queued) while anything else is waiting.
//  2. The device sends `Access-Control-Allow-Origin: *` but never answers OPTIONS
//     preflights, so cross-origin (dev) POSTs must be "simple": text/plain bodies.
//     The firmware parses the body regardless of Content-Type.
//
// Unknown routes come back as HTTP 200 {"ok":false,"error":"not_found"} on this
// firmware (its 404 handler), which is indistinguishable from a handler's own
// not_found (e.g. POST /api/creature with an unknown name). So only calls marked
// `probe` (endpoints newer firmware may lack) map that body to UnsupportedError;
// a real HTTP 404/405/501 is always UnsupportedError.

import type {
  CreatureRegistry, DeviceInfo, FluidOrder, FluidState, Frame, LinkInfo,
  WifiConnectResult, WifiNetwork, WifiStatus, Diag,
} from './types';
import { parsePPM } from './ppm';

export class UnsupportedError extends Error {
  constructor(public path: string) { super(`not supported by this firmware: ${path}`); }
}
export class DeviceError extends Error {
  constructor(public code: string, public body?: unknown) { super(code); }
}
export class OfflineError extends Error {}

type Job = { run: () => Promise<void>; drop?: () => void; low: boolean };

function resolveBase(): string {
  const q = new URLSearchParams(location.search).get('device');
  try {
    if (q !== null) {
      if (q) localStorage.setItem('molty.device', q); else localStorage.removeItem('molty.device');
    }
  } catch { /* storage blocked: fine, the query param still works for this load */ }
  let v = q;
  if (v === null) { try { v = localStorage.getItem('molty.device'); } catch { v = null; } }
  if (!v) return '';
  if (!/^https?:\/\//.test(v)) v = 'http://' + v;
  return v.replace(/\/+$/, '');
}

export class DeviceApi {
  readonly base: string;
  private queue: Job[] = [];
  private busy = false;

  constructor(base = resolveBase()) { this.base = base; }

  get label(): string { return this.base ? this.base.replace(/^https?:\/\//, '') : location.host || 'this device'; }

  // ── transport ───────────────────────────────────────────────────────────────
  private enqueue<T>(fn: () => Promise<T>, low = false): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      const job: Job = {
        low,
        run: () => fn().then(resolve, reject),
        drop: () => reject(new DOMException('dropped', 'AbortError')),
      };
      if (low) this.queue.push(job);
      else {
        const i = this.queue.findIndex((j) => j.low);
        if (i < 0) this.queue.push(job); else this.queue.splice(i, 0, job);
      }
      this.pump();
    });
  }

  private async pump() {
    if (this.busy) return;
    const job = this.queue.shift();
    if (!job) return;
    this.busy = true;
    try { await job.run(); } finally { this.busy = false; this.pump(); }
  }

  private async raw(path: string, init: RequestInit = {}, timeoutMs = 8000): Promise<Response> {
    const ctl = new AbortController();
    const t = setTimeout(() => ctl.abort(), timeoutMs);
    try {
      const res = await fetch(this.base + path, { cache: 'no-store', ...init, signal: ctl.signal });
      if (res.status === 404 || res.status === 405 || res.status === 501) throw new UnsupportedError(path);
      return res;
    } catch (e) {
      if (e instanceof UnsupportedError) throw e;
      throw new OfflineError(ctl.signal.aborted ? `timeout: ${path}` : `unreachable: ${path}`);
    } finally { clearTimeout(t); }
  }

  private async json<T>(path: string, init?: RequestInit, timeoutMs?: number, probe = false): Promise<T> {
    return this.enqueue(async () => {
      const res = await this.raw(path, init, timeoutMs);
      let body: any;
      try { body = await res.json(); } catch { throw new DeviceError('bad_json'); }
      if (body && body.ok === false) {
        if (probe && body.error === 'not_found') throw new UnsupportedError(path);
        throw new DeviceError(String(body.error ?? 'error'), body);
      }
      if (!res.ok) throw new DeviceError(`http_${res.status}`, body);
      return body as T;
    });
  }

  private get<T>(path: string, timeoutMs?: number, probe = false) { return this.json<T>(path, { method: 'GET' }, timeoutMs, probe); }
  private post<T>(path: string, body: unknown, timeoutMs?: number, probe = false) {
    return this.json<T>(path, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain;charset=UTF-8' },
      body: JSON.stringify(body),
    }, timeoutMs, probe);
  }

  // ── device ──────────────────────────────────────────────────────────────────
  info() { return this.get<DeviceInfo>('/api/info'); }

  /** Live screen snapshot (P6 PPM, 412×412). Low priority: dropped while user actions wait. */
  framebuffer(): Promise<Frame> {
    return this.enqueue(async () => {
      if (this.queue.some((j) => !j.low)) throw new DOMException('dropped', 'AbortError');
      const res = await this.raw('/api/framebuffer', {}, 6000);
      const ct = res.headers.get('content-type') || '';
      if (ct.includes('json')) {
        const b = await res.json().catch(() => ({}));
        throw new DeviceError(String(b.error ?? 'no_framebuffer'));
      }
      return parsePPM(new Uint8Array(await res.arrayBuffer()));
    }, true);
  }

  // ── Wi-Fi (same endpoints + bodies as the old console's deviceApi.ts) ───────
  wifiStatus() { return this.get<WifiStatus>('/api/status'); }
  /** Blocking scan on the device (~2–4 s). */
  async wifiScan(): Promise<WifiNetwork[]> {
    const r = await this.get<{ networks: WifiNetwork[] }>('/api/scan', 15000);
    return r.networks ?? [];
  }
  /** Device waits up to 15 s for the association, saves creds to NVS on success. */
  wifiConnect(ssid: string, password: string) {
    return this.post<WifiConnectResult>('/api/set_creds', { ssid, password }, 25000);
  }
  /** Wipes saved creds; the device REBOOTS ~0.5 s later. UI must confirm first. */
  wifiForget() { return this.post<{ ok: true }>('/api/forget', {}); }

  // ── creatures ──────────────────────────────────────────────────────────────
  creatures() { return this.get<CreatureRegistry>('/api/creature'); }
  selectCreature(name: string) { return this.post<CreatureRegistry>('/api/creature', { name }); }
  creatureState() { return this.get<FluidState>('/api/creature/state'); }
  fluid(order: FluidOrder) { return this.post<FluidState>('/api/creature/state', order); }

  /** Upload a .molty creature file (POST /api/creature/file, raw body). */
  uploadCreature(id: string, bytes: Uint8Array) {
    return this.json<{ ok: true; [k: string]: unknown }>(
      '/api/creature/file?name=' + encodeURIComponent(id),
      { method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: new Blob([bytes as BlobPart]) },
      20000,
      true,
    );
  }

  // ── diagnostics (firmware mole_diag: /api/diag, /api/log) ──────────────────
  diag(timeline = 0) { return this.get<Diag>(`/api/diag${timeline ? `?timeline=${timeline}` : ''}`, undefined, true); }
  /** Log ring lines with seq >= since (text); next = the seq to ask for next time. */
  log(since: number, max = 16384): Promise<{ text: string; next: number }> {
    return this.enqueue(async () => {
      const res = await this.raw(`/api/log?since=${since}&max=${max}`);
      if ((res.headers.get('content-type') || '').includes('json')) throw new UnsupportedError('/api/log');
      const text = await res.text();
      const hdr = Number(res.headers.get('X-Next-Seq'));
      return { text, next: Number.isFinite(hdr) && hdr > 0 ? hdr : since };
    });
  }

  // ── agent link (docs/AGENT-LINK.md) ────────────────────────────────────────
  link() { return this.get<LinkInfo>('/api/link', undefined, true); }
  /** relay_url must be a ws:// or wss:// origin (no path); the device reconnects. */
  setRelay(relay_url: string) { return this.post<LinkInfo>('/api/link', { relay_url }, undefined, true); }
}

/** Device ids are random (molty-[a-z2-9]{10}) and only GET /api/link knows them. */

/** ws(s)://host ⇄ http(s)://host: the device dials the ws form, agents call the http form. */
export const relayHttp = (url: string) => url.trim().replace(/\/+$/, '').replace(/^ws(s?):\/\//, 'http$1://');
export const relayWs = (url: string) => url.trim().replace(/\/+$/, '').replace(/^http(s?):\/\//, 'ws$1://');
