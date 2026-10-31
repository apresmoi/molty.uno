// Home: is everything alright, and where to change it. The numbers are one click away.
import type { DeviceInfo, LinkInfo, FluidState, WifiStatus, CreatureRegistry } from '../api/types';
import type { AppCtx } from '../app';
import { linkContext } from '../ui/agent-link';
import { h, mount, note } from '../ui/dom';
import { bars, errText, uptime } from '../ui/fmt';
import { more, statusRow } from '../ui/kit';
import { preview } from '../ui/preview';
import { updateCard } from '../ui/update-card';
import { boxLine, getVoice, reflexOn, voiceOn, type VoiceStatus } from '../voice/voice';

const voiceSummary = (v: VoiceStatus) => {
  const who = voiceOn(v);
  if (who === 'off') return 'Text only';
  const r = reflexOn(v.reflex);
  return [who === 'cloud' ? 'Cloud voice' : 'Voice box', v.first_words?.mode === 'opener' ? 'first words' : null, r && r !== 'off' ? 'reactions' : null].filter(Boolean).join(', ');
};

export function statusPage(root: HTMLElement, { api }: AppCtx) {
  const pv = preview(api, { size: 300 });
  const rows = h('div', { class: 'srows' });
  const facts = h('dl', { class: 'facts' });
  const msg = h('div');
  const upd = updateCard(api);
  mount(root, h('div', { class: 'split' }, h('div', { class: 'stick' }, pv.el),
    h('div', null, h('h1', null, 'Your Molty'), msg, rows, upd.el, more('Device details', facts))));

  let alive = true, timer = 0, voice: VoiceStatus | null = null;
  const fact = (k: string, v: string) => [h('dt', null, k), h('dd', null, v)];

  const refresh = async () => {
    const [info, wifi, reg, st, ln] = await Promise.allSettled([api.info(), api.wifiStatus(), api.creatures(), api.creatureState(), api.link()]);
    if (!alive) return;
    const val = <T,>(r: PromiseSettledResult<unknown>) => (r.status === 'fulfilled' ? (r.value as T) : null);
    const i = val<DeviceInfo>(info), w = val<WifiStatus>(wifi), r = val<CreatureRegistry>(reg), s = val<FluidState>(st), l = val<LinkInfo>(ln);
    if (!voice && l?.device_id && l.link_code) {
      const c = linkContext(l);
      voice = await getVoice({ relay: c.relay, deviceId: c.deviceId, linkCode: c.linkCode }).catch(() => null);
      if (!alive) return;
    }
    mount(msg, !i && info.status === 'rejected' ? note('err', `Can’t reach Molty: ${errText(info.reason)}`) : null);
    const up = w?.state === 'connected';
    const boxDown = voice && boxLine(voice).kind === 'warn';
    mount(rows,
      statusRow('Creature', s?.creature_info?.name ?? r?.current ?? '…', 'dim', '#/molty'),
      statusRow('Voice', voice ? voiceSummary(voice) + (boxDown ? ' (box offline)' : '') : l ? 'See settings' : '…', boxDown ? 'warn' : 'dim', '#/voice'),
      statusRow('Agent', !l ? '…' : l.connected ? (l.last_pair?.ok ? `${l.last_pair.agent_name} is connected` : 'Connected') : 'Not connected', l?.connected ? 'ok' : 'warn', '#/agent'),
      statusRow('Wi-Fi', !w ? '…' : up ? `${w.ssid} ${bars(w.rssi)}` : w.state.replace('_', ' '), up ? 'ok' : 'warn', '#/wifi'));
    mount(facts,
      fact('Device', l?.device_id ?? '—'),
      fact('Firmware', i ? `${i.fw ?? 'build'} (${i.fw_build}, IDF ${i.idf_version})` : '—'),
      fact('Console', __MOLTY_VERSION__),
      fact('Running for', i ? `${uptime(i.uptime_ms)}, boot #${i.boot_count ?? '?'} (${i.reset_reason ?? '?'})` : '—'),
      fact('Address', w?.ip ?? '—'),
      fact('Signal', up ? `${w!.rssi} dBm` : '—'),
      fact('Screen', s ? `${s.state}${s.palette ? `, ${s.palette}` : ''}${s.fps !== undefined ? `, ${s.fps.toFixed(0)} fps` : ''}` : '—'),
      fact('Free memory', i ? `${Math.round(i.free_heap / 1024)} KB, ${Math.round(i.free_psram / 1024)} KB PSRAM` : '—'));
    upd.check(i, l);
    timer = window.setTimeout(refresh, 3000);
  };
  refresh();
  return () => { alive = false; clearTimeout(timer); pv.stop(); upd.stop(); };
}
