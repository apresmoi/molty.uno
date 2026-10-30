import type { DeviceInfo, LinkInfo, FluidState, WifiStatus, CreatureRegistry } from '../api/types';
import type { AppCtx } from '../app';
import { h, mount, note } from '../ui/dom';
import { bars, errText, uptime } from '../ui/fmt';
import { preview } from '../ui/preview';
import { updateCard } from '../ui/update-card';

export function statusPage(root: HTMLElement, { api }: AppCtx) {
  const pv = preview(api, { size: 300 });
  const facts = h('dl', { class: 'facts' });
  const msg = h('div');
  const upd = updateCard(api);
  mount(root,
    h('h1', null, 'hello ', h('span', { class: 'dim' }, '⠶ this is your molty')),
    h('div', { class: 'split' }, pv.el, h('div', null, facts, upd.el, msg)));

  let alive = true, timer = 0;
  const row = (k: string, v: unknown, cls = '') => [h('dt', null, k), h('dd', { class: cls }, v as string)];

  const refresh = async () => {
    const [info, wifi, reg, st, ln] = await Promise.allSettled([api.info(), api.wifiStatus(), api.creatures(), api.creatureState(), api.link()]);
    if (!alive) return;
    const i = info.status === 'fulfilled' ? (info.value as DeviceInfo) : null;
    const w = wifi.status === 'fulfilled' ? (wifi.value as WifiStatus) : null;
    const r = reg.status === 'fulfilled' ? (reg.value as CreatureRegistry) : null;
    const s = st.status === 'fulfilled' ? (st.value as FluidState) : null;
    const l = ln.status === 'fulfilled' ? (ln.value as LinkInfo) : null;
    mount(msg, !i && info.status === 'rejected' ? note('err', errText(info.reason)) : null);
    mount(facts,
      row('device', l?.device_id ?? '—', 'hi'),
      row('firmware', i ? `${i.fw ?? 'build'} ${i.fw_build} ⠂ idf ${i.idf_version}` : '—'),
      row('console', __MOLTY_VERSION__, i?.fw && i.fw !== __MOLTY_VERSION__ ? 'warn' : ''),
      row('uptime', i ? `${uptime(i.uptime_ms)} ⠂ boot #${i.boot_count ?? '?'} (${i.reset_reason ?? '?'})` : '—'),
      row('wi-fi', w ? (w.state === 'connected' ? `${w.ssid} ${bars(w.rssi)} ${w.rssi} dBm` : `${w.state.replace('_', ' ')}${w.ssid ? ' · ' + w.ssid : ''}`) : '—',
        w?.state === 'connected' ? 'ok' : 'warn'),
      row('address', w?.ip ?? '—'),
      row('molty', r ? r.current : '—'),
      row('state', s ? `${s.state}${s.autoplay ? ' · autoplay' : ''}${s.palette ? ' · ' + s.palette : ''}` : '—'),
      row('fps', s?.fps !== undefined ? `${s.fps.toFixed(1)}` : '—'),
      row('memory', i ? `${Math.round(i.free_heap / 1024)} KB heap ⠂ ${Math.round(i.free_psram / 1024)} KB psram free` : '—'),
    );
    upd.check(i, l);
    timer = window.setTimeout(refresh, 3000);
  };
  refresh();
  return () => { alive = false; clearTimeout(timer); pv.stop(); upd.stop(); };
}
