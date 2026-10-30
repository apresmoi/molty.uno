// Which glass beats a Molty plays: its signature beats (beats/<creature-id>.ts, picked up
// automatically) and the generic ones. Adding a creature: add beats/<catalog-id>.ts exporting
// `beats`; the file name is the catalog id.
import type { Beat } from "./api";
import { GENERIC } from "./generic";

const FILES = import.meta.glob<readonly Beat[]>("./cute-*.ts", { eager: true, import: "beats" });

/** Signature beats by catalog id. */
export const SIGNATURES: Readonly<Record<string, readonly Beat[]>> = Object.fromEntries(
  Object.entries(FILES).map(([path, beats]) => [path.slice(2, -3), beats]),
);

const hash = (str: string) => {
  let h = 2166136261;
  for (let i = 0; i < str.length; i++) h = Math.imul(h ^ str.charCodeAt(i), 16777619);
  return h >>> 0;
};

/** Every beat a Molty can play: its signatures first, then the generic ones. */
export function beatsOf(id: string): Beat[] {
  return [...(SIGNATURES[id] ?? []), ...GENERIC];
}

/** The `k`-th beat a Molty plays in a slot: with signatures, they take the even turns (the
 *  first play included) and generic ones the odd turns; without, generic only. The generic
 *  order starts and strides per Molty and slot (GENERIC.length is prime, so all come round). */
export function pickBeat(id: string, slot: number, k: number): Beat {
  const sig = SIGNATURES[id] ?? [];
  if (sig.length && k % 2 === 0) return sig[(k / 2) % sig.length]!;
  const n = sig.length ? Math.floor(k / 2) : k,
    h = hash(`${id}|${slot}`),
    stride = 1 + ((h >>> 5) % (GENERIC.length - 1));
  return GENERIC[(h + n * stride) % GENERIC.length]!;
}

/** A seed for one play's `b.rand`. */
export const seedOf = (id: string, slot: number, k: number) => hash(`${id}|${slot}|${k}`);
