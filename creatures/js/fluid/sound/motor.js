// Simulated ERM haptics (3 V coin/cylinder motor, ~11000 rpm).
//
// Consumes 'audio' {level, beat, t} and 'pulse' {strength, t}; emits 'haptic' {level, duty, t}.
//   duty  = the PWM command we would write to the motor pin this audio frame (0..1).
//   level = the simulated rotor speed (0..1) — what the hand actually feels. It follows
//           duty through first-order inertia (spin-up tau ~35 ms, spin-down ~55 ms) and an
//           ERM stall dead-zone (below ~0.15 duty the rotor does not start).
//
// Drive law (same code is meant to run on the device):
//   base  = (audio level above a dead-zone)^1.5 · 0.42 → a soft "purr" that follows sound
//   kick  = on every 'pulse': duty = min(1, 1.4·k) for the first 15 ms (overdrive to beat the
//           rotor's inertia — what haptic driver ICs call "overdrive"), then k = 0.35+0.65·strength
//           for 40–70 ms, then release to base (coast).
//   duty  = max(base, kick)
//
// PWM pattern format (for the ESP32 later): an array of [durationMs, duty] steps, played in
// order; duty is a 0..1 fraction of the LEDC PWM period (suggest 20 kHz, inaudible, through a
// logic-level MOSFET + flyback diode); 0 = coast. Example "think accent" kick:
//   [[15, 1.0], [45, 0.68], [80, 0]]
// motor.takePattern() returns the duty stream recorded since the last call in this format
// (run-length encoded at audio-frame resolution), and kickPattern(strength) builds one kick.

const clamp = (v, a = 0, b = 1) => (v < a ? a : v > b ? b : v);

export function kickPattern(strength = 0.5) {
  const k = 0.35 + 0.65 * clamp(strength);
  return [[15, Math.min(1, 1.4 * k)], [Math.round(40 + 30 * strength) - 15, +k.toFixed(2)], [80, 0]];
}

export function createMotor({
  bus, el = null, vibrate = false, tauUp = 0.035, tauDown = 0.055, stall = 0.15, deadZone = 0.12,
} = {}) {
  let speed = 0, lastT = null, kickT0 = -1, kickUntil = -1, kickK = 0;
  let rec = [], recDuty = -1, recT = null, lastVib = -1;
  const orig = el ? { transform: el.style.transform, boxShadow: el.style.boxShadow } : null;
  const canVibrate = typeof navigator !== 'undefined' && typeof navigator.vibrate === 'function';
  const state = { vibrate: !!vibrate };

  const offPulse = bus.on('pulse', (p = {}) => {
    const s = clamp(p.strength ?? 0.5), t = p.t ?? lastT ?? 0;
    const k = 0.35 + 0.65 * s;
    if (t >= kickUntil) { kickT0 = t; kickK = k; } else kickK = Math.max(kickK, k);
    kickUntil = Math.max(kickUntil, t + 0.04 + 0.03 * s);
    if (state.vibrate && canVibrate && s >= 0.3 && t - lastVib > 0.08) {
      lastVib = t;
      try { navigator.vibrate(Math.round(15 + 45 * s)); } catch {}
    }
  });

  const offAudio = bus.on('audio', (a = {}) => {
    const t = a.t ?? 0;
    const dt = lastT == null ? 1 / 60 : clamp(t - lastT, 0, 0.05); lastT = t;
    const lv = clamp(a.level ?? 0);
    let duty = lv > deadZone ? Math.pow((lv - deadZone) / (1 - deadZone), 1.5) * 0.42 : 0;
    if (a.beat) duty = Math.max(duty, 0.25 * lv + 0.12);
    if (t < kickUntil) duty = Math.max(duty, t - kickT0 < 0.015 ? Math.min(1, 1.4 * kickK) : kickK);
    else kickK = 0;
    duty = clamp(duty);
    const drive = duty < stall && speed < 0.05 ? 0 : duty; // stalled rotor won't start
    const tau = drive > speed ? tauUp : tauDown;
    if (dt > 0) speed += (drive - speed) * (1 - Math.exp(-dt / tau));
    if (speed < 1e-4) speed = 0;

    // record PWM duty stream, run-length encoded
    const q = Math.round(duty * 20) / 20;
    if (recT == null) { recT = t; recDuty = q; }
    else if (q !== recDuty) { rec.push([Math.round((t - recT) * 1000), recDuty]); recT = t; recDuty = q; }
    if (rec.length > 4000) rec.splice(0, rec.length - 4000);

    if (el) {
      const j = speed * 2.2;
      el.style.transform = `translate(${((Math.random() * 2 - 1) * j).toFixed(2)}px,${((Math.random() * 2 - 1) * j).toFixed(2)}px) scale(${(1 + 0.12 * speed).toFixed(3)})`;
      el.style.boxShadow = `0 0 ${(4 + 22 * speed).toFixed(1)}px ${(1 + 7 * speed).toFixed(1)}px rgba(255,190,130,${(0.12 + 0.6 * speed).toFixed(2)})`;
      el.style.setProperty('--haptic', speed.toFixed(3));
    }
    bus.emit('haptic', { level: speed, duty, t });
  });

  return {
    get level() { return speed; },
    setVibrate(on) { state.vibrate = !!on; },
    canVibrate,
    takePattern() {
      const out = rec; rec = [];
      if (recT != null && lastT != null && lastT > recT) { out.push([Math.round((lastT - recT) * 1000), recDuty]); recT = lastT; }
      return out;
    },
    dispose() {
      offPulse(); offAudio();
      if (el && orig) { el.style.transform = orig.transform; el.style.boxShadow = orig.boxShadow; el.style.removeProperty('--haptic'); }
    },
  };
}
