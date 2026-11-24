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
  /** Scenes after `after` s, the first played and the rest queued behind it, as a skill sends
   *  them; again every `every` s. */
  | {
      kind: "scenes";
      scenes: Array<{ layout: string; params: Record<string, unknown>; ttl: number }>;
      after: number;
      every: number;
    }
  /** Words, one at a time at `wpm`, after `after` s, and again `rest` s after the last. */
  | { kind: "say"; text: string; wpm: number; after: number; rest: number };

export interface ProgramRun {
  /** Every frame, with the screen's seconds. */
  frame(seconds: number): void;
  /** The question was answered with this option. */
  answer(index: number): void;
}

export function runProgram(p: Program, screen: MoltyScreenHandle): ProgramRun {
  let on = false; // the question stands
  let at = p.after; // when the next thing happens
  let now = 0;
  return {
    frame(seconds) {
      now = seconds;
      if (now < at) return;
      if (p.kind === "say") {
        screen.state({ state: "text", text: p.text, wpm: p.wpm });
        at = now + (p.text.split(/\s+/).length * 60) / p.wpm + p.rest;
        return;
      }
      if (p.kind === "scenes") {
        p.scenes.forEach((s, i) => screen.scene(s.layout, s.params, s.ttl, i > 0));
        at = now + p.every;
        return;
      }
      if (!on) {
        on = screen.ask(p.question);
        at = now + p.wait;
        return;
      }
      screen.askEnd();
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
