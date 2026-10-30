// What the setup flow should show, given a GET /api/link poll. Pure, so it is tested
// without a device. Contract (fields may be absent on older firmware):
//   pair:      null | {agent_name, code, expires_in_s}   — a request waiting for a tap
//   last_pair: null | {seq, agent_name, ok, reason}      — seq increments per decision
// `seen` is the last_pair.seq already accounted for (taken when the agent step opened),
// so an old decision never reads as a new one.
import type { LinkInfo } from '../api/types';

export type PairView =
  | { kind: 'wait' }
  | { kind: 'tap'; agent: string; code: string }
  | { kind: 'done'; agent: string }
  | { kind: 'failed'; agent: string; reason: string };

/** The seq to treat as "already seen" when the agent step opens. */
export const pairBaseline = (l: LinkInfo | null) => l?.last_pair?.seq ?? -1;

export function pairView(l: LinkInfo, seen: number): { view: PairView; seen: number } {
  const lp = l.last_pair, fresh = !!lp && typeof lp.seq === 'number' && lp.seq > seen;
  if (fresh && lp!.ok) return { view: { kind: 'done', agent: lp!.agent_name || 'your agent' }, seen: lp!.seq };
  if (l.pair && l.pair.code) return { view: { kind: 'tap', agent: l.pair.agent_name || 'An agent', code: String(l.pair.code) }, seen: fresh ? lp!.seq : seen };
  if (fresh) return { view: { kind: 'failed', agent: lp!.agent_name || 'your agent', reason: lp!.reason || 'not accepted' }, seen: lp!.seq };
  return { view: { kind: 'wait' }, seen };
}
