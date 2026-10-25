// The one rhythm bus shared by the fluid visuals, the sound engine and the haptic motor.
// Nothing here owns a clock; producers emit, consumers subscribe. The contract:
//
// fluid → everyone
//   'frame'  { t, dt, energy, breath, state, mood }  every animation frame.
//            energy 0..1 = mean surface agitation, breath 0..1 = breathing phase.
//   'state'  { from, to }                             a transition starts (blob/creature/text/effect:<name>).
//   'settle' { state }                                the fluid has reached its target.
//   'ripple' { x, y, strength }                       a ripple was injected (x,y in 0..1, strength 0..1).
//   'word'   { word, orp, ms }                        the speed reader shows a new word for ms.
//   'mood'   { name }                                 creature mood changed.
//
// sound → fluid (+ motor)
//   'audio'  { level, pitch, beat }                   every audio frame: output envelope 0..1, pitch 0..1,
//                                                     beat=true on a rhythmic onset. The fluid MUST let
//                                                     level/pitch modulate the surface (ripple amplitude,
//                                                     vibration frequency) so what you hear is what you see.
//   'pulse'  { strength, x?, y? }                     sound asks the fluid for a ripple/pulse at an onset.
//
// motor (simulated haptics) listens to 'audio' and 'pulse' and emits:
//   'haptic' { level }                                0..1 drive, for the on-screen motor indicator.

const subs = new Map();

export const bus = {
  on(type, fn) {
    if (!subs.has(type)) subs.set(type, new Set());
    subs.get(type).add(fn);
    return () => subs.get(type).delete(fn);
  },
  emit(type, data) {
    const set = subs.get(type);
    if (set) for (const fn of set) fn(data);
  },
};
