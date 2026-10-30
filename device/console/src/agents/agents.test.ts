import { describe, expect, it } from 'vitest';
import { DEFAULT_RELAY, PLUGIN_INSTALL, SKILL_HOME, agentMessage, mcpUrl, pairCommand } from './agents';
import { pairBaseline, pairView } from './pairing';
import type { LinkInfo } from '../api/types';

const c = { relay: 'https://relay.example/', deviceId: 'molty-k7m2w9c4ya', linkCode: 'K7QX2MPA' };
const link = (o: Partial<LinkInfo> = {}): LinkInfo => ({ device_id: c.deviceId, link_code: c.linkCode, relay_url: 'wss://relay.example', connected: true, ...o });

describe('agent message', () => {
  it('is the single short message with the real ids and the skill on GitHub (official relay: not named)', () => {
    expect(SKILL_HOME).toBe('https://github.com/apresmoi/molty.uno/tree/main/skills/molty');
    expect(agentMessage({ ...c, relay: DEFAULT_RELAY + '/' })).toBe(
      "Hi! Please connect yourself to my Molty.\n\nSkill: https://github.com/apresmoi/molty.uno/tree/main/skills/molty\nDevice: molty-k7m2w9c4ya\nCode: K7QX2MPA\n\nI'll tap Molty to accept you.");
  });
  it('names a private relay, and never points at the relay for the skill', () => {
    const m = agentMessage({ ...c, relay: 'https://relay.example/' });
    expect(m).toContain('\nRelay: https://relay.example\n');
    expect(m).not.toContain('skill.md');
  });
  it('install lines use the git marketplace only', () => {
    expect(PLUGIN_INSTALL.claude).toBe('claude plugin marketplace add apresmoi/molty.uno && claude plugin install molty@molty');
    expect(PLUGIN_INSTALL.codex).toBe('codex plugin marketplace add apresmoi/molty.uno && codex plugin add molty@molty');
  });
  it('trims trailing slashes from the relay', () => {
    expect(mcpUrl(c.relay)).toBe('https://relay.example/mcp');
    expect(pairCommand(c)).toContain('--device molty-k7m2w9c4ya --code K7QX2MPA');
  });
});

describe('pairView', () => {
  it('waits when nothing is pending (and when the fields are absent)', () => {
    expect(pairView(link(), -1).view.kind).toBe('wait');
    expect(pairView(link({ pair: null, last_pair: null }), -1).view.kind).toBe('wait');
  });
  it('ignores a decision that predates the agent step', () => {
    const l = link({ last_pair: { seq: 4, agent_name: 'old', ok: true } });
    expect(pairView(l, pairBaseline(l)).view.kind).toBe('wait');
  });
  it('shows the tap step for a pending request', () => {
    expect(pairView(link({ pair: { agent_name: 'Grok', code: '4821', expires_in_s: 60 } }), -1).view)
      .toEqual({ kind: 'tap', agent: 'Grok', code: '4821' });
  });
  it('finishes on a new ok decision, fails on a new rejected one, once', () => {
    const ok = link({ last_pair: { seq: 5, agent_name: 'Grok', ok: true } });
    expect(pairView(ok, 4)).toEqual({ view: { kind: 'done', agent: 'Grok' }, seen: 5 });
    const no = link({ last_pair: { seq: 5, agent_name: 'Grok', ok: false, reason: 'denied' } });
    const r = pairView(no, 4);
    expect(r.view).toEqual({ kind: 'failed', agent: 'Grok', reason: 'denied' });
    expect(pairView(no, r.seen).view.kind).toBe('wait');
  });
  it('a fresh retry request wins over the failure it follows', () => {
    const l = link({ pair: { agent_name: 'Grok', code: '1234' }, last_pair: { seq: 5, agent_name: 'Grok', ok: false, reason: 'expired' } });
    expect(pairView(l, 4)).toEqual({ view: { kind: 'tap', agent: 'Grok', code: '1234' }, seen: 5 });
  });
});
