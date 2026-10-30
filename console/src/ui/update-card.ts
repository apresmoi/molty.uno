// "update available: X.Y.Z — what's new" on the status page. The latest release comes
// from the device's relay (public GET /v1/releases/latest?board=, no device data). The
// install path depends on the firmware: one that can update itself (GET /api/update
// answers) gets an Update button (POST /api/update with the relay's official image +
// sha256); older firmware gets the manual path: download the OTA image, upload it here
// (POST /api/ota). docs/AGENT-LINK.md "Firmware updates".
import { DeviceError, OfflineError, UnsupportedError, relayHttp, type DeviceApi } from '../api/device';
import type { DeviceInfo, LinkInfo } from '../api/types';
import { latestRelease, notesText, updateView, type UpdateView } from '../update/update';
import { DEFAULT_RELAY } from './agent-link';
import { confirmButton, h, mount, note } from './dom';
import { errText } from './fmt';

const RECHECK_MS = 30 * 60 * 1000;

export interface UpdateCard { el: HTMLElement; check(info: DeviceInfo | null, link: LinkInfo | null): void; stop(): void }

export function updateCard(api: DeviceApi, fetchImpl: typeof fetch = (...a) => fetch(...a)): UpdateCard {
  const el = h('div', { class: 'update', 'aria-live': 'polite' });
  let key = '', at = 0, alive = true, poll = 0;
  let view: UpdateView | null = null;
  let selfUpdate = false;
  /** Set once an install started: the card then follows it instead of offering it again. */
  let installing: { version: string; line: string; kind: 'info' | 'warn' | 'err' | 'ok' } | null = null;
  /** Why the last attempt did not start (the offer stays). */
  let problem: string | null = null;

  async function check(info: DeviceInfo | null, link: LinkInfo | null) {
    if (!alive || !info) return;
    if (installing && info.fw && info.fw === installing.version) {
      installing = { ...installing, line: `updated to ${installing.version} ✓`, kind: 'ok' };
      view = { kind: 'current', latest: installing.version };
      return render();
    }
    const board = typeof info.board === 'string' ? info.board : '';
    const relay = relayHttp(link?.relay_url || '') || DEFAULT_RELAY;
    const k = `${relay}|${board}|${info.fw ?? ''}`;
    if (!board || (k === key && Date.now() - at < RECHECK_MS)) return;
    key = k; at = Date.now();
    const r = await latestRelease(relay, board, fetchImpl);
    if (!alive || key !== k) return;
    view = updateView(info.fw, r);
    if (view.kind === 'available') {
      try { await api.updateState(); selfUpdate = true; }
      catch (e) { selfUpdate = false; if (!(e instanceof UnsupportedError)) at = 0; }
    }
    render();
  }

  function status() {
    return installing ? note(installing.kind, installing.line) : null;
  }

  function render() {
    const v = view;
    if (!v || v.kind === 'none' || (v.kind === 'unknown' && v.why === 'unreachable')) { mount(el, status()); return; }
    if (v.kind === 'unknown') { mount(el, h('p', { class: 'dim small' }, 'development build ⠂ not compared with releases')); return; }
    if (v.kind === 'current') { mount(el, installing ? status() : h('p', { class: 'dim small' }, `firmware up to date ⠂ ${v.latest}`)); return; }
    const notes = notesText(v.notes);
    mount(el, h('div', { class: 'card' },
      h('div', { class: 'row' },
        h('b', { class: 'hi' }, `update available: ${v.latest}`),
        h('a', { href: v.notesUrl, target: '_blank', rel: 'noopener' }, 'release notes')),
      notes ? h('details', { class: 'notes' }, h('summary', null, 'what’s new'), h('pre', null, notes)) : null,
      installing ? status() : action(v),
      problem ? note('err', problem) : null));
  }

  function action(v: Extract<UpdateView, { kind: 'available' }>) {
    if (selfUpdate && v.target) {
      const t = v.target;
      return h('div', { class: 'row' },
        h('span', { class: 'dim small' }, 'Molty downloads it, checks it and restarts (about a minute).'),
        confirmButton('Update', 'Sure? Update now', () => void start(t), 'primary'));
    }
    if (!v.ota) return h('p', { class: 'dim small' }, 'this release has no image for this board yet.');
    const name = v.ota.url.slice(v.ota.url.lastIndexOf('/') + 1);
    const file = h('input', { type: 'file', accept: '.bin', 'aria-label': 'firmware image' });
    const install = confirmButton('Install', 'Sure? Molty restarts', () => void upload(file, v.latest), 'primary');
    return h('div', { class: 'manual' },
      h('p', { class: 'small' }, '1. download ', h('a', { href: v.ota.url }, name)),
      h('p', { class: 'small' }, '2. install it on Molty (settings and pairings stay):'),
      h('div', { class: 'row' }, file, install));
  }

  async function start(t: { version: string; url: string; sha256: string; size: number }) {
    problem = null;
    installing = { version: t.version, line: 'starting the update…', kind: 'info' };
    render();
    try { await api.startUpdate(t); }
    catch (e) { installing = null; problem = errText(e); render(); return; }
    follow();
  }

  /** Polls GET /api/update until the device restarts; the next /api/info with the new fw says "updated". */
  function follow() {
    clearTimeout(poll);
    poll = window.setTimeout(async () => {
      if (!alive || !installing || installing.kind === 'ok') return;
      try {
        const s = await api.updateState();
        if (s.state === 'failed') { installing = { ...installing, line: `update failed: ${s.error ?? 'unknown'} — Molty kept its firmware`, kind: 'err' }; return render(); }
        const pct = typeof s.progress === 'number' ? ` ${Math.round(s.progress * 100)}%` : '';
        installing = { ...installing, line: s.state === 'downloading' ? `downloading${pct}…` : s.state === 'flashing' ? 'installing…' : s.state === 'rebooting' ? 'restarting…' : installing.line };
      } catch (e) {
        if (e instanceof OfflineError) installing = { ...installing, line: 'Molty is restarting…' };
      }
      render();
      follow();
    }, 1500);
  }

  async function upload(input: HTMLInputElement, version: string) {
    const f = input.files?.[0];
    if (!f) { problem = 'pick the .bin you downloaded first'; render(); return; }
    problem = null;
    installing = { version, line: `installing ${f.name}… (keep this page open)`, kind: 'info' };
    render();
    try {
      await api.ota(new Uint8Array(await f.arrayBuffer()));
      installing = { ...installing, line: 'installed — Molty is restarting…' };
    } catch (e) {
      installing = null;
      problem = `not installed: ${e instanceof DeviceError && e.code === 'wrong_board' ? 'that image is for another board' : errText(e)}`;
    }
    render();
  }

  return { el, check: (i, l) => void check(i, l), stop() { alive = false; clearTimeout(poll); } };
}
