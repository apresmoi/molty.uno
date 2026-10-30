// Agent (settings): the same single-message design as setup step 3 — one message, one
// copy button, Advanced for ids / relay / CLI / MCP fallback — plus the live link state
// (relay connection, a pending pair request to tap, the last decision) — and the Voice
// card: the owner's own STT/TTS box (ui/voice-card.ts).
// Older firmware without /api/link degrades to a notice.
import { UnsupportedError } from '../api/device';
import type { LinkInfo } from '../api/types';
import type { AppCtx } from '../app';
import { linkPanel } from '../ui/agent-link';
import { voiceCard } from '../ui/voice-card';
import { h, mount, note } from '../ui/dom';
import { errText } from '../ui/fmt';

export function agentPage(root: HTMLElement, { api }: AppCtx) {
  let alive = true, timer = 0, link: LinkInfo | null = null;
  const panel = linkPanel(api, (l) => { link = l; renderState(); voice.update(l); });
  const voice = voiceCard();
  const state = h('div', { class: 'link-state', 'aria-live': 'polite' });
  const msg = h('div');

  mount(root,
    h('h1', null, 'agent ', h('span', { class: 'dim' }, '⠶ who is my brain')),
    h('div', { class: 'agent-page' },
      h('p', { class: 'say' }, 'Send this to your agent. ', h('span', { class: 'dim' }, 'Claude, Grok, OpenClaw… it does the rest.')),
      panel.box, state, msg, panel.adv, voice.el));

  function renderState() {
    const l = link;
    if (!l) { mount(state, h('span', { class: 'dim' }, 'asking molty…')); return; }
    const lp = l.last_pair;
    mount(state,
      h('span', { class: l.connected ? 'badge ok' : 'badge warn' }, l.connected ? '⠶ relay connected' : l.auth_failed ? '⠂ relay refused molty' : '⠂ relay offline'),
      l.pair ? h('span', { class: 'badge warn' }, `tap molty to accept ${l.pair.agent_name} (${l.pair.code})`) : null,
      !l.pair && lp ? h('span', { class: 'badge' + (lp.ok ? ' ok' : '') }, lp.ok ? `${lp.agent_name} paired` : `${lp.agent_name}: ${lp.reason || 'not accepted'}`) : null);
  }

  async function poll() {
    try { link = await api.link(); panel.update(link); voice.update(link); renderState(); mount(msg); }
    catch (e) {
      if (e instanceof UnsupportedError) {
        mount(state); mount(msg, note('warn', 'this firmware doesn\'t support the agent link yet — update molty, then come back.'));
      } else mount(msg, note('err', errText(e)));
    }
    if (alive) timer = window.setTimeout(poll, 2000);
  }
  renderState(); poll();
  return () => { alive = false; clearTimeout(timer); voice.stop(); };
}
