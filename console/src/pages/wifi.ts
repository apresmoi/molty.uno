// Wi-Fi: same device endpoints as the old console (GET /api/status, GET /api/scan,
// POST /api/set_creds {ssid,password}, POST /api/forget). Forget reboots the device,
// so it sits behind a two-step confirm and is never called implicitly.
import type { WifiNetwork, WifiStatus } from '../api/types';
import type { AppCtx } from '../app';
import { confirmButton, h, mount, note } from '../ui/dom';
import { bars, errText } from '../ui/fmt';

export function wifiPage(root: HTMLElement, { api }: AppCtx) {
  const now = h('div', { class: 'card' });
  const list = h('ul', { class: 'nets', 'aria-label': 'networks' });
  const form = h('div');
  const msg = h('div');
  const scanBtn = h('button', { type: 'button', onclick: () => scan() }, 'scan');
  let alive = true, timer = 0, status: WifiStatus | null = null;

  mount(root,
    h('h1', null, 'wi-fi ', h('span', { class: 'dim' }, '⠶ let molty see the world')),
    now, h('div', { class: 'row' }, h('h2', null, 'networks'), scanBtn), list, form, msg);

  const renderNow = () => {
    const s = status;
    mount(now, !s ? h('span', { class: 'dim' }, 'checking…')
      : h('div', { class: 'row' },
          h('div', null,
            h('div', { class: s.state === 'connected' ? 'ok' : 'warn' }, s.state === 'connected' ? `connected ⠶ ${s.ssid}` : s.state.replace('_', ' ')),
            h('div', { class: 'dim small' }, s.state === 'connected' ? `${s.ip} ⠂ ${bars(s.rssi)} ${s.rssi} dBm` : s.ssid ? `last network: ${s.ssid}` : 'no network saved')),
          s.ssid ? confirmButton('forget', 'forget + reboot?', forget) : null));
  };

  const poll = async () => {
    try { status = await api.wifiStatus(); } catch (e) { status = null; mount(now, note('err', errText(e))); }
    if (!alive) return;
    if (status) renderNow();
    timer = window.setTimeout(poll, 4000);
  };

  const scan = async () => {
    scanBtn.disabled = true; scanBtn.textContent = 'scanning ⠴';
    mount(list, h('li', { class: 'dim' }, 'listening for networks…'));
    try {
      const nets = await api.wifiScan();
      const seen = new Set<string>();
      const uniq = nets.filter((n) => n.ssid && !seen.has(n.ssid) && seen.add(n.ssid));
      mount(list, uniq.length ? uniq.map(netRow) : h('li', { class: 'dim' }, 'no networks found — try again'));
    } catch (e) { mount(list, h('li', null, note('err', errText(e)))); }
    finally { if (alive) { scanBtn.disabled = false; scanBtn.textContent = 'scan again'; } }
  };

  const netRow = (n: WifiNetwork) =>
    h('li', null, h('button', { type: 'button', class: 'net', onclick: () => pick(n.ssid, n.open) },
      h('span', null, n.ssid), h('span', { class: 'dim' }, `${n.open ? '' : '⠿ '}${bars(n.rssi)}`)));

  const pick = (ssid: string, open: boolean) => {
    const pass = h('input', { type: 'password', placeholder: 'password', autocomplete: 'current-password', 'aria-label': 'password' });
    const ssidIn = h('input', { type: 'text', value: ssid, 'aria-label': 'network name', placeholder: 'network name' });
    const go = h('button', { class: 'primary', type: 'submit' }, 'connect');
    const f = h('form', { class: 'card connect' },
      h('label', null, 'network', ssidIn), open ? null : h('label', null, 'password', pass),
      h('div', { class: 'row' }, h('span', { class: 'dim small' }, 'molty tries for up to 15 s'), go));
    f.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      go.disabled = true; go.textContent = 'connecting ⠴';
      mount(msg);
      try {
        const r = await api.wifiConnect(ssidIn.value.trim(), open ? '' : pass.value);
        mount(msg, note('ok', `connected ⠶ ${r.ip ?? ''} — molty saved this network.`));
        mount(form);
        poll();
      } catch (e) {
        mount(msg, note('err', errText(e) === 'molty said: connect_failed' ? 'couldn\'t join — check the password and that it\'s 2.4 GHz.' : errText(e)));
      } finally { go.disabled = false; go.textContent = 'connect'; }
    });
    mount(form, f);
    (open ? go : pass).focus();
  };

  async function forget() {
    try { await api.wifiForget(); mount(msg, note('warn', 'forgotten. molty is rebooting into setup mode (≈10 s).')); }
    catch (e) { mount(msg, note('err', errText(e))); }
  }

  mount(list, h('li', { class: 'dim' }, 'press scan to look for networks'));
  mount(form, h('button', { class: 'ghost small', type: 'button', onclick: () => pick('', false) }, 'enter a hidden network'));
  renderNow(); poll();
  return () => { alive = false; clearTimeout(timer); };
}
