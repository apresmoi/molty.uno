// The molty.UNO mark and wordmark, and the theme switch: as in the website's header.
import { h } from './dom';

const MARK = '<svg class="mark" viewBox="0 0 10 10" aria-hidden="true"><rect class="l" x="0" y="6" width="4" height="4"/><rect class="r" x="6" y="0" width="4" height="4"/></svg>';

export function brand(href?: string) {
  const el = h(href ? 'a' : 'span', { class: 'brand', href, 'aria-label': 'molty.UNO' }, h('span', null, 'molty.', h('b', null, 'UNO')));
  el.insertAdjacentHTML('afterbegin', MARK);
  return el;
}

/** Names the theme a click switches to. */
export function themeToggle() {
  const root = document.documentElement;
  const label = () => (root.dataset.theme === 'light' ? 'dark' : 'light');
  const el = h('button', { type: 'button', class: 'theme', onclick: () => {
    root.dataset.theme = label();
    try { localStorage.setItem('molty-theme', root.dataset.theme); } catch { /* private mode: this page only */ }
    el.textContent = label();
  } }, label());
  return el;
}
