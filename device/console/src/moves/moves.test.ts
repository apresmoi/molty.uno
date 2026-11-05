import { describe, expect, it } from 'vitest';
import { ACTS, MOODS, creatureMoves, moveLabel } from './moves';

describe('creature gestures and moods', () => {
  it('falls back to the firmware vocabulary when the device lists none', () => {
    expect(creatureMoves(null)).toEqual({ acts: ACTS, moods: MOODS });
    expect(creatureMoves({ creature_info: { name: 'Duck', package: 'duck' } })).toEqual({ acts: ACTS, moods: MOODS });
    expect(creatureMoves({ creature_info: { gestures: [], moods: 'happy' } })).toEqual({ acts: ACTS, moods: MOODS });
  });
  it('uses the package lists the device reports', () => {
    expect(creatureMoves({ creature_info: { gestures: ['nod', 'paddle'], moods: ['happy'] } })).toEqual({ acts: ['nod', 'paddle'], moods: ['happy'] });
  });
  it('shows a package gesture of its own (the cat eats) with the shared ones', () => {
    expect(creatureMoves({ creature_info: { gestures: ['nod', 'eat'] } }).acts).toEqual(['nod', 'eat']);
  });
  it('labels without underscores', () => {
    expect(moveLabel('glance_left')).toBe('glance left');
  });
});
