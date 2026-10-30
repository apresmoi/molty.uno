// The "give Molty a brain" panel, shared by setup step 3 and the settings Agent page:
// ONE message + ONE big copy button, the two plugin install lines (Claude Code, Codex:
// the skill ships only through the git marketplaces), and a collapsed Advanced (ids,
// editable relay, CLI pair command, MCP fallback). Rendering only — the caller polls GET /api/link and
// feeds update().
import { relayHttp, relayWs, type DeviceApi } from '../api/device';
import type { LinkInfo } from '../api/types';
import { DEFAULT_RELAY, PLUGIN_INSTALL, agentMessage, mcpUrl, pairCommand, type LinkContext } from '../agents/agents';
import { copyButton, copyText, h, mount } from './dom';
import { errText } from './fmt';

export { DEFAULT_RELAY };

export function linkContext(l: LinkInfo | null): LinkContext {
  return { relay: relayHttp(l?.relay_url || '') || DEFAULT_RELAY, deviceId: l?.device_id || 'molty-…', linkCode: l?.link_code || '…' };
}

export interface LinkPanel { box: HTMLElement; adv: HTMLElement; update(l: LinkInfo | null): void }

export function linkPanel(api: DeviceApi, onLink: (l: LinkInfo) => void = () => {}): LinkPanel {
  let link: LinkInfo | null = null;
  const pre = h('pre', { class: 'msg-text', 'aria-label': 'message for your agent' });
  const copy = h('button', { class: 'big', type: 'button' }, 'Copy message');
  copy.addEventListener('click', async () => {
    await copyText(agentMessage(linkContext(link)));
    copy.textContent = 'Copied ✓'; copy.classList.add('done');
  });
  // Claude Code / Codex: install the skill first (once per machine), then paste the message.
  const install = (who: string, line: string) => {
    const code = h('code', { 'aria-label': `${who} install command` }, line);
    return h('div', { class: 'copy' }, code, copyButton(() => line, 'Copy', 'Copied', 'pill'));
  };
  const plugin = h('div', { class: 'plugin' },
    h('p', { class: 'adv-note' }, 'Claude Code or Codex? Install the Molty skill once, then paste the message:'),
    install('Claude Code', PLUGIN_INSTALL.claude),
    install('Codex', PLUGIN_INSTALL.codex));
  const box = h('div', { class: 'msg' }, pre, copy, plugin);

  const ids = h('div', { class: 'box' });
  const relayIn = h('input', { type: 'url', 'aria-label': 'relay url', spellcheck: false, class: 'mini' });
  const save = h('button', { type: 'button', class: 'pill' }, 'Save');
  const saveMsg = h('div', { class: 'adv-msg', role: 'status' });
  const cli = h('code');
  const mcp = h('code');
  const adv = h('details', { class: 'adv' }, h('summary', null, 'Advanced'),
    ids,
    h('div', { class: 'adv-row' }, h('span', { class: 'k' }, 'relay'), relayIn, save), saveMsg,
    h('div', { class: 'copy' }, cli, copyButton(() => cli.textContent || '', 'Copy', 'Copied', 'pill')),
    h('p', { class: 'adv-note' }, 'Agent can’t open links or run code? Add this MCP server:'),
    h('div', { class: 'copy' }, mcp, copyButton(() => mcp.textContent || '', 'Copy', 'Copied', 'pill')));

  save.addEventListener('click', async () => {
    const ws = relayWs(relayIn.value);
    if (!/^wss?:\/\/[^\s/?#@]+$/.test(ws)) { saveMsg.textContent = 'Use an origin like https://relay.example (no path)'; return; }
    save.disabled = true;
    try { const l = await api.setRelay(ws); saveMsg.textContent = 'Saved — Molty is reconnecting'; update(l); onLink(l); }
    catch (e) { saveMsg.textContent = errText(e); }
    finally { save.disabled = false; }
  });

  function update(l: LinkInfo | null) {
    link = l;
    const c = linkContext(l);
    pre.textContent = agentMessage(c);
    mount(ids,
      h('div', null, h('span', { class: 'k' }, 'device'), c.deviceId),
      h('div', null, h('span', { class: 'k' }, 'code'), c.linkCode, h('span', { class: 'dim' }, '  rotates every 15 min')));
    if (document.activeElement !== relayIn) relayIn.value = c.relay;
    cli.textContent = pairCommand(c);
    mcp.textContent = mcpUrl(c.relay);
  }
  update(null);
  return { box, adv, update };
}

/** One short line about the relay connection (the pairing path). */
export function relayLine(l: LinkInfo | null): string {
  if (!l) return 'Asking Molty…';
  if (l.connected) return 'Waiting for your agent…';
  if (l.auth_failed) return 'The relay refused me — check Advanced';
  return 'Reaching the relay…';
}
