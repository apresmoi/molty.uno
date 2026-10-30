// Entry point for the fluid sound + simulated-haptics layer.
//   import('./sound/index.js').then((m) => m.start())   // from a user gesture ("sound on")
//   m.stop()
// start() creates the AudioContext, the engine, the motor simulator, the control panel
// (#sound-panel, optional) and the haptic indicator (#motor, optional), and drives the
// engine's audio frame at ~60 Hz. Everything talks over ../bus.js — see engine.js/motor.js.

import { bus } from '../bus.js';
import { createEngine } from './engine.js';
import { createMotor } from './motor.js';
import { mountPanel } from './ui.js';
import { PRESETS } from './presets.js';

export { createEngine } from './engine.js';
export { createMotor, kickPattern } from './motor.js';

const KEY = 'molty.fluid.sound.v1';
let S = null;

function loadPrefs() {
  let p = {};
  try { p = JSON.parse(localStorage.getItem(KEY) || '{}') || {}; } catch {}
  return {
    volume: typeof p.volume === 'number' ? p.volume : 0.7,
    preset: PRESETS[p.preset] ? p.preset : 'glass',
    layers: p.layers && typeof p.layers === 'object' ? p.layers : {},
    vibrate: !!p.vibrate,
  };
}

export async function start(opts = {}) {
  if (S) { try { await S.ctx.resume(); } catch {} return S.api; }
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) { console.warn('[sound] Web Audio not available'); return null; }
  const prefs = { ...loadPrefs(), ...opts };
  const savePrefs = () => { try { localStorage.setItem(KEY, JSON.stringify(prefs)); } catch {} };
  const ctx = new AC({ latencyHint: 'interactive' });
  const resumed = ctx.resume().catch(() => {}); // inside the gesture
  const engine = createEngine({ ctx, bus, preset: prefs.preset, volume: prefs.volume, layers: prefs.layers });
  const motor = createMotor({ bus, el: document.getElementById('motor'), vibrate: prefs.vibrate });
  const panel = mountPanel(document.getElementById('sound-panel'), { engine, motor, bus, prefs, savePrefs });
  const timer = setInterval(() => engine.tick(), 1000 / 60);
  const api = { ctx, engine, motor, stop, takePattern: () => motor.takePattern() };
  S = { ctx, engine, motor, panel, timer, api };
  await resumed;
  return api;
}

export async function stop() {
  if (!S) return;
  const s = S; S = null;
  clearInterval(s.timer);
  s.panel.dispose(); s.motor.dispose(); s.engine.dispose();
  try { await s.ctx.close(); } catch {}
}

export function running() { return !!S; }
