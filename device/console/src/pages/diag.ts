// Diagnostics: what "laggy" means objectively. Frame-interval histograms for the
// sim and for what reaches the panel, per-second worst frame, heap headroom,
// per-task CPU, and a live tail of the device log ring (ESP_LOG + EVT events).
import { UnsupportedError } from '../api/device';
import type { Diag, FrameStats } from '../api/types';
import type { AppCtx } from '../app';
import { h, mount, note } from '../ui/dom';
import { errText, uptime } from '../ui/fmt';
import { more, pageHead, section, statusRow } from '../ui/kit';

const BLOCKS = ' ▁▂▃▄▅▆▇█';
const kb = (n: number) => `${(n / 1024).toFixed(1)} KB`;
const ms = (v?: number) => (v === undefined ? '—' : `${v.toFixed(1)} ms`);

function histRows(f: FrameStats) {
  const total = (f.hist ?? []).reduce((a, b) => a + b.n, 0) || 1;
  return (f.hist ?? []).map((b) => {
    const w = Math.round((b.n / total) * 40);
    const bad = b.le === '<=100' || b.le === '<=250' || b.le === '>250';
    return h('div', { class: 'hrow' },
      h('span', { class: 'dim' }, b.le.padStart(6), ' ms '),
      h('span', { class: bad && b.n ? 'warn' : 'hi' }, '█'.repeat(w) || (b.n ? '▏' : '')),
      ' ', h('span', { class: 'dim' }, String(b.n)));
  });
}

function frameCard(title: string, f: FrameStats, extra: string) {
  return h('div', { class: 'card' },
    h('div', null, h('b', null, title), ' ', h('span', { class: 'dim' }, extra)),
    h('div', { class: 'small' }, `${(f.fps ?? 0).toFixed(1)} fps ⠂ p50 ${ms(f.p50_ms)} ⠂ p95 ${ms(f.p95_ms)} ⠂ p99 ${ms(f.p99_ms)} ⠂ max ${ms(f.max_ms)}`),
    h('div', { class: (f.over100_total ?? 0) > 0 ? 'warn small' : 'dim small' },
      `since boot: ${f.over50_total ?? 0} frames >50 ms, ${f.over100_total ?? 0} >100 ms, worst ${ms(f.max_total_ms)} at ${f.max_total_at_s ?? 0} s`),
    h('pre', { class: 'hist' }, histRows(f)));
}

/** One braille-free spark row: per-second worst frame interval, scaled to 250 ms. */
function spark(d: Diag) {
  const rows = d.timeline ?? [];
  const cols = (d.timeline_cols ?? '').split(',');
  const at = (r: Array<number | string>, k: string) => Number(r[cols.indexOf(k)] ?? 0);
  const line = (k: string) => rows.map((r) => BLOCKS[Math.min(8, Math.round((at(r, k) / 250) * 8))]).join('');
  const bad = rows.filter((r) => at(r, 'sim_max_ms') > 100 || at(r, 'panel_max_ms') > 100).length;
  return h('div', { class: 'card' },
    h('div', null, h('b', null, 'last ', String(rows.length), ' s'), ' ', h('span', { class: 'dim' }, 'worst frame per second (full block = 250 ms)')),
    h('pre', { class: 'spark' }, `sim   ${line('sim_max_ms')}\npanel ${line('panel_max_ms')}`),
    h('div', { class: bad ? 'warn' : 'dim' }, bad ? `${bad} second(s) with a frame over 100 ms` : 'no frame over 100 ms'));
}

export function diagPage(root: HTMLElement, { api }: AppCtx) {
  const msg = h('div');
  const health = h('div', { class: 'srows' });
  const frames = h('div'), tasks = h('div');
  const logEl = h('pre', { class: 'log' });
  mount(root,
    ...pageHead('Diagnostics', 'If Molty feels slow or stops answering, start here.'),
    msg, section('Health', null, health),
    more('Frame timing', frames), more('Tasks', tasks), more('Device log', logEl));

  let alive = true, timer = 0, since = 0;
  const lines: string[] = [];
  const refresh = async () => {
    try {
      const d = await api.diag(60);
      if (!alive) return;
      const hp = d.heap, st = d.stalls, n = d.net, fps = d.frames.panel.fps ?? 0;
      const low = hp.internal.min_free < 16384, stalled = Math.max(st.probe_core0_max_ms, st.probe_core1_max_ms) > 250;
      mount(msg, hp.alloc_failures ? note('warn', `${hp.alloc_failures} allocation failure(s) since boot (last ${hp.last_fail_size} B, caps 0x${hp.last_fail_caps.toString(16)})`) : null);
      mount(health,
        statusRow('Screen', `${fps.toFixed(0)} frames per second`, fps < 20 ? 'warn' : 'ok'),
        statusRow('Memory', `${low ? 'Low' : 'OK'}: ${kb(hp.internal.free)} free (lowest ${kb(hp.internal.min_free)})`, low ? 'warn' : 'ok'),
        statusRow('Smoothness', stalled ? `Stalled up to ${Math.max(st.probe_core0_max_ms, st.probe_core1_max_ms)} ms` : `No long stalls (${st.gap_events} short)`, stalled ? 'warn' : 'ok'),
        statusRow('Wi-Fi', `${n.rssi} dBm, ${n.gw_ping_lost} of ${n.gw_ping_sent} pings lost`, n.rssi < -80 ? 'warn' : 'ok'),
        statusRow('Relay', n.link_connected ? `Connected${n.audio_busy ? ', audio running' : ''}` : 'Not connected', n.link_connected ? 'ok' : 'warn'),
        statusRow('Running for', `${uptime(d.uptime_ms)}, last restart: ${d.reset_reason}, firmware ${d.fw}`, 'dim'));
      mount(frames,
        frameCard('Simulation', d.frames.sim, `work ${ms(d.frames.sim.work_ms)}`),
        frameCard('Screen', d.frames.panel, `draw ${ms(d.frames.panel.draw_ms)}, flush ${ms(d.frames.panel.flush_ms)}`),
        d.frames.touch_to_panel.n ? h('div', { class: 'card' }, h('b', null, 'Touch to screen'), ' ',
          `p50 ${ms(d.frames.touch_to_panel.p50_ms)}, max ${ms(d.frames.touch_to_panel.max_ms)} (${d.frames.touch_to_panel.total} taps)`) : null,
        spark(d));
      mount(tasks, h('p', { class: 'why' }, 'Percent of one core over the last second.'),
        h('pre', { class: 'tasks' }, d.tasks.filter((t) => t.cpu > 0 || t.name.startsWith('IDLE')).map((t) =>
          `${t.name.padEnd(16)} ${String(t.cpu).padStart(3)}%  core ${t.core < 0 ? '*' : t.core}  prio ${String(t.prio).padStart(2)}  stack ${t.stack_free}\n`).join('')));
      const lg = await api.log(since);
      if (!alive) return;
      since = lg.next;
      if (lg.text) {
        lines.push(...lg.text.replace(/\n$/, '').split('\n'));
        lines.splice(0, Math.max(0, lines.length - 300));
        const stick = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 8;
        logEl.textContent = lines.join('\n');
        if (stick) logEl.scrollTop = logEl.scrollHeight;
      }
    } catch (e) {
      if (alive) mount(msg, note(e instanceof UnsupportedError ? 'warn' : 'err', e instanceof UnsupportedError ? 'This firmware has no diagnostics. Update Molty to see them.' : `Can’t reach Molty: ${errText(e)}`));
    }
    if (alive) timer = window.setTimeout(refresh, 2000);
  };
  refresh();
  return () => { alive = false; clearTimeout(timer); };
}
