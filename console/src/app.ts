// Shell + router. First run lands in the setup flow (#/setup/<step>, full screen, no
// chrome); afterwards the settings tabs (#/<page>). Hash routing keeps it one static file.
import { DeviceApi } from './api/device';
import { defaultCatalog, type CreatureCatalog } from './catalog/catalog';
import { animateBraille } from './ui/braille';
import { h, mount, type Page } from './ui/dom';
import { statusPage } from './pages/status';
import { wifiPage } from './pages/wifi';
import { creaturePage } from './pages/creature';
import { agentPage } from './pages/agent';
import { diagPage } from './pages/diag';
import { licencesPage } from './pages/licences';
import { setupPage, SETUP_KEY } from './pages/setup';

export interface AppCtx {
  api: DeviceApi;
  catalog: CreatureCatalog;
}

const PAGES: Array<{ id: string; label: string; page: Page }> = [
  { id: 'status', label: 'home', page: statusPage },
  { id: 'wifi', label: 'wi-fi', page: wifiPage },
  { id: 'molty', label: 'molty', page: creaturePage },
  { id: 'agent', label: 'agent', page: agentPage },
  { id: 'diag', label: 'diag', page: diagPage },
];
const ALIAS: Record<string, string> = { creature: 'molty' };
const LICENCES = { id: 'licences', label: 'licences', page: licencesPage };   // footer link, not a tab

const store = {
  get(k: string) { try { return localStorage.getItem(k); } catch { return null; } },
};

export function start(root: HTMLElement) {
  const api = new DeviceApi();
  const catalog = defaultCatalog();
  const nav = h('nav', { 'aria-label': 'pages' });
  const main = h('main');
  const field = h('pre', { class: 'field', 'aria-hidden': 'true' });
  const target = api.base ? h('span', { class: 'target', title: 'device override (?device=)' }, '→ ', api.label) : null;
  const shell = h('div', { class: 'shell' },
    h('header', null,
      h('a', { class: 'brand', href: '#/status' }, h('span', { class: 'mark' }, '⣿'), ' molty'),
      target, nav),
    field, main,
    h('footer', null, 'molty console ⠂ ', h('a', { href: '#/setup/wifi' }, 'run setup again'), ' ⠂ ', h('a', { href: '#/licences' }, 'licences')));
  const setup = h('div', { class: 'setup-root' });
  mount(root, shell, setup);
  animateBraille(field, 64, 3, 0.42);

  let cleanup: (() => void) | void;
  const route = () => {
    const hash = location.hash.replace(/^#\/?/, '');
    if (!hash) { location.replace(store.get(SETUP_KEY) === 'done' ? '#/status' : '#/setup/wifi'); return; }
    const [a, b] = hash.split('/');
    cleanup?.();
    window.scrollTo(0, 0);
    const ctx: AppCtx = { api, catalog };
    if (a === 'setup') {
      shell.hidden = true; setup.hidden = false;
      document.body.classList.add('in-setup');
      const page = h('section', { class: 'page', 'data-page': 'setup' });
      mount(setup, page);
      mount(main);
      cleanup = setupPage(page, ctx, b || 'wifi');
      return;
    }
    shell.hidden = false; setup.hidden = true;
    document.body.classList.remove('in-setup');
    mount(setup);
    const id = ALIAS[a] ?? a;
    const entry = (id === 'licences' ? LICENCES : PAGES.find((p) => p.id === id)) ?? PAGES[0];
    mount(nav, PAGES.map((p) => h('a', { href: `#/${p.id}`, class: p.id === entry.id ? 'here' : '', 'aria-current': p.id === entry.id ? 'page' : undefined }, p.label)));
    const page = h('section', { class: 'page', 'data-page': entry.id });
    mount(main, page);
    cleanup = entry.page(page, ctx);
  };
  addEventListener('hashchange', route);
  route();
}
