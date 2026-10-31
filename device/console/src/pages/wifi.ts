// Wi-Fi: same device endpoints as the old console (GET /api/status, GET /api/scan,
// POST /api/set_creds {ssid,password}, POST /api/forget). Forget reboots the device,
// so it sits behind a two-step confirm and is never called implicitly.
import type { WifiNetwork, WifiStatus } from '../api/types';
import type { AppCtx } from '../app';
import { confirmButton, h, mount, note } from '../ui/dom';
import { bars, errText } from '../ui/fmt';
import { more, pageHead, section, statusRow } from '../ui/kit';

export function wifiPage(root: HTMLElement, { api }: AppCtx) {
  const now = h('div', { class: 'srows' });
  const manage = h('div');
  const list = h('div', { class: 'list', role: 'group', 'aria-label': 'networks' });
  const form = h('div');
  const msg = h('div');
  const scanBtn = h('button', { type: 'button', class: 'primary', onclick: () => scan() }, 'Look for networks');
  let alive = true, timer = 0, status: WifiStatus | null = null;

  mount(root,
    ...pageHead('Wi-Fi', 'The network Molty uses. It needs 2.4 GHz.'),
    now, msg,
    section('Change network', null, scanBtn, list, form),
    manage);

  const renderNow = () => {
    const s = status;
    const up = s?.state === 'connected', rssi = s?.rssi ?? 0;
    mount(now, !s ? h('p', { class: 'why' }, 'Checking…') : [
      statusRow('Network', up ? s.ssid ?? '' : s.ssid ? `Not connected (last: ${s.ssid})` : 'No network saved', up ? 'ok' : 'warn'),
      up ? statusRow('Signal', `${bars(rssi)} ${rssi < -80 ? 'Weak' : rssi < -67 ? 'OK' : 'Strong'} (${rssi} dBm)`, rssi < -80 ? 'warn' : 'dim') : null,
      up ? statusRow('Address', s.ip ?? '—', 'dim') : null]);
    mount(manage, s?.ssid ? more('Forget this network', h('p', { class: 'why' }, 'Molty restarts in setup mode and makes its own Wi-Fi until you connect it again.'), confirmButton('Forget and restart', 'Sure? Forget', forget)) : null);
  };

  const poll = async () => {
    try { status = await api.wifiStatus(); } catch (e) { status = null; mount(now, note('err', errText(e))); }
    if (!alive) return;
    if (status) renderNow();
    timer = window.setTimeout(poll, 4000);
  };

  const scan = async () => {
    scanBtn.disabled = true; scanBtn.textContent = 'Looking…';
    mount(list);
    try {
      const nets = await api.wifiScan();
      const seen = new Set<string>();
      const uniq = nets.filter((n) => n.ssid && !seen.has(n.ssid) && seen.add(n.ssid));
      mount(list, uniq.length ? uniq.map(netRow) : h('p', { class: 'why' }, 'No networks found. Try again.'));
    } catch (e) { mount(list, note('err', errText(e))); }
    finally { if (alive) { scanBtn.disabled = false; scanBtn.textContent = 'Look again'; } }
  };

  const netRow = (n: WifiNetwork) =>
    h('button', { type: 'button', class: 'opt' + (n.ssid === status?.ssid && status?.state === 'connected' ? ' on' : ''), onclick: () => pick(n.ssid, n.open) },
      h('span', { class: 'ssid' }, n.ssid), h('span', { class: 'meta' }, `${n.ssid === status?.ssid && status?.state === 'connected' ? 'in use ' : n.open ? 'open ' : ''}${bars(n.rssi)}`));

  const pick = (ssid: string, open: boolean) => {
    const pass = h('input', { type: 'password', placeholder: 'Password', autocomplete: 'current-password', 'aria-label': 'password' });
    const ssidIn = h('input', { type: 'text', value: ssid, 'aria-label': 'network name', placeholder: 'Network name' });
    const go = h('button', { class: 'primary', type: 'submit' }, 'Connect');
    const f = h('form', { class: 'card connect' },
      h('label', null, 'Network', ssidIn), open ? null : h('label', null, 'Password', pass),
      h('div', { class: 'row' }, h('span', { class: 'dim small' }, 'Takes up to 15 seconds'), go));
    f.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      go.disabled = true; go.textContent = 'Connecting…';
      mount(msg);
      try {
        const r = await api.wifiConnect(ssidIn.value.trim(), open ? '' : pass.value);
        mount(msg, note('ok', `Connected. Molty saved this network (${r.ip ?? ''}).`));
        mount(form);
        poll();
      } catch (e) {
        mount(msg, note('err', errText(e) === 'molty said: connect_failed' ? 'Couldn’t join. Check the password, and that the network is 2.4 GHz.' : errText(e)));
      } finally { go.disabled = false; go.textContent = 'Connect'; }
    });
    mount(form, f);
    (open ? go : pass).focus();
  };

  async function forget() {
    try { await api.wifiForget(); mount(msg, note('warn', 'Forgotten. Molty is restarting in setup mode (about 10 seconds).')); }
    catch (e) { mount(msg, note('err', errText(e))); }
  }

  mount(form, h('button', { type: 'button', class: 'link', onclick: () => pick('', false) }, 'Enter a hidden network'));
  renderNow(); poll();
  return () => { alive = false; clearTimeout(timer); };
}
