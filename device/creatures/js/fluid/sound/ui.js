// Control panel mounted into #sound-panel: master volume, per-layer toggles, voice preset,
// phone-vibrate toggle and a tiny live meter. Styles are scoped under .snd- and inherit
// the host page's colours (currentColor) so they sit inside whatever the visuals use.

import { LAYERS, LAYER_LABELS } from './engine.js';
import { PRESET_NAMES } from './presets.js';

const CSS = `
.snd-panel{display:flex;flex-direction:column;gap:8px;font:12px/1.3 ui-monospace,Menlo,monospace;color:inherit}
.snd-row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.snd-panel label{display:inline-flex;align-items:center;gap:4px;cursor:pointer;opacity:.9}
.snd-panel input[type=range]{width:120px;accent-color:currentColor}
.snd-panel select{font:inherit;color:inherit;background:transparent;border:1px solid currentColor;border-radius:4px;padding:1px 4px}
.snd-panel select option{color:#000}
.snd-meter{height:4px;flex:1;min-width:60px;border-radius:2px;background:color-mix(in srgb,currentColor 15%,transparent);overflow:hidden}
.snd-meter>i{display:block;height:100%;width:0;background:currentColor;transition:width 40ms linear}
`;

export function mountPanel(root, { engine, motor, bus, prefs, savePrefs }) {
  if (!root) return { dispose() {} };
  const style = document.createElement('style'); style.textContent = CSS;
  const box = document.createElement('div'); box.className = 'snd-panel';
  const el = (tag, props = {}, kids = []) => { const e = Object.assign(document.createElement(tag), props); kids.forEach((k) => e.append(k)); return e; };

  const vol = el('input', { type: 'range', min: 0, max: 1, step: 0.01, value: prefs.volume });
  vol.oninput = () => { engine.setVolume(+vol.value); prefs.volume = +vol.value; savePrefs(); };
  const sel = el('select', {}, PRESET_NAMES.map((n) => el('option', { value: n, textContent: n, selected: n === prefs.preset })));
  sel.onchange = () => { engine.setPreset(sel.value); prefs.preset = sel.value; savePrefs(); };
  const meterBar = el('i'), hapBar = el('i');
  box.append(
    el('div', { className: 'snd-row' }, [el('label', {}, ['vol ', vol]), el('label', {}, ['voice ', sel])]),
    el('div', { className: 'snd-row' }, LAYERS.map((n) => {
      const cb = el('input', { type: 'checkbox', checked: prefs.layers[n] !== false });
      cb.onchange = () => { engine.setLayer(n, cb.checked); prefs.layers[n] = cb.checked; savePrefs(); };
      return el('label', {}, [cb, LAYER_LABELS[n]]);
    })),
    el('div', { className: 'snd-row' }, [
      el('span', { textContent: 'sound' }), el('div', { className: 'snd-meter' }, [meterBar]),
      el('span', { textContent: 'motor' }), el('div', { className: 'snd-meter' }, [hapBar]),
    ]),
  );
  if (motor.canVibrate) {
    const cb = el('input', { type: 'checkbox', checked: !!prefs.vibrate });
    cb.onchange = () => { motor.setVibrate(cb.checked); prefs.vibrate = cb.checked; savePrefs(); };
    box.append(el('div', { className: 'snd-row' }, [el('label', {}, [cb, 'phone vibrate'])]));
  }
  root.append(style, box);
  const offs = [
    bus.on('audio', (a) => { meterBar.style.width = `${(a.level * 100).toFixed(0)}%`; }),
    bus.on('haptic', (h) => { hapBar.style.width = `${(h.level * 100).toFixed(0)}%`; }),
  ];
  return { dispose() { offs.forEach((o) => o()); style.remove(); box.remove(); } };
}
