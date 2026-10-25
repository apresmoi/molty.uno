// Ten-line hyperscript: enough DOM for a console, no framework.
type Child = Node | string | number | null | undefined | false | Child[];
type Props = Record<string, unknown> & { class?: string };

export function h<K extends keyof HTMLElementTagNameMap>(tag: K, props: Props | null = null, ...kids: Child[]): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props ?? {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v as EventListener);
    else if (k === 'class') el.className = String(v);
    else if (k in el && typeof v !== 'string') (el as any)[k] = v;
    else el.setAttribute(k, v === true ? '' : String(v));
  }
  append(el, kids);
  return el;
}

function append(el: Node, kids: Child[]) {
  for (const c of kids) {
    if (c === null || c === undefined || c === false) continue;
    if (Array.isArray(c)) append(el, c);
    else el.appendChild(typeof c === 'object' ? c : document.createTextNode(String(c)));
  }
}

export function mount(el: Element, ...kids: Child[]) { el.replaceChildren(); append(el, kids); }

/** A page: render() builds DOM once; the cleanup it returns stops timers when leaving. */
export type Page = (root: HTMLElement, ctx: import('../app').AppCtx) => (() => void) | void;

/** Clipboard write; on an insecure origin (http://device) falls back to a hidden textarea. */
export async function copyText(t: string) {
  try { await navigator.clipboard.writeText(t); }
  catch {
    const ta = h('textarea', { style: 'position:fixed;opacity:0' }); ta.value = t;
    document.body.appendChild(ta); ta.select(); document.execCommand('copy'); ta.remove();
  }
}

export function copyButton(text: () => string, label = 'copy', done = 'copied ⠿', cls = 'ghost small') {
  const b = h('button', { class: cls, type: 'button' }, label);
  b.addEventListener('click', async () => {
    await copyText(text());
    b.textContent = done; b.classList.add('done');
    setTimeout(() => { b.textContent = label; b.classList.remove('done'); }, 1600);
  });
  return b;
}

export function note(kind: 'info' | 'warn' | 'err' | 'ok', ...kids: Child[]) {
  return h('div', { class: `note ${kind}`, role: kind === 'err' ? 'alert' : 'status' }, ...kids);
}

/** Two-step confirm inline (no window.confirm): first click arms, second click fires. */
export function confirmButton(label: string, armed: string, onFire: () => void, cls = 'danger') {
  const b = h('button', { class: cls, type: 'button' }, label);
  let t = 0;
  b.addEventListener('click', () => {
    if (b.dataset.armed) { clearTimeout(t); delete b.dataset.armed; b.textContent = label; onFire(); return; }
    b.dataset.armed = '1'; b.textContent = armed;
    t = window.setTimeout(() => { delete b.dataset.armed; b.textContent = label; }, 4000);
  });
  return b;
}
