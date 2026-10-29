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

  // pack-e.js Puppy: big listening head tilt, happy rear wiggle, tongue lolls out + pants.
  'cute-puppy': [
    // creature.headPivot.rotation.z += 0.32 * w.listening
    { target: { kind: 'head' }, channel: 'rz', op: 'add',
      terms: [{ driver: 'const', amp: 0.32, gain: { listening: 1 } }] },
    // creature.root.rotation.z += 0.045 * w.happy * sin(TAU*5*t)
    { target: { kind: 'root' }, channel: 'rz', op: 'add',
      terms: [{ driver: 'sin', amp: 0.045, freq: 5, gain: { happy: 1 } }] },
    // k = ease(k, happy, dt, 8); tongue pos = in + (out - in) k (+ 0.008 happy sin(TAU*3t) on y);
    // scale = 0.3 (1 + (1/0.3 - 1) k). PUP_TONGUE: in [0,-0.13,0.2], out [0,-0.215,0.27]
    { target: { kind: 'part', name: 'tongue' }, channel: 'py', op: 'set',
      terms: [{ driver: 'const', amp: -0.085, gain: { happy: 1 }, ease: 8 },
              { driver: 'sin', amp: 0.008, freq: 3, gain: { happy: 1 } }] },
    { target: { kind: 'part', name: 'tongue' }, channel: 'pz', op: 'set',
      terms: [{ driver: 'const', amp: 0.07, gain: { happy: 1 }, ease: 8 }] },
    { target: { kind: 'part', name: 'tongue' }, channel: 's', op: 'set',
      terms: [{ driver: 'const', amp: 1 / 0.3 - 1, gain: { happy: 1 }, ease: 8 }] },
  ],

  // pack-e.js Fox: slow tail swish; pounce-ready crouch every 5–9 s while idle.
  'cute-fox': [
    // tail.rotation.z = base.z + (0.3 (1 - 0.8 sleepy) + 0.15 happy) sin(TAU*0.55*t) + 0.3 sleepy
    { target: { kind: 'socket', name: 'tail' }, channel: 'rz', op: 'set',
      terms: [{ driver: 'sin', amp: 0.3, freq: 0.55, gain: { k: 1, sleepy: -0.8 } },
              { driver: 'sin', amp: 0.15, freq: 0.55, gain: { happy: 1 } },
              { driver: 'const', amp: 0.3, gain: { sleepy: 1 } }] },
    // the crouch (root.rotation.x += 0.18 crouch, a 1.8 s event every 5–9 s while idle). One
    // channel only: each pulse term has its own random timer, so a second channel would drift
    // out of step. The wiggle and the spring need a gated product (not in v1).
    { target: { kind: 'root' }, channel: 'rx', op: 'add',
      terms: [{ driver: 'pulse', amp: 0.2, freq: 1.8, p0: 5, p1: 9, gain: { idle: 1 } }] },
  ],

  // pack-e.js Turtle: the head tucks into the shell (surprised, half when sleepy) and now and
  // then ducks in and peeks back out.
  'cute-turtle': [
    // k = ease(k, max(surprised, 0.5 sleepy), dt, 7); headPivot pos += (0, -0.04, -0.2) k;
    // headPivot scale *= 1 - 0.6 k  (TURTLE_TUCK). Three eased terms with the same gain stay in step.
    { target: { kind: 'head' }, channel: 'pz', op: 'add',
      terms: [{ driver: 'const', amp: -0.2, gain: { surprised: 1, sleepy: 0.5 }, ease: 7 }] },
    { target: { kind: 'head' }, channel: 'py', op: 'add',
      terms: [{ driver: 'const', amp: -0.04, gain: { surprised: 1, sleepy: 0.5 }, ease: 7 }] },
    { target: { kind: 'head' }, channel: 's', op: 'add',
      terms: [{ driver: 'const', amp: -0.6, gain: { surprised: 1, sleepy: 0.5 }, ease: 7 }] },
    // the peek (every 7–12 s while calm): scale only (one channel, one timer), deep enough
    // that the head hides behind the shell's front at the bottom of the bump
    { target: { kind: 'head' }, channel: 's', op: 'add',
      terms: [{ driver: 'pulse', amp: -0.75, freq: 2.6, p0: 7, p1: 12, gain: { idle: 1, listening: 1 } }] },
  ],
};
