// First-run setup, "Molty talks": the round Molty screen on top asks ONE question per
// step in a speech bubble; big answers below; a tiny progress line. No paragraphs.
//
//   wifi ─ pass ─ joining ─▶ molty ─▶ agent ⇄ tap ─▶ voice ─▶ gap ─▶ reflex ─▶ done
//   (text only skips gap and reflex; a relay that can't be reached skips all three)
//                                      (polls GET /api/link ~1 s; see agents/pairing.ts)
//
// The sub-step lives in the hash (#/setup/<step>) via replaceState, so a reload or a
// deep link (#/setup/agent) resumes there without re-mounting on every step.
import { UnsupportedError } from '../api/device';
import type { LinkInfo, WifiNetwork, WifiStatus } from '../api/types';
import { pairBaseline, pairView } from '../agents/pairing';
import type { AppCtx } from '../app';
import type { CreatureEntry } from '../catalog/catalog';
import { FILLER_DEFAULT_MS, FILLER_OFF } from '../filler/filler';
import { linkContext, linkPanel, relayLine } from '../ui/agent-link';
import { h, mount } from '../ui/dom';
import { errText } from '../ui/fmt';
import { moltyScreen } from '../ui/screen';
import { gapOptions, getVoice, reflexOptions, setVoice, voiceOptions, type GapFill, type ReflexMode, type SetupOption, type VoiceMode, type VoiceStatus, type VoiceTarget } from '../voice/voice';

type Step = 'wifi' | 'pass' | 'joining' | 'molty' | 'agent' | 'tap' | 'voice' | 'gap' | 'reflex' | 'done';
/** Signal as the mock's block bars (▂▄▆█, _ for missing). */
const bars = (rssi: number) => { const n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1; return '▂▄▆█'.split('').map((c, i) => (i < n ? c : '_')).join(''); };
const STEP_N: Record<Step, number> = { wifi: 0, pass: 0, joining: 0, molty: 1, agent: 2, tap: 2, voice: 3, gap: 3, reflex: 3, done: 4 };
const HASH_STEP: Record<string, Step> = { wifi: 'wifi', molty: 'molty', creature: 'molty', agent: 'agent', tap: 'agent', voice: 'voice', gap: 'voice', reflex: 'voice', done: 'done' };
export const SETUP_KEY = 'molty.setup';
const LOOK_KEY = 'molty.look';
const VOICE_STEPS: Step[] = ['voice', 'gap', 'reflex'];

const store = {
  get(k: string) { try { return localStorage.getItem(k); } catch { return null; } },
  set(k: string, v: string) { try { localStorage.setItem(k, v); } catch { /* private mode */ } },
};

export function setupPage(root: HTMLElement, { api, catalog }: AppCtx, first = 'wifi') {
  let alive = true, step: Step = HASH_STEP[first] ?? 'wifi';
  const screen = moltyScreen(api, 248);
  const bubble = h('div', { class: 'bubble', 'aria-live': 'polite' });
  const body = h('div', { class: 'body' });
  const prog = h('div', { class: 'prog', 'aria-label': 'progress' });
  mount(root, h('div', { class: 'setup' }, screen.el, bubble, body, prog));

  // shared state across steps
  let status: WifiStatus | null = null, nets: WifiNetwork[] | null = null, scanErr = '';
  let net = '', netOpen = false, wifiErr = '', joined = false;
  let list: CreatureEntry[] = [];
  let chosen: CreatureEntry | null = null;
  let agentName = '', pairFail = '', skipped = false;

  const say = (text: string, hint?: string) => mount(bubble, text, hint ? h('small', null, hint) : null);
  const wait = (text: string) => h('div', { class: 'wait' }, h('span', { class: 'spin', 'aria-hidden': 'true' }, '⠴'), text);
  const big = (label: string, onclick: () => void, disabled = false) => h('button', { class: 'big', type: 'button', onclick, disabled }, label);
  const ghost = (label: string, onclick: () => void) => h('button', { class: 'ghost', type: 'button', onclick }, label);

  function go(s: Step) {
    if (!alive) return;
    step = s;
    const tag = s === 'pass' || s === 'joining' ? 'wifi' : s === 'gap' || s === 'reflex' ? 'voice' : s;
    history.replaceState(null, '', `#/setup/${tag}`);
    root.dataset.step = s;
    const n = STEP_N[s];
    prog.textContent = ['Wi-Fi', 'Molty', 'Agent', 'Voice'].map((t, i) => `${i < n ? '✓' : i === n ? '●' : '○'} ${t}`).join('   ');
    if (s === 'tap') { /* screen set by the poller (needs the code) */ }
    else if (chosen && (s === 'molty' || s === 'agent' || s === 'done' || VOICE_STEPS.includes(s))) screen.show(chosen.id);
    else screen.blob();
    if (s === 'agent' || s === 'tap') startPairPoll(); else stopPairPoll();
    if (VOICE_STEPS.includes(s) && !voice) { step = 'voice'; void loadVoice(); }
    render();
  }

  function render() {
    switch (step) {
      case 'wifi': return renderWifi();
      case 'pass': return renderPass();
      case 'joining': say('Connecting…'); return mount(body, wait(`Joining ${net || 'your network'}`));
      case 'molty': return renderMolty();
      case 'agent': return renderAgent();
      case 'tap': return renderTap();
      case 'voice': return renderVoice();
      case 'gap': return renderGap();
      case 'reflex': return renderReflex();
      case 'done': return renderDone();
    }
  }

  // ── 1. Wi-Fi ───────────────────────────────────────────────────────────────
  async function loadWifi() {
    const [st, sc] = await Promise.allSettled([api.wifiStatus(), api.wifiScan()]);
    if (st.status === 'fulfilled') status = st.value;
    if (sc.status === 'fulfilled') {
      const seen = new Set<string>();
      nets = sc.value.filter((n) => n.ssid && !seen.has(n.ssid) && seen.add(n.ssid)).sort((a, b) => b.rssi - a.rssi);
      scanErr = '';
    } else { nets = []; scanErr = errText(sc.reason); }
    if (alive && step === 'wifi') render();
  }
  const rescan = () => { nets = null; render(); loadWifi(); };

  function renderWifi() {
    const on = status?.state === 'connected' ? status.ssid : '';
    if (on) say(`I'm on ${on}. Keep it?`, 'Or pick another');
    else say('Hi! Which Wi-Fi should I use?', '2.4 GHz networks only');
    const pick = (n: WifiNetwork) => { net = n.ssid; netOpen = n.open; wifiErr = ''; go(n.open ? 'joining' : 'pass'); if (n.open) join(''); };
    mount(body,
      on ? h('div', { class: 'list' }, h('button', { class: 'opt on', type: 'button', onclick: () => go('molty') },
        h('span', null, '✓ ', on), h('span', { class: 'meta' }, 'connected'))) : null,
      on ? h('div', { class: 'gap' }) : null,
      nets === null ? wait('Looking for networks…')
        : h('div', { class: 'list', 'aria-label': 'networks' },
            nets.filter((n) => n.ssid !== on).map((n) => h('button', { class: 'opt net', type: 'button', onclick: () => pick(n) },
              h('span', { class: 'ssid' }, n.ssid), h('span', { class: 'meta' }, `${n.open ? '' : '⠿ '}${bars(n.rssi)}`))),
            !nets.length ? h('div', { class: 'wait' }, scanErr || 'No networks found') : null),
      h('div', { class: 'row-ghosts' },
        ghost('Hidden network?', () => { net = ''; netOpen = false; wifiErr = ''; go('pass'); }),
        nets !== null ? ghost('Scan again', rescan) : null));
  }

  function renderPass() {
    if (wifiErr) say('Hmm, that didn’t work.', wifiErr);
    else if (net) say(`What’s the password for ${net}?`);
    else say('Which network?', '2.4 GHz networks only');
    const ssidIn = h('input', { type: 'text', placeholder: 'Network name', 'aria-label': 'network name', autocomplete: 'off', value: net });
    const passIn = h('input', { type: 'password', placeholder: 'Password', 'aria-label': 'password', autocomplete: 'current-password' });
    const f = h('form', { class: 'list' }, net ? null : ssidIn, passIn,
      h('button', { class: 'big', type: 'submit' }, 'Connect'));
    f.addEventListener('submit', (ev) => {
      ev.preventDefault();
      if (!net) net = ssidIn.value.trim();
      if (!net) { ssidIn.focus(); return; }
      go('joining'); join(passIn.value);
    });
    mount(body, f, ghost('← Back', () => { wifiErr = ''; go('wifi'); }));
    setTimeout(() => (net ? passIn : ssidIn).focus());
  }

  async function join(password: string) {
    try {
      const r = await api.wifiConnect(net, netOpen ? '' : password);
      status = { ok: true, state: 'connected', ssid: net, ip: r.ip, rssi: r.rssi };
      joined = true;
      go('molty');
    } catch (e) {
      const t = errText(e);
      wifiErr = t === 'molty said: connect_failed' ? 'Check the password — 2.4 GHz only.' : t;
      go(netOpen ? 'wifi' : 'pass');
      if (netOpen) say('Hmm, that didn’t work.', wifiErr);
    }
  }

  // ── 2. Molty ───────────────────────────────────────────────────────────────
  let sendWant: CreatureEntry | null = null, sending = false, sendNote = '', fluidChecked = false;
  async function send() {
    if (sending) return;
    sending = true;
    while (sendWant) {
      const e = sendWant; sendWant = null;
      sendNote = 'Sending…'; hintMolty();
      try {
        await api.uploadCreature(e.id, await catalog.fetchFile(e));
        await wakeFluid();
        if (!sendWant) sendNote = 'On your Molty now';
      } catch (err) {
        if (!sendWant) sendNote = err instanceof UnsupportedError ? 'My firmware can’t take new looks yet' : `Couldn’t send it (${errText(err)})`;
      }
      hintMolty();
    }
    sending = false;
  }
  /** Make the device show the uploaded Molty: the fluid creature, in its "creature" state. */
  async function wakeFluid() {
    try {
      if (!fluidChecked) {
        fluidChecked = true;
        const reg = await api.creatures();
        if (reg.current !== 'fluid' && reg.available.includes('fluid')) await api.selectCreature('fluid');
      }
      await api.fluid({ state: 'creature' });
    } catch { /* best effort: the upload itself already landed */ }
  }
  function hintMolty() {
    if (step !== 'molty') return;
    say(joined ? 'Connected! Now, who should I be?' : 'Who should I be?', chosen ? sendNote || 'Tap one to bring it to life' : 'Tap one to bring it to life');
  }

  function renderMolty() {
    hintMolty();
    const choose = big(chosen ? `Choose ${chosen.name}` : 'Pick one', () => { if (chosen) { store.set(LOOK_KEY, chosen.id); go('agent'); } }, !chosen);
    const grid = h('div', { class: 'grid', role: 'listbox', 'aria-label': 'Moltys' });
    const tiles = list.map((c) => {
      const t = h('button', { class: 'tile' + (chosen?.id === c.id ? ' on' : ''), type: 'button', role: 'option', 'aria-selected': String(chosen?.id === c.id), 'data-id': c.id },
        c.thumb ? h('img', { src: c.thumb, alt: '', width: 96, height: 96 }) : h('div', { class: 'ph' }, '⠴'),
        h('div', { class: 'n' }, c.name));
      t.addEventListener('click', () => {
        chosen = c;
        for (const x of tiles) { const on = x === t; x.classList.toggle('on', on); x.setAttribute('aria-selected', String(on)); }
        choose.disabled = false; choose.textContent = `Choose ${c.name}`;
        screen.show(c.id);
        sendWant = c; send();
      });
      return t;
    });
    mount(grid, list.length ? tiles : wait('Loading Moltys…'));
    mount(body, grid, h('div', { class: 'foot' }, choose));
  }

  // ── 3. Agent (+ 4. tap) ────────────────────────────────────────────────────
  let link: LinkInfo | null = null, linkUnsupported = false, seen = -1, based = false, pollT = 0, polling = false, tapCode = '';
  const panel = linkPanel(api, (l) => { link = l; });
  const waitLine = h('div', { class: 'wait' });

  function startPairPoll() { if (!polling) { polling = true; based = false; pollLink(); } }
  function stopPairPoll() { polling = false; clearTimeout(pollT); }
  async function pollLink() {
    if (!polling || !alive) return;
    try {
      link = await api.link(); linkUnsupported = false;
      if (!based) { seen = pairBaseline(link); based = true; }
      const r = pairView(link, seen); seen = r.seen;
      const v = r.view;
      if (v.kind === 'done') { agentName = v.agent; go('voice'); return; }
      if (v.kind === 'tap') {
        agentName = v.agent;
        if (step !== 'tap' || tapCode !== v.code) { tapCode = v.code; screen.word(v.code); go('tap'); }
      } else if (v.kind === 'failed') { pairFail = v.reason; agentName = v.agent; go('agent'); }
      else if (step === 'tap') go('agent'); // request expired/withdrawn without a decision
      else if (step === 'agent') { panel.update(link); waitLine.replaceChildren(...wait(relayLine(link)).childNodes); }
    } catch (e) {
      if (e instanceof UnsupportedError) { linkUnsupported = true; if (step === 'agent') render(); }
    }
    if (polling && alive) pollT = window.setTimeout(pollLink, linkUnsupported ? 10000 : 1000);
  }

  function renderAgent() {
    if (linkUnsupported) {
      say('I need a firmware update to get a brain.', 'You can do this later');
      return mount(body, ghost('Skip for now', skip));
    }
    if (pairFail) say(`That didn’t work (${pairFail}). Send the message again?`);
    else say('Who will be my brain?', 'Send this to Claude, Grok, OpenClaw… it does the rest');
    panel.update(link);
    waitLine.replaceChildren(...wait(relayLine(link)).childNodes);
    mount(body, panel.box, waitLine, panel.adv, ghost('Skip for now', skip));
  }
  const skip = () => { skipped = true; go('voice'); };

  function renderTap() {
    pairFail = '';
    say('Tap me if you see this code', `${agentName} wants to connect`);
    mount(body, h('div', { class: 'wait' }, 'Same code on Molty? Tap its screen.'));
  }

  // ── 4. Voice, the gap, reflex: what the owner's tools can do ─────────────────
  let voice: VoiceStatus | null = null, target: VoiceTarget | null = null, saving = false, saveErr = '';

  /** Asks the relay once; one that can't answer (offline, too old) skips these steps. */
  async function loadVoice() {
    try {
      const c = linkContext(link ?? await api.link());
      target = { relay: c.relay, deviceId: c.deviceId, linkCode: c.linkCode };
      voice = await getVoice(target);
      if (alive && step === 'voice') render();
    } catch { if (alive && step === 'voice') go('done'); }
  }

  function chooser<T extends string>(q: string, hint: string, options: SetupOption<T>[], on: T | undefined, pick: (id: T) => Promise<void>) {
    say(q, saveErr ? `Couldn’t save that (${saveErr})` : hint);
    mount(body, h('div', { class: 'list' }, options.map((o) => h('button', {
      class: 'opt choice' + (o.id === on ? ' on' : ''), type: 'button',
      onclick: async () => {
        if (saving) return;
        saving = true; saveErr = '';
        try { await pick(o.id); } catch (e) { saveErr = errText(e); }
        saving = false;
        if (saveErr) render();
      },
    }, h('span', null, o.label), h('span', { class: 'meta' }, o.hint)))));
  }

  function renderVoice() {
    if (!voice) { say('Should I talk?'); return mount(body, wait('Asking the relay…')); }
    const v = voice;
    const on = v.mode === 'auto' ? (v.serving.tts === 'box' ? 'box' : 'cloud') : v.mode;
    chooser<VoiceMode>('Should I talk?', 'You can change this any time', voiceOptions(v), on, async (id) => {
      voice = await setVoice(target!, { mode: id });
      go(id === 'off' ? 'done' : 'gap');
    });
  }

  function renderGap() {
    const on = voice!.first_words?.mode === 'opener' ? 'local' : undefined;
    chooser<GapFill>('Agents can be slow. What should I do meanwhile?', 'Until your agent speaks', gapOptions(voice!), on, async (id) => {
      voice = await setVoice(target!, { first_words: id === 'local' ? 'opener' : 'off' });
      // a delay the owner tuned stays
      if (id === 'off') await api.fluid({ filler_ms: FILLER_OFF });
      else if (id === 'phrases' && !(await api.creatureState()).filler_ms) await api.fluid({ filler_ms: FILLER_DEFAULT_MS });
      go('reflex');
    });
  }

  function renderReflex() {
    chooser<ReflexMode>('Should I react while you talk?', 'Who picks my emotes', reflexOptions(voice!), voice!.reflex?.mode, async (id) => {
      voice = await setVoice(target!, { reflex: id });
      go('done');
    });
  }

  // ── 5. Done ────────────────────────────────────────────────────────────────
  function renderDone() {
    const name = chosen?.name ?? 'Molty';
    if (skipped || !agentName) say(`I’m ${name}!`, 'Give me a brain any time in Agent');
    else say(`I’m ${name}, and ${agentName} is my brain.`, 'Say hi to Molty');
    mount(body, h('div', { class: 'foot' }, big('Open Molty', () => { store.set(SETUP_KEY, 'done'); location.hash = '#/status'; })));
  }

  // ── boot ───────────────────────────────────────────────────────────────────
  catalog.list().then((l) => {
    list = l;
    const prev = store.get(LOOK_KEY);
    chosen ??= l.find((c) => c.id === prev) ?? null;
    if (!alive) return;
    if (chosen && (step === 'molty' || step === 'agent' || step === 'done' || VOICE_STEPS.includes(step))) screen.show(chosen.id);
    if (step === 'molty' || step === 'done') render();
  }, () => { /* empty grid shows "Loading"; bundled catalog can't fail */ });
  go(step);
  if (step === 'wifi') loadWifi();
  return () => { alive = false; stopPairPoll(); screen.stop(); };
}
