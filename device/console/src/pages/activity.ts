// Activity: one timeline of everything that happened on this Molty, with which agent did it
// (activity/activity.ts): turns grouped by job — the agent's call, the job's stage timings,
// voice provider, heard/said and the device's own log lines for it — plus device events,
// pairings and the voice box. Read from the relay's owner API with the link code from
// GET /api/link (like the Voice card); polls every 3 s while the tab is visible.
import { UnsupportedError } from '../api/device';
import type { AppCtx } from '../app';
import {
  ACTIVITY_KINDS, ActivityApiError, agentsOf, entryLine, filterGroups, fmtMs, getActivity, groupEntries, mergeEntries, setTranscripts, timingRows, transcript, voiceLine,
  type ActivityEntry, type ActivityTarget, type Filters, type Group,
} from '../activity/activity';
import { linkContext } from '../ui/agent-link';
import { h, mount, note } from '../ui/dom';
import { errText } from '../ui/fmt';

const POLL_MS = 3000;
const LINK_MS = 60000;   // the link code rotates every 15 min

const clock = (ts: number) => new Date(ts).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });

export function activityPage(root: HTMLElement, { api }: AppCtx) {
  let alive = true, timer = 0, linkTimer = 0, busy = false;
  let target: ActivityTarget | null = null;
  let entries: ActivityEntry[] = [];
  let next = '';
  let loaded = false, unsupported = false, transcripts = true;
  let problem: string | null = null;
  const filters: Filters = {};
  const open = new Set<string>();
  let lastSig = '';

  const controls = h('div', { class: 'act-controls' });
  const settings = h('div', { class: 'act-settings' });
  const msg = h('div', { 'aria-live': 'polite' });
  const list = h('div', { class: 'act-list' });
  mount(root,
    h('h1', null, 'activity ', h('span', { class: 'dim' }, '⠶ what happened, and who did it')),
    h('p', { class: 'lede' }, 'Every agent call and turn on this Molty, the device’s own log lines for it, and what Molty itself did — the last 24 h, kept by the relay.'),
    controls, msg, list, settings);

  async function loadLink() {
    clearTimeout(linkTimer);
    try {
      const l = await api.link();
      if (l.device_id && l.link_code) {
        const c = linkContext(l);
        const changed = !target || target.relay !== c.relay || target.deviceId !== c.deviceId;
        target = { relay: c.relay, deviceId: c.deviceId, linkCode: c.linkCode };
        if (changed) { entries = []; next = ''; loaded = false; unsupported = false; void poll(); }
      }
    } catch (e) {
      problem = e instanceof UnsupportedError ? 'this firmware doesn’t support the agent link yet — update molty, then come back.' : errText(e);
      render();
    }
    if (alive) linkTimer = window.setTimeout(loadLink, LINK_MS);
  }

  async function poll() {
    clearTimeout(timer);
    if (!alive || !target) return;
    if (!busy && !document.hidden) {
      busy = true;
      try {
        // first load: the newest page; then everything after the last id, page by page
        for (let i = 0; i < 5; i++) {
          const p = await getActivity(target, next ? { since: next, limit: 200 } : { limit: 200 });
          entries = mergeEntries(entries, p.entries);
          if (p.next) next = p.next;
          transcripts = p.transcripts;
          if (!p.more || !next) break;
        }
        loaded = true; problem = null;
      } catch (e) {
        if (e instanceof ActivityApiError && e.code === 'unsupported') unsupported = true;
        else if (e instanceof ActivityApiError && e.code === 'unauthorized') void loadLink();   // the code rotated
        else problem = e instanceof ActivityApiError && e.code === 'unreachable' ? 'can’t reach the relay — is Molty online?' : (e as Error).message;
      } finally { busy = false; }
      // re-render only on news: an open select or a scrolled list must survive the poll
      const sig = `${entries.length}|${next}|${transcripts}|${problem}|${loaded}|${unsupported}`;
      if (sig !== lastSig) { lastSig = sig; render(); }
    }
    if (alive && !unsupported) timer = window.setTimeout(() => void poll(), POLL_MS);
  }

  const onVisible = () => { if (!document.hidden) void poll(); };
  document.addEventListener('visibilitychange', onVisible);

  async function toggleTranscripts(on: boolean) {
    if (!target || on === transcripts) return;
    try { transcripts = (await setTranscripts(target, on)).transcripts; problem = null; }
    catch (e) { problem = `not saved: ${(e as Error).message}`; }
    render();
  }

  let controlsKey = '';
  function renderControls() {
    const agents = agentsOf(entries);
    const key = `${agents.join('\n')}|${filters.agent}|${filters.kind}|${filters.errors}`;
    if (key === controlsKey) return;
    controlsKey = key;
    const agentSel = h('select', { 'aria-label': 'agent', onchange: (ev: Event) => { filters.agent = (ev.target as HTMLSelectElement).value || undefined; render(); } },
      h('option', { value: '', selected: !filters.agent }, 'all agents'),
      agents.map((a) => h('option', { value: a, selected: filters.agent === a }, a)));
    const kindSel = h('select', { 'aria-label': 'kind', onchange: (ev: Event) => { filters.kind = (ev.target as HTMLSelectElement).value || undefined; render(); } },
      h('option', { value: '', selected: !filters.kind }, 'everything'),
      ACTIVITY_KINDS.map((k) => h('option', { value: k.id, selected: filters.kind === k.id }, k.label)));
    const errs = h('button', { type: 'button', class: 'chip' + (filters.errors ? ' on' : ''), 'aria-pressed': String(!!filters.errors),
      onclick: () => { filters.errors = !filters.errors; render(); } }, 'errors only');
    mount(controls, h('div', { class: 'row act-filters' }, agentSel, kindSel, errs));
  }

  function renderSettings() {
    if (!loaded) { mount(settings); return; }
    mount(settings, h('div', { class: 'card' },
      h('div', { class: 'row' }, h('b', { class: 'hi' }, 'log transcripts'), h('span', { class: 'dim small' }, 'what Molty heard and said, in this log')),
      h('div', { class: 'chips', role: 'group', 'aria-label': 'log transcripts' },
        [['on', true], ['off', false]].map(([label, on]) => h('button', {
          type: 'button', class: 'chip' + (transcripts === on ? ' on' : ''), 'aria-pressed': String(transcripts === on),
          onclick: () => void toggleTranscripts(on as boolean),
        }, label as string))),
      h('p', { class: 'dim small' }, transcripts ? 'on: turns keep the words (agents only ever see their own).' : 'off: new turns keep timings and outcomes only; the words are never stored.')));
  }

  function groupEl(g: Group) {
    const failed = g.outcome === 'failed' || g.outcome === 'error';
    const badge = h('span', { class: `badge ${failed ? 'warn' : g.outcome === 'pending' ? '' : 'ok'}` },
      failed ? (g.error ?? 'error') : g.outcome === 'pending' ? 'running' : g.outcome === 'done' ? 'done' : 'ok');
    const head = [
      h('span', { class: 'dim act-time' }, clock(g.ts)),
      h('b', { class: 'act-agent' }, g.agent ?? 'molty'),
      h('span', { class: 'act-op' }, g.job ? g.op : entryLine(g.entries[0])),
      g.ms !== undefined ? h('span', { class: 'dim' }, fmtMs(g.ms)) : null,
      g.job || failed ? badge : null,
    ];
    if (!g.job) return h('div', { class: 'act-item' + (failed ? ' err' : ''), 'data-kind': g.entries[0].kind }, ...head);
    const d = h('details', { class: 'act-turn' + (failed ? ' err' : ''), open: open.has(g.key) },
      h('summary', null, ...head), turnBody(g));
    d.addEventListener('toggle', () => { if (d.open) open.add(g.key); else open.delete(g.key); });
    return d;
  }

  function turnBody(g: Group) {
    const j = g.jobEntry;
    const rows = timingRows(j?.data?.timings);
    const heard = transcript(j, 'heard'), said = transcript(j, 'said');
    const vl = voiceLine(j);
    const reason = j?.data?.reason;
    const ans = j?.data?.answer;
    return h('div', { class: 'act-body' },
      h('p', { class: 'dim small' }, g.job),
      g.call?.args && Object.keys(g.call.args).length ? h('p', { class: 'small' }, h('span', { class: 'dim' }, 'call '), `${g.call.op ?? ''} ${JSON.stringify(g.call.args)}`) : null,
      g.error ? h('p', { class: 'small warn' }, `failed: ${g.error}${j?.message ? ` — ${j.message}` : g.call?.message ? ` — ${g.call.message}` : ''}`) : null,
      vl ? h('p', { class: 'small' }, vl) : null,
      reason ? h('p', { class: 'small' }, h('span', { class: 'dim' }, 'ended '), String(reason)) : null,
      said !== null ? h('p', { class: 'small act-said' }, h('span', { class: 'dim' }, 'said '), said === 'redacted' ? h('i', { class: 'dim' }, 'redacted') : `“${said}”`) : null,
      heard !== null ? h('p', { class: 'small act-heard' }, h('span', { class: 'dim' }, 'heard '), heard === 'redacted' ? h('i', { class: 'dim' }, 'redacted') : `“${heard}”`) : null,
      ans !== undefined ? h('p', { class: 'small' }, h('span', { class: 'dim' }, 'answer '), ans === null ? 'none' : String(ans)) : null,
      rows.length ? h('dl', { class: 'facts act-timings', 'aria-label': 'stage timings' }, rows.map(([k, v]) => [h('dt', null, k), h('dd', null, v)])) : null,
      g.lines.length ? h('pre', { class: 'log act-lines', 'aria-label': 'device log' }, g.lines.map((l) => {
        const lv = l.data?.level;
        return h('span', { class: `act-line ${lv === 'E' ? 'err' : lv === 'W' ? 'warn' : ''}` }, `${clock(l.ts)} ${l.data?.text ?? ''}${l.data?.approx ? ' ~' : ''}\n`);
      })) : h('p', { class: 'dim small' }, 'no device lines for this turn'));
  }

  function render() {
    renderControls();
    renderSettings();
    if (unsupported) { mount(msg, note('warn', 'this relay has no activity log yet.')); mount(list); return; }
    mount(msg, problem ? note('warn', problem) : null);
    if (!loaded) { mount(list, problem ? null : h('p', { class: 'dim' }, 'asking the relay…')); return; }
    const groups = filterGroups(groupEntries(entries), filters).reverse();   // newest first
    if (!groups.length) {
      mount(list, h('p', { class: 'dim' }, entries.length ? 'nothing matches these filters.' : 'nothing yet — pair an agent and talk to Molty.'));
      return;
    }
    mount(list, groups.map(groupEl));
  }

  render();
  void loadLink();
  return () => { alive = false; clearTimeout(timer); clearTimeout(linkTimer); document.removeEventListener('visibilitychange', onVisible); };
}
