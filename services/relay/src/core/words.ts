// SPDX-License-Identifier: MIT
// Word helpers shared by the partial chunker (core/partials.ts) and the reflex (core/reflex.ts).
// Plain functions only (no TypeScript-only syntax): tools/reflex-bench.ts runs reflex.ts under
// Node's type stripping.

/** Whitespace-split words, as the transcript shows them. */
export function words(text: string): string[] {
  return text.split(/\s+/).filter(Boolean);
}

/** A word for comparison: lowercase, letters / digits / apostrophes only ("Today," = "today"). */
export function normWord(w: string): string {
  return w.toLowerCase().replace(/[’`]/g, "'").replace(/[^\p{L}\p{N}']+/gu, "");
}

/** The text ends a sentence: its last word ends with . ? ! (closing quotes allowed), not an ellipsis. */
export function sentenceEnd(text: string): boolean {
  const t = text.trim();
  if (/(\.\.\.|…)["')\]]*$/.test(t)) return false;
  return /[.?!]["')\]]*$/.test(t);
}
