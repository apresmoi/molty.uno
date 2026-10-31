// Activity: one timeline of everything that happened on this Molty, with which agent did it
// (activity/activity.ts): turns grouped by job — the agent's call, the job's stage timings,
// voice provider, heard/said and the device's own log lines for it — plus device events,
// pairings and the voice box. Read from the relay's owner API with the link code from
// GET /api/link (like the Voice card); polls every 3 s while the tab is visible.
import { UnsupportedError } from '../api/device';
import type { AppCtx } from '../app';
import {
  ACTIVITY_KINDS, ActivityApiError, agentsOf, audioFileName, entryLine, filterGroups, fmtMs, getActivity, getAudio, groupEntries, mergeEntries, setKeepAudio, setTranscripts, timingRows, transcript, visibleRange, voiceLine,
  type ActivityEntry, type ActivityTarget, type Filters, type Group,
} from '../activity/activity';
import { fmtAt, traceModel, type TraceModel } from '../activity/trace';
import { getSpeech, resetSpeech, SpeechApiError, speechRows, type SpeechStats } from '../speech/speech';
import { linkContext } from '../ui/agent-link';
import { h, mount, note } from '../ui/dom';
import { errText } from '../ui/fmt';
import { more, pageHead, section, toggle } from '../ui/kit';

const POLL_MS = 3000;
const LINK_MS = 60000;   // the link code rotates every 15 min
const ROW = 24;          // px, one line of the log: .act-row in style.css
const PAGE = 500;        // the relay's largest page
const KEEP = 4000;       // above the relay's 3072 entries, so the whole 24 h stays searchable

const clock = (ts: number) => new Date(ts).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });

export function activityPage(root: HTMLElement, { api }: AppCtx) {
  let alive = true, timer = 0, linkTimer = 0, busy = false;
  let target: ActivityTarget | null = null;
  let entries: ActivityEntry[] = [];
  let next = '';
  let loaded = false, unsupported = false, transcripts = true;
  let keepAudio: boolean | undefined;   // undefined: this relay has no such setting
  let audioInfo = { turns: 5, max_s: 30, max_age_h: 24 };
  let problem: string | null = null;
  const filters: Filters = {};
  const open = new Set<string>();
  let lastSig = '';
  let shown: Group[] = [];
  let selected: string | null = null;
  let detailSig = '';
  let follow = true;   // stay on the newest line until they scroll up
  let speech: SpeechStats | null = null;
  let speechAt = 0;
  const SPEECH_MS = 30000;   // the habits change per turn: no need to poll them every 3 s

  const controls = h('div', { class: 'act-controls' });
  const settings = h('div', { class: 'act-settings' });
  const habits = h('div', { class: 'act-speech' });
  const msg = h('div', { 'aria-live': 'polite' });
  const search = h('input', { type: 'search', class: 'act-search', placeholder: 'Search words, errors, agents…', 'aria-label': 'search the log',
    oninput: () => { filters.q = search.value; renderList(); },
    onkeydown: (ev: KeyboardEvent) => { if (ev.key === 'Escape') { search.value = ''; filters.q = ''; renderList(); } } }) as HTMLInputElement;
  const rows = h('div', { class: 'act-rows' });
  const term = h('div', { class: 'act-term', tabindex: '0', role: 'listbox', 'aria-label': 'activity log', onscroll: onScroll, onkeydown: onTermKey }, rows);
  const newest = h('button', { type: 'button', class: 'act-newest', onclick: () => { follow = true; renderList(); } }, '↓ newest');
  const status = h('div', { class: 'act-status dim', 'aria-live': 'polite' });
  const detail = h('div', { class: 'act-detail' });
  mount(root,
    ...pageHead('Activity', 'What happened on this Molty and which agent did it, for the last 24 hours.'),
    search, controls, msg, h('div', { class: 'act-log' }, term, newest, status), detail, settings, more('How you talk', habits));

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
        // from the oldest kept entry, page by page, then everything after the last id
        for (let i = 0; i < 10; i++) {
          const p = await getActivity(target, { since: next || 'a0', limit: PAGE });
          entries = mergeEntries(entries, p.entries, KEEP);
          if (p.next) next = p.next;
          transcripts = p.transcripts;
          keepAudio = p.keep_audio;
          if (p.audio) audioInfo = p.audio;
          if (!p.more || !next) break;
        }
        loaded = true; problem = null;
        if (Date.now() - speechAt > SPEECH_MS) {
          speechAt = Date.now();
          try { speech = await getSpeech(target); }
          catch (e) { speech = null; if (!(e instanceof SpeechApiError)) throw e; }
        }
      } catch (e) {
        if (e instanceof ActivityApiError && e.code === 'unsupported') unsupported = true;
        else if (e instanceof ActivityApiError && e.code === 'unauthorized') void loadLink();   // the code rotated
        else problem = e instanceof ActivityApiError && e.code === 'unreachable' ? 'Can’t reach the relay. Is Molty online?' : (e as Error).message;
      } finally { busy = false; }
      // re-render only on news: an open select or a scrolled list must survive the poll
      const sig = `${entries.length}|${next}|${transcripts}|${keepAudio}|${problem}|${loaded}|${unsupported}|${speechAt}`;
      if (sig !== lastSig) { lastSig = sig; render(); }
    }
    if (alive && !unsupported) timer = window.setTimeout(() => void poll(), POLL_MS);
  }

  const onVisible = () => { if (!document.hidden) void poll(); };
  document.addEventListener('visibilitychange', onVisible);
  const onSlash = (ev: KeyboardEvent) => {
    if (ev.key !== '/' || (ev.target as HTMLElement).closest('input, select, textarea')) return;
    ev.preventDefault();
    search.focus();
  };
  document.addEventListener('keydown', onSlash);

  async function toggleTranscripts(on: boolean) {
    if (!target || on === transcripts) return;
    try { transcripts = (await setTranscripts(target, on)).transcripts; problem = null; }
    catch (e) { problem = `not saved: ${(e as Error).message}`; }
    render();
  }

  async function toggleKeepAudio(on: boolean) {
    if (!target || on === keepAudio) return;
    try { const p = await setKeepAudio(target, on); keepAudio = p.keep_audio ?? on; problem = null; }
    catch (e) { problem = `not saved: ${(e as Error).message}`; }
    render();
  }

  async function download(job: string, btn: HTMLButtonElement) {
    if (!target) return;
    const label = btn.textContent;
    btn.disabled = true; btn.textContent = 'downloading…';
    try {
      const blob = await getAudio(target, job);
      const url = URL.createObjectURL(blob);
      const a = h('a', { href: url, download: audioFileName(job), style: 'display:none' });
      document.body.appendChild(a); a.click(); a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 10000);
      btn.textContent = 'saved ⠿';
    } catch (e) {
      btn.textContent = e instanceof ActivityApiError && e.code === 'not_found' ? 'audio expired' : `failed: ${(e as Error).message}`;
    } finally {
      btn.disabled = false;
      setTimeout(() => { btn.textContent = label; }, 2500);
    }
  }

  let controlsKey = '';
  function renderControls() {
    const agents = agentsOf(entries);
    const key = `${agents.join('\n')}|${filters.agent}|${filters.kind}|${filters.errors}`;
    if (key === controlsKey) return;
    controlsKey = key;
    const agentSel = h('select', { 'aria-label': 'agent', onchange: (ev: Event) => { filters.agent = (ev.target as HTMLSelectElement).value || undefined; render(); } },
      h('option', { value: '', selected: !filters.agent }, 'All agents'),
      agents.map((a) => h('option', { value: a, selected: filters.agent === a }, a)));
    const kindSel = h('select', { 'aria-label': 'kind', onchange: (ev: Event) => { filters.kind = (ev.target as HTMLSelectElement).value || undefined; render(); } },
      h('option', { value: '', selected: !filters.kind }, 'Everything'),
      ACTIVITY_KINDS.map((k) => h('option', { value: k.id, selected: filters.kind === k.id }, k.label)));
    const errs = h('button', { type: 'button', class: 'chip' + (filters.errors ? ' on' : ''), 'aria-pressed': String(!!filters.errors),
      onclick: () => { filters.errors = !filters.errors; render(); } }, 'Errors only');
    mount(controls, h('div', { class: 'row act-filters' }, agentSel, kindSel, errs));
  }

  function renderSettings() {
    if (!loaded) { mount(settings); return; }
    mount(settings, section('Privacy', 'What the relay keeps. Agents only ever see their own turns.',
      toggle('Keep the words', transcripts ? 'What Molty heard and said is stored in this log.' : 'New turns keep timings and outcomes only, never the words.', transcripts, toggleTranscripts),
      keepAudio === undefined ? null : !transcripts ? h('p', { class: 'why' }, 'No audio is kept while the words are off.')
        : toggle('Keep recordings for debugging', `The last ${audioInfo.turns} turns (up to ${audioInfo.max_s} s each), deleted after ${audioInfo.max_age_h} h. Only you can download them, here.`, keepAudio, toggleKeepAudio)));
  }

  async function startOver() {
    if (!target) return;
    try { speech = await resetSpeech(target); speechAt = Date.now(); problem = null; }
    catch (e) { problem = `not reset: ${(e as Error).message}`; }
    render();
  }

  /** How the human talks and what the relay tuned from it (docs/AGENT-LINK.md "Calibration"); hidden on a relay without it. */
  function renderHabits() {
    if (!loaded || !speech) { mount(habits); return; }
    const rows = speechRows(speech);
    mount(habits, h('div', null,
      h('div', { class: 'row' }, h('span', { class: 'why' }, 'Learned by the relay to tune how it listens.')),
      h('dl', { class: 'facts', 'aria-label': 'speech habits' }, rows.map(([k, v]) => [h('dt', null, k), h('dd', null, v)])),
      h('p', { class: 'dim small' }, 'Agents get your words in chunks while you talk (context only) and may have a short opener ready; the relay tunes both to how you speak.'),
      h('button', { type: 'button', class: 'link', onclick: () => void startOver() }, 'Start over')));
  }

  const failed = (g: Group) => g.outcome === 'failed' || g.outcome === 'error';

  /** One line of the log. A fixed height is what lets the list draw only what is on screen. */
  function rowEl(g: Group, i: number) {
    return h('div', { class: 'act-row' + (failed(g) ? ' err' : '') + (g.key === selected ? ' on' : ''), style: `top:${i * ROW}px`, role: 'option', 'aria-selected': String(g.key === selected), 'data-kind': g.entries[0].kind,
      onclick: () => { select(g.key === selected ? null : g.key); } },
      h('span', { class: 'dim' }, clock(g.ts)),
      h('b', { class: 'act-agent' }, g.agent ?? 'molty'),
      h('span', { class: 'act-op' }, g.job ? g.op : entryLine(g.entries[0])),
      h('span', { class: 'dim' }, g.ms !== undefined ? fmtMs(g.ms) : ''),
      h('span', { class: failed(g) ? 'tone-err' : 'dim' }, failed(g) ? (g.error ?? 'error') : g.outcome === 'pending' ? 'running' : g.job ? 'done' : ''));
  }

  function drawRows() {
    const [from, to] = visibleRange(term.scrollTop, term.clientHeight, ROW, shown.length);
    mount(rows, shown.slice(from, to).map((g, i) => rowEl(g, from + i)));
  }

  function onScroll() {
    follow = term.scrollTop + term.clientHeight >= term.scrollHeight - ROW;
    newest.classList.toggle('show', !follow);
    drawRows();
  }

  function select(key: string | null) {
    selected = key;
    drawRows();
    renderDetail();
  }

  function onTermKey(ev: KeyboardEvent) {
    const step = ({ ArrowDown: 1, ArrowUp: -1, PageDown: 15, PageUp: -15 } as Record<string, number>)[ev.key];
    if (!step || !shown.length) return;
    ev.preventDefault();
    const at = shown.findIndex((g) => g.key === selected);
    const i = Math.max(0, Math.min(shown.length - 1, (at < 0 ? shown.length : at) + step));
    if (i * ROW < term.scrollTop) term.scrollTop = i * ROW;
    else if ((i + 1) * ROW > term.scrollTop + term.clientHeight) term.scrollTop = (i + 1) * ROW - term.clientHeight;
    select(shown[i].key);
  }

  function renderDetail() {
    const g = shown.find((x) => x.key === selected);
    const sig = g ? `${g.key}|${g.entries.length}|${g.jobEntry?.data?.status ?? ''}` : '';
    if (sig === detailSig) return;   // an open timeline or a scrolled device log must survive the poll
    detailSig = sig;
    if (!g) { mount(detail); return; }
    mount(detail, h('div', { class: 'row act-detail-head' },
      h('b', null, `${clock(g.ts)} ${g.agent ?? 'molty'} ${g.job ? g.op : ''}`),
      h('button', { type: 'button', class: 'link', onclick: () => select(null) }, 'Close')),
      g.job ? turnBody(g) : h('p', { class: 'small' }, entryLine(g.entries[0])));
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
      j?.data?.audio ? h('p', null, h('button', { type: 'button', class: 'ghost small', onclick: (ev: Event) => void download(g.job!, ev.currentTarget as HTMLButtonElement) }, 'download audio (wav)')) : null,
      timelineEl(g),
      g.lines.length ? h('pre', { class: 'log act-lines', 'aria-label': 'device log' }, g.lines.map((l) => {
        const lv = l.data?.level;
        return h('span', { class: `act-line ${lv === 'E' ? 'err' : lv === 'W' ? 'warn' : ''}` }, `${clock(l.ts)} ${l.data?.text ?? ''}${l.data?.approx ? ' ~' : ''}\n`);
      })) : h('p', { class: 'dim small' }, 'no device lines for this turn'));
  }

  function timelineEl(g: Group) {
    const m = traceModel(g.jobEntry, g.traces);
    if (!m) return null;
    const key = `${g.key}#trace`;
    const top = m.summary[0];
    const d = h('details', { class: 'act-trace', open: open.has(key) },
      h('summary', null, h('span', { class: 'dim' }, 'timeline '), h('span', { class: `tone-${top.tone}` }, top.text)),
      m.summary.length > 1 ? h('ul', { class: 'act-sum', 'aria-label': 'trace summary' }, m.summary.slice(1).map((x) => h('li', { class: `tone-${x.tone}` }, x.text))) : null,
      traceSvg(m),
      m.rssi ? h('p', { class: 'dim small' }, `Wi-Fi ${m.rssi[0] ?? '?'} → ${m.rssi[1] ?? '?'} dBm`) : null,
      m.words.length ? h('p', { class: 'small act-words', 'aria-label': 'words' }, h('span', { class: 'dim' }, 'words '), m.words.map((w) => `${w.text}@${fmtAt(w.at)}`).join(' ')) : null,
      h('ol', { class: 'act-events', 'aria-label': 'turn events' }, m.events.map((e) =>
        h('li', { class: `tone-${e.tone}` }, h('span', { class: 'dim act-at' }, fmtAt(e.at)), h('span', { class: 'dim act-lane' }, e.lane), h('span', null, e.label)))));
    d.addEventListener('toggle', () => { if (d.open) open.add(key); else open.delete(key); });
    return d;
  }

  function render() {
    renderControls();
    renderHabits();
    renderSettings();
    mount(msg, unsupported ? note('warn', 'This relay has no activity log yet.') : problem ? note('warn', problem) : null);
    renderList();
  }

  function renderList() {
    const all = loaded ? groupEntries(entries) : [];
    shown = filterGroups(all, filters);   // oldest first, the newest at the bottom like a terminal
    rows.style.height = `${shown.length * ROW}px`;
    status.textContent = unsupported ? '' : !loaded ? (problem ? '' : 'Asking the relay…')
      : !all.length ? 'Nothing yet. Pair an agent and talk to Molty.'
      : !shown.length ? 'Nothing matches.'
      : `${shown.length === all.length ? all.length : `${shown.length} of ${all.length}`} lines`;
    if (follow) term.scrollTop = term.scrollHeight;
    newest.classList.toggle('show', !follow);
    drawRows();
    renderDetail();
  }

  render();
  void loadLink();
  return () => { alive = false; clearTimeout(timer); clearTimeout(linkTimer); document.removeEventListener('visibilitychange', onVisible); document.removeEventListener('keydown', onSlash); };
}

const NS = 'http://www.w3.org/2000/svg';
function svg(tag: string, attrs: Record<string, string | number>, ...kids: Array<SVGElement | string>): SVGElement {
  const el = document.createElementNS(NS, tag) as SVGElement;
  for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, String(v));
  for (const c of kids) el.append(c);
  return el;
}

/**
 * The turn as one picture, ms from t0 left to right: the mic level (100 ms bars, clipping in
 * red), the box's speech spans and words, its pause estimate against the threshold, the frames
 * as they reached the relay (resent amber, lost red), and every event as a tick.
 */
function traceSvg(m: TraceModel): SVGElement {
  const W = 360, H = 118, span = Math.max(1, m.to - m.from);
  const x = (at: number) => +(((at - m.from) / span) * W).toFixed(1);
  const kids: SVGElement[] = [];
  const tone = (t: string) => (t === 'err' ? 'var(--err)' : t === 'warn' ? 'var(--m-warn)' : t === 'ok' ? 'var(--m-accent)' : 'var(--m-dim)');
  for (const e of m.events) kids.push(svg('line', { x1: x(e.at), x2: x(e.at), y1: 0, y2: 100, stroke: tone(e.tone), 'stroke-width': 0.6, 'stroke-dasharray': '2 2', opacity: 0.8 }, svg('title', {}, `${fmtAt(e.at)} ${e.label}`)));
  // level: -90 dBFS = empty, 0 dBFS = full (30 px)
  const bw = Math.max(0.6, (100 / span) * W - 0.3);
  for (const l of m.levels) {
    const v = Math.max(0, Math.min(1, (l.db + 90) / 90)), hh = Math.max(0.5, v * 30);
    kids.push(svg('rect', { x: x(l.at), y: 32 - hh, width: bw, height: hh, fill: 'var(--m-accent)', opacity: 0.75 }));
  }
  for (const c of m.clips) kids.push(svg('circle', { cx: x(c.at + 50), cy: 3, r: 2, fill: 'var(--err)' }, svg('title', {}, `${c.n} clipped`)));
  for (const [a, b] of m.speech) kids.push(svg('rect', { x: x(a), y: 38, width: Math.max(1, x(b) - x(a)), height: 6, rx: 2, fill: 'var(--m-accent)', opacity: 0.5 }));
  for (const w of m.words) kids.push(svg('line', { x1: x(w.at), x2: x(w.at), y1: 46, y2: 52, stroke: 'var(--m-ink)', 'stroke-width': 0.8 }, svg('title', {}, w.text)));
  if (m.pause.length > 1) {
    const pts = m.pause.map((p) => `${x(p.at)},${(78 - p.v * 22).toFixed(1)}`).join(' ');
    kids.push(svg('polyline', { points: pts, fill: 'none', stroke: 'var(--m-warn)', 'stroke-width': 1 }));
    if (m.threshold !== undefined) kids.push(svg('line', { x1: 0, x2: W, y1: 78 - m.threshold * 22, y2: 78 - m.threshold * 22, stroke: 'var(--m-faint)', 'stroke-dasharray': '3 3' }));
  }
  for (const f of m.frames) kids.push(svg('circle', { cx: x(f.at), cy: 88, r: f.rt ? 2.4 : 1.6, fill: f.rt ? 'var(--m-warn)' : 'var(--m-dim)' }, svg('title', {}, `frame ${f.seq}${f.rt ? `, ${f.rt} failed sends` : ''}`)));
  const lostEv = m.events.filter((e) => e.tone === 'err' && e.lane === 'relay');
  for (const e of lostEv) kids.push(svg('rect', { x: x(e.at) - 2, y: 84, width: 4, height: 8, fill: 'var(--err)' }));
  // t0 gets a label only when it is clear of both ends (labels ~40 px wide)
  const axis = [m.from, ...(x(0) > 40 && x(0) < W - 40 ? [0] : []), m.to].filter((v, i, a) => a.indexOf(v) === i);
  for (const t of axis) kids.push(svg('text', { x: Math.min(W - 2, Math.max(2, x(t))), y: 112, 'font-size': 9, fill: 'var(--m-dim)', 'text-anchor': t === m.from ? 'start' : t === m.to ? 'end' : 'middle' }, fmtAt(t)));
  const labels: Array<[number, string]> = [[12, 'level'], [44, 'speech'], [70, 'pause'], [91, 'frames']];
  for (const [y, t] of labels) kids.push(svg('text', { x: W - 1, y, 'font-size': 8, fill: 'var(--m-faint)', 'text-anchor': 'end' }, t));
  return svg('svg', { viewBox: `0 0 ${W} ${H}`, class: 'act-trace-svg', role: 'img', 'aria-label': 'turn timeline' }, ...kids);
}
