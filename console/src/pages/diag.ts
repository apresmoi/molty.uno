// Diagnostics: what "laggy" means objectively. Frame-interval histograms for the
// sim and for what reaches the panel, per-second worst frame, heap headroom,
// per-task CPU, and a live tail of the device log ring (ESP_LOG + EVT events).
import type { Diag, FrameStats } from '../api/types';
import type { AppCtx } from '../app';
import { h, mount, note } from '../ui/dom';
import { errText, uptime } from '../ui/fmt';

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
    h('div', null, `${(f.fps ?? 0).toFixed(1)} fps ⠂ p50 ${ms(f.p50_ms)} ⠂ p95 ${ms(f.p95_ms)} ⠂ p99 ${ms(f.p99_ms)} ⠂ max ${ms(f.max_ms)}`),
    h('div', { class: (f.over100_total ?? 0) > 0 ? 'warn' : 'dim' },
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
  const top = h('div');
  const logEl = h('pre', { class: 'log' });
  mount(root,
    h('h1', null, 'diagnostics ', h('span', { class: 'dim' }, '⠶ frames, memory, stalls, log')),
    msg, top,
    h('h2', null, 'log tail'), logEl);

  let alive = true, timer = 0, since = 0;
  const lines: string[] = [];
  const refresh = async () => {
    try {
      const d = await api.diag(60);
      if (!alive) return;
      const hp = d.heap, st = d.stalls, n = d.net;
      mount(msg, hp.alloc_failures ? note('warn', `${hp.alloc_failures} allocation failure(s) since boot (last ${hp.last_fail_size} B, caps 0x${hp.last_fail_caps.toString(16)})`) : null);
      mount(top,
        h('div', { class: 'card' },
          h('div', null, `up ${uptime(d.uptime_ms)} ⠂ reset ${d.reset_reason} ⠂ fw ${d.fw}`),
          h('div', { class: hp.internal.min_free < 16384 ? 'warn' : '' },
            `internal ${kb(hp.internal.free)} free (low ${kb(hp.internal.min_free)}, largest ${kb(hp.internal.largest)}) ⠂ psram ${kb(hp.psram.free)}`),
          h('div', { class: Math.max(st.probe_core0_max_ms, st.probe_core1_max_ms) > 250 ? 'warn' : 'dim' },
            `starvation probes: core0 ${st.probe_core0_max_ms} ms, core1 ${st.probe_core1_max_ms} ms ⠂ stall events ${st.gap_events}`),
          h('div', { class: n.link_connected ? '' : 'warn' },
            `wi-fi ${n.rssi} dBm, ps ${n.wifi_ps} ⠂ gateway ping ${n.gw_rtt_last_ms} ms, lost ${n.gw_ping_lost}/${n.gw_ping_sent} ⠂ link ${n.link_connected ? 'up' : 'down'}${n.audio_busy ? ' · audio' : ''}`)),
        frameCard('sim', d.frames.sim, `work ${ms(d.frames.sim.work_ms)}`),
        frameCard('panel', d.frames.panel, `draw ${ms(d.frames.panel.draw_ms)} ⠂ flush ${ms(d.frames.panel.flush_ms)}`),
        d.frames.touch_to_panel.n ? h('div', { class: 'card' }, h('b', null, 'touch → panel'), ' ',
          `p50 ${ms(d.frames.touch_to_panel.p50_ms)} ⠂ max ${ms(d.frames.touch_to_panel.max_ms)} (${d.frames.touch_to_panel.total} taps)`) : null,
        spark(d),
        h('div', { class: 'card' }, h('b', null, 'tasks'), ' ', h('span', { class: 'dim' }, '% of one core, last second'),
          h('pre', { class: 'tasks' }, d.tasks.filter((t) => t.cpu > 0 || t.name.startsWith('IDLE')).map((t) =>
            `${t.name.padEnd(16)} ${String(t.cpu).padStart(3)}%  core ${t.core < 0 ? '*' : t.core}  prio ${String(t.prio).padStart(2)}  stack ${t.stack_free}\n`).join(''))));
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
      if (alive) mount(msg, note('err', errText(e)));
    }
    if (alive) timer = window.setTimeout(refresh, 2000);
  };
  refresh();
  return () => { alive = false; clearTimeout(timer); };
}
