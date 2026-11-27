// SPDX-License-Identifier: MIT
// chords.mjs: the chord kinds (data/chords.json) and the twelve notes (data/notes.json), and wave.play's `chords` as
// Molty takes them ({id, label, semis, ratio, spell}, docs/AGENT-LINK.md "Waves").
import { readFileSync } from 'node:fs';

const DATA = new URL('../data/', import.meta.url);
export const NOTES = JSON.parse(readFileSync(new URL('notes.json', DATA))).notes;
export const CHORDS = JSON.parse(readFileSync(new URL('chords.json', DATA))).chords;

/** The kinds as the wheel holds them: their names in capitals ("ALONE", "MAJOR SEVENTH"). */
export const wire = () => CHORDS.map((c) => ({ id: c.id, label: c.name.toUpperCase(), semis: c.semis, ratio: c.ratio, spell: c.spell_in }));
