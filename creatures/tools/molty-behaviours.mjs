// molty-behaviours.mjs — declarative re-expressions of cute-kit creatures' JS hooks
// (build/update), for molty-export.mjs. Hooks cannot ship as code to the device; each
// entry here is a list of behaviours in the vocabulary of creatures/MOLTY-FORMAT.md:
//
//   { target: { kind: 'part'|'socket'|'root'|'head'|'eyes'|'body', name? },
//     channel: 'rx'|'ry'|'rz'|'px'|'py'|'pz'|'s'|'sx'|'sy'|'sz'|'sxz'|'lid',
//     op: 'add'|'set',
//     terms: [{ driver: 'const'|'sin'|'abssin'|'possin'|'square'|'mouth'|'lagx'|'lagz'|'pulse'|'spin',
//               amp, freq, phase, gain: { k, <mood>: coef }, gainB?, ease?, sideSign?, sidePhase?, p0?, p1? }] }
//
// value = Σ amp · gain(w) · gainB(w) · driver(t), gain(w) = k + Σ coef_mood · w_mood (k defaults
// to 0 when gain is given, 1 when omitted). Keep each entry next to a comment quoting what the
// JS hook does, so a reviewer can check the translation line by line.

export const BEHAVIOURS = {
  // pack-a.js Kitten: tail swish, slow blinks, bottlebrush tail when startled, kneading paws.
  'cute-kitten': [
    // creature.root.rotation.z += 0.03 * sin(TAU*0.45*t) * (1 - w.sleepy)
    { target: { kind: 'root' }, channel: 'rz', op: 'add',
      terms: [{ driver: 'sin', amp: 0.03, freq: 0.45, gain: { k: 1, sleepy: -1 } }] },
    // creature.root.position.y += 0.006 * |sin(TAU*1.4*t)| * w.happy
    { target: { kind: 'root' }, channel: 'py', op: 'add',
      terms: [{ driver: 'abssin', amp: 0.006, freq: 1.4, gain: { happy: 1 } }] },
    // tail.rotation.set(base.x - 0.55 sleepy + 0.35 surprised + 0.15 happy, base.y, base.z + swish)
    { target: { kind: 'socket', name: 'tail' }, channel: 'rx', op: 'set',
      terms: [{ driver: 'const', amp: 1, gain: { sleepy: -0.55, surprised: 0.35, happy: 0.15 } }] },
    { target: { kind: 'socket', name: 'tail' }, channel: 'ry', op: 'set', terms: [] },   // base.y: the kit wag is replaced
    // swish = 0.38 (1-happy)(1-0.7 sleepy) sin(TAU*0.5*t) + 0.08 happy sin(TAU*6*t)
    { target: { kind: 'socket', name: 'tail' }, channel: 'rz', op: 'set',
      terms: [{ driver: 'sin', amp: 0.38, freq: 0.5, gain: { k: 1, happy: -1 }, gainB: { k: 1, sleepy: -0.7 } },
              { driver: 'sin', amp: 0.08, freq: 6, gain: { happy: 1 } }] },
    // s.puff = ease(s.puff, w.surprised, dt, 10); tail + tip scale (1+1.1 puff, 1+0.15 puff, 1+1.1 puff)
    ...['tail', 'tailTip'].flatMap((name) => [
      { target: { kind: 'part', name }, channel: 'sxz', op: 'set', terms: [{ driver: 'const', amp: 1.1, gain: { surprised: 1 }, ease: 10 }] },
      { target: { kind: 'part', name }, channel: 'sy', op: 'set', terms: [{ driver: 'const', amp: 0.15, gain: { surprised: 1 }, ease: 10 }] },
    ]),
    // kneading: k = happy * max(0, sin(TAU*1.6*t + i*PI)); foot.y += 0.03 k; foot.rx -= 0.4 k
    { target: { kind: 'socket', name: 'foot' }, channel: 'py', op: 'add',
      terms: [{ driver: 'possin', amp: 0.03, freq: 1.6, gain: { happy: 1 }, sidePhase: true }] },
    { target: { kind: 'socket', name: 'foot' }, channel: 'rx', op: 'add',
      terms: [{ driver: 'possin', amp: -0.4, freq: 1.6, gain: { happy: 1 }, sidePhase: true }] },
    // whisker.rotation.z = baseRot.z + 0.12 * speaking * mouth * side
    { target: { kind: 'part', name: 'whisker' }, channel: 'rz', op: 'set',
      terms: [{ driver: 'mouth', amp: 0.12, gain: { speaking: 1 }, sideSign: true }] },
    // slow "I trust you" blink every 4–7 s in calm moods: lids close softly over 1.6 s
    { target: { kind: 'eyes' }, channel: 'lid', op: 'set',
      terms: [{ driver: 'pulse', amp: 1.6, freq: 1.6, p0: 4, p1: 7, gain: { idle: 1, listening: 1 } }] },
  ],
};
