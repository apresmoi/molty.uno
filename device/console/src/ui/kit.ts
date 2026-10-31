// The console's controls, one look everywhere: a section with its question, big choice cards,
// small pills, a labelled row, a switch and a closed "more". Setup and settings share them.
import { h } from './dom';

type Kid = Parameters<typeof h>[2];
export interface Choice<T extends string = string> { id: T; label: string; hint?: string; sw?: string; /** Not set up here: shown dim, and what it needs. */ need?: string }

/** What a page is for, in one line. */
export const pageHead = (title: string, why: string) => [h('h1', null, title), h('p', { class: 'lede' }, why)];

export const section = (title: string, why: string | null, ...kids: Kid[]) =>
  h('section', { class: 'sec' }, h('h2', null, title), why ? h('p', { class: 'why' }, why) : null, ...kids);

/** Runs a pick once, with the pressed control marked until it lands. */
const picking = (pick: () => unknown) => async (ev: Event) => {
  const b = ev.currentTarget as HTMLElement;
  if (b.getAttribute('aria-busy')) return;
  b.setAttribute('aria-busy', 'true');
  try { await pick(); } finally { b.removeAttribute('aria-busy'); }
};

/** Big answers, one per line, each saying what it means. */
export const choices = <T extends string>(name: string, options: ReadonlyArray<Choice<T>>, on: T | undefined, pick: (id: T) => unknown) =>
  h('div', { class: 'list', role: 'group', 'aria-label': name }, options.map((o) => h('button', {
    type: 'button', class: 'opt choice' + (o.id === on ? ' on' : ''), 'aria-pressed': String(o.id === on), onclick: picking(() => pick(o.id)),
  }, h('span', { class: 'opt-label' }, o.label), o.hint ? h('span', { class: 'meta' }, o.hint) : null)));

/** Small answers that fit on a line or two. */
export const pills = <T extends string>(name: string, options: ReadonlyArray<Choice<T>>, on: T | undefined, pick: (id: T) => unknown) =>
  h('div', { class: 'chips', role: 'group', 'aria-label': name }, options.map((o) => h('button', {
    type: 'button', class: 'chip' + (o.id === on ? ' on' : '') + (o.need ? ' dim' : ''), 'aria-pressed': String(o.id === on), style: o.sw ? `--sw:${o.sw}` : undefined, title: o.hint, onclick: picking(() => pick(o.id)),
  }, o.sw ? h('span', { class: 'swatch' }) : null, o.label)));

/** "Label, what it means" on the left, its control on the right. */
export const field = (label: string, hint: string | null, control: Kid) =>
  h('div', { class: 'field' }, h('div', { class: 'field-text' }, h('div', { class: 'field-label' }, label), hint ? h('div', { class: 'why' }, hint) : null), control);

export const toggle = (label: string, hint: string | null, on: boolean, set: (on: boolean) => unknown) =>
  field(label, hint, h('button', { type: 'button', class: 'switch' + (on ? ' on' : ''), role: 'switch', 'aria-checked': String(on), 'aria-label': label, onclick: picking(() => set(!on)) }, h('span')));

export const dropdown = <T extends string>(label: string, options: ReadonlyArray<Choice<T>>, on: T | undefined, pick: (id: T) => unknown) =>
  h('select', { 'aria-label': label, onchange: (ev: Event) => pick((ev.target as HTMLSelectElement).value as T) },
    options.map((o) => h('option', { value: o.id, selected: o.id === on }, o.label)));

/** Secondary detail, closed until asked for. */
export const more = (label: string, ...kids: Kid[]) => h('details', { class: 'more' }, h('summary', null, label), h('div', { class: 'more-body' }, ...kids));

/** A status line: name, value, and where to change it. */
export const statusRow = (label: string, value: string, kind: 'ok' | 'warn' | 'dim', href?: string) =>
  h(href ? 'a' : 'div', { class: 'srow', href }, h('span', { class: 'srow-k' }, label), h('span', { class: `srow-v ${kind}` }, value), href ? h('span', { class: 'srow-go', 'aria-hidden': 'true' }, '›') : null);
