import { describe, expect, it } from 'vitest';
import { GESTURE_INPUTS, gestureChoice, gesturesSupported } from './gestures';

const input = (id: string) => GESTURE_INPUTS.find((i) => i.id === id)!;

describe('gesture settings', () => {
  it('is shown only when the firmware reports gestures', () => {
    expect(gesturesSupported({ gestures: { tap: 'event' } })).toBe(true);
    expect(gesturesSupported({})).toBe(false);
    expect(gesturesSupported(null)).toBe(false);
  });

  it('reads what the device reports, else the out-of-the-box binding', () => {
    expect(gestureChoice({ tap: 'stop' }, input('tap'))).toBe('stop');
    expect(gestureChoice({}, input('tap'))).toBe('event');
    expect(gestureChoice(undefined, input('double_tap'))).toBe('talk');
    expect(gestureChoice({ tap: 'dance' }, input('tap'))).toBe('event');
  });
});
