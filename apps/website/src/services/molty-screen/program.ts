// What a screen does when nobody drives it: one small loop on the screen's own clock (it stops
// while the screen is unseen). Data in, calls on the handle out.
import type { MoltyScreenHandle } from "./screen";

export type Program =
  /** A question comes up after `after` s, waits `wait` s for a tap, and comes back `rest` s
   *  after it went; `then[i]` is the state the cat is sent when option i is tapped. */
  | {
      kind: "ask";
      question: { title: string; detail?: string; code?: string; options: string[] };
      then: Array<Record<string, unknown>>;
      after: number;
      wait: number;
      rest: number;
    }
  /** A scene plays after `after` s for `ttl` s, and again `rest` s after the cat is back. */
  | { kind: "scene"; layout: string; params: Record<string, unknown>; ttl: number; after: number; rest: number };

export interface ProgramRun {
  /** Every frame, with the screen's seconds. */
  frame(seconds: number): void;
  /** The question was answered with this option. */
  answer(index: number): void;
}

export function runProgram(p: Program, screen: MoltyScreenHandle): ProgramRun {
  let on = false; // the question stands / the scene plays
  let at = p.after; // when the next thing happens
  let now = 0;
  return {
    frame(seconds) {
      now = seconds;
      if (now < at) return;
      if (!on) {
        on = p.kind === "ask" ? screen.ask(p.question) : screen.scene(p.layout, p.params, p.ttl);
        at = now + (p.kind === "ask" ? p.wait : p.ttl + 1);
        return;
      }
      if (p.kind === "ask") screen.askEnd();
      on = false;
      at = now + p.rest;
    },
    answer(index) {
      if (p.kind !== "ask" || !on) return;
      on = false;
      at = now + p.rest;
      const then = p.then[index];
      if (then) screen.state(then);
    },
  };
}
