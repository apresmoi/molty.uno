// Agent (settings): the same single-message design as setup step 3 — one message, one
// copy button, Advanced for ids / relay / CLI / MCP fallback — plus the live link state
// (relay connection, a pending pair request to tap, the last decision).
// Older firmware without /api/link degrades to a notice.
import { UnsupportedError } from '../api/device';
import type { LinkInfo } from '../api/types';
import type { AppCtx } from '../app';
import { linkPanel } from '../ui/agent-link';
import { h, mount, note } from '../ui/dom';
import { pageHead, section, statusRow } from '../ui/kit';
import { errText } from '../ui/fmt';

export function agentPage(root: HTMLElement, { api }: AppCtx) {
  let alive = true, timer = 0, link: LinkInfo | null = null;
  const panel = linkPanel(api, (l) => { link = l; renderState(); });
  const state = h('div', { class: 'link-state', 'aria-live': 'polite' });
  const msg = h('div');

  mount(root,
    ...pageHead('Agent', 'The AI that thinks and answers through Molty.'),
    state, msg,
    section('Connect an agent', 'Send this message to Claude, Grok, Codex or any agent with skills. It does the rest.',
      panel.box, panel.adv));

  function renderState() {
    const l = link;
    if (!l) { mount(state, h('p', { class: 'why' }, 'Asking Molty…')); return; }
    const lp = l.last_pair;
    mount(state, h('div', { class: 'srows' },
      statusRow('Relay', l.connected ? 'Connected' : l.auth_failed ? 'Refused this Molty' : 'Not connected', l.connected ? 'ok' : 'warn'),
      lp ? statusRow('Last agent', lp.ok ? `${lp.agent_name} is paired` : `${lp.agent_name}: ${lp.reason || 'not accepted'}`, lp.ok ? 'ok' : 'dim') : null),
      l.pair ? note('warn', `Tap Molty to accept ${l.pair.agent_name} (code ${l.pair.code}).`) : null);
  }

  async function poll() {
    try { link = await api.link(); panel.update(link); renderState(); mount(msg); }
    catch (e) {
      if (e instanceof UnsupportedError) {
        mount(state); mount(msg, note('warn', 'this firmware doesn\'t support the agent link yet — update molty, then come back.'));
      } else mount(msg, note('err', errText(e)));
    }
    if (alive) timer = window.setTimeout(poll, 2000);
  }
  renderState(); poll();
  return () => { alive = false; clearTimeout(timer); };
}
