// One-shot ACTIONS: short gestures played over whatever mood the creature is in (a nod, a
// glance, a yawn). Pure JS (no three.js). The device twin is
// device/firmware/main/creature/cute_act.h: the same names, durations and curves, sample for
// sample — device/creatures/tools/act-golden.mjs writes the samples to
// device/firmware/test/host/act_golden.txt and the firmware's host test compares its own.
//
// An action is a pose OFFSET (same fields as mood.js zeroPose, minus eye/emote) as a
// function of its own time t in 0..dur: zero at both ends. MoodEngine.act(name) plays one:
// the offsets are added after the springs, look before the saccade clamp, lid after the
// blink (as max), mouth as max.

const TAU = Math.PI * 2;

/** name → duration (s), in the device's enum order. */
export const ACTIONS = {
  wiggle: 1.4, nod: 1.2, shake_head: 1.3, bounce: 1.2, peek: 2.2,
  glance_left: 1.4, glance_right: 1.4, blink: 0.35, wobble: 1.6, stretch: 1.8, look_up: 1.6, yawn: 2.6,
  inspect: 4.8, paddle: 3.6,
};
export const ACTION_NAMES = Object.keys(ACTIONS);

const ss = (x) => { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); };
const env = (t, d, a, r) => ss(t / a) * ss((d - t) / r);
const bump = (t, a, b) => (t < a || t > b ? 0 : Math.sin(Math.PI * (t - a) / (b - a)));

export function zeroAction() {
  return { pos: { x: 0, y: 0, z: 0 }, rot: { x: 0, y: 0, z: 0 }, head: { x: 0, y: 0, z: 0 }, spine: { x: 0, y: 0, z: 0 }, squash: 0, lid: 0, look: { x: 0, y: 0 }, mouth: 0, zoom: 0, focus: 0 };
}

/** The pose offset of action `name` at its time t; all zero outside 0..duration. */
export function actionPose(name, t) {
  const o = zeroAction();
  const d = ACTIONS[name];
  if (d === undefined || t < 0 || t > d) return o;
  switch (name) {
    case 'wiggle': {
      const e = env(t, d, 0.12, 0.3), s = Math.sin(TAU * 3.2 * t);
      o.rot.z = 0.14 * e * s;
      o.squash = 0.05 * e * Math.sin(TAU * 6.4 * t);
      o.head.z = 0.1 * e * Math.sin(TAU * 3.2 * t + 1.4);
      break;
    }
    case 'nod': {
      const e = env(t, d, 0.1, 0.25), n = Math.max(0, Math.sin(TAU * 1.8 * t));
      o.head.x = 0.26 * e * n;
      o.spine.x = 0.04 * e * n;
      o.look.y = -0.2 * e * n;
      break;
    }
    case 'shake_head': {
      const e = env(t, d, 0.1, 0.3), s = Math.sin(TAU * 2.2 * t);
      o.head.y = 0.34 * e * s;
      o.look.x = 0.4 * e * s;
      break;
    }
    case 'bounce': {
      const q = (t % 0.6) / 0.6, hop = Math.sin(Math.PI * q), k = t < 0.6 ? 1 : 0.65;
      const e = ss(t / 0.08) * ss((d - t) / 0.15);
      o.pos.y = 0.14 * k * hop * e;
      o.squash = k * e * (hop < 0.25 ? -0.12 * (1 - hop / 0.25) : 0.06 * hop);
      break;
    }
    case 'peek': {
      const e = env(t, d, 0.3, 0.5), dip = bump(t, 0, 0.9);
      const up = ss((t - 0.6) / 0.3) * e, look = ss((t - 0.7) / 0.2) * e;
      const s = Math.sin(TAU * 0.9 * (t - 0.7));
      o.pos.y = -0.13 * dip + 0.03 * up;
      o.squash = -0.12 * dip;
      o.look.x = 0.75 * s * look;
      o.head.y = 0.18 * s * look;
      o.look.y = 0.2 * up;
      break;
    }
    case 'glance_left': case 'glance_right': {
      const e = env(t, d, 0.15, 0.35), side = name === 'glance_left' ? -1 : 1;
      o.look.x = 0.85 * side * e;
      o.head.y = 0.22 * side * e;
      o.head.z = -0.04 * side * e;
      break;
    }
    case 'blink':
      o.lid = t < 0.08 ? t / 0.08 : t < 0.15 ? 1 : t < 0.3 ? 1 - (t - 0.15) / 0.15 : 0;
      break;
    case 'wobble': {
      const k = Math.exp(-2.2 * t) * ss(t / 0.08) * ss((d - t) / 0.3);
      o.rot.z = 0.12 * k * Math.sin(TAU * 2.2 * t);
      o.squash = 0.05 * k * Math.sin(TAU * 2.2 * t + 1.2);
      break;
    }
    case 'stretch': {
      const e = env(t, d, 0.6, 0.6);
      o.squash = 0.14 * e;
      o.head.x = -0.18 * e;
      o.spine.x = -0.08 * e;
      o.lid = 0.35 * e;
      break;
    }
    case 'look_up': {
      const e = env(t, d, 0.25, 0.4);
      o.head.x = -0.32 * e;
      o.look.y = 0.8 * e;
      o.spine.x = -0.05 * e;
      break;
    }
    case 'yawn': {
      const e = env(t, d, 0.5, 0.7), m = env(t, 2.2, 0.5, 0.5);
      o.head.x = -0.26 * e;
      o.spine.x = -0.06 * e;
      o.squash = 0.08 * e;
      o.lid = 0.65 * e;
      o.mouth = t < 2.2 ? 0.85 * m : 0;
      break;
    }
    case 'inspect': { // the firmware's camera follows zoom + focus (cute_rig.c); the authoring stage doesn't
      const e = env(t, d, 0.9, 0.9), s = Math.sin(TAU * 0.55 * (t - 0.9));
      const look = ss((t - 0.9) / 0.4) * e;
      o.zoom = 5.5 * e;
      o.focus = e;
      o.look.x = 0.8 * s * look;
      o.look.y = 0.25 * Math.sin(TAU * 0.3 * (t - 0.9)) * look;
      o.head.y = 0.1 * s * look;
      o.lid = bump(t, 3.2, 3.5);
      break;
    }
    case 'paddle': {
      const e = env(t, d, 0.3, 0.5), b = Math.sin(TAU * 1.6 * t);
      o.pos.y = 0.05 * e * b;
      o.rot.z = 0.1 * e * Math.sin(TAU * 0.8 * t);
      o.spine.x = 0.08 * e;
      o.head.x = 0.1 * e;
      o.look.y = -0.4 * e;
      o.squash = -0.04 * e * b;
      break;
    }
  }
  return o;
}

/** The offsets as one flat row, in cute_act_pose_t's field order (the golden's columns). */
export function actionRow(o) {
  return [o.pos.x, o.pos.y, o.pos.z, o.rot.x, o.rot.y, o.rot.z, o.head.x, o.head.y, o.head.z,
    o.spine.x, o.spine.y, o.spine.z, o.squash, o.lid, o.look.x, o.look.y, o.mouth, o.zoom, o.focus];
}
