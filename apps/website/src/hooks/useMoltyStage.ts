import { useRef, useState } from "react";
import type { MoltyScreenHandle } from "../services/molty-screen";
import {
  ASK_SECONDS,
  LISTEN_SECONDS,
  MOOD_HOLD,
  STAGE_QUESTION,
  madeUpVoice,
  type MoltyAction,
} from "../features/molty/actions";

/** A gesture is over when the device says none plays; it may wait a little before it starts. */
const ACT_POLL = 0.25;
const ACT_START = 3.5;

/** A stage: one MoltyScreen asked to do things (features/molty/actions), one at a time.
 *  `bind` goes on the MoltyScreen; `play` asks for an action (it replaces the one playing);
 *  `playing` is the one on screen now, read back from the device's own state where it has one
 *  (the gesture playing), else by its length on the screen's clock. */
export function useMoltyStage() {
  const screen = useRef<MoltyScreenHandle | null>(null);
  const run = useRef<{ action: MoltyAction; from: number; until: number; seen: boolean; poll: number } | null>(null);
  const clock = useRef(0);
  const [ready, setReady] = useState(false);
  const [playing, setPlaying] = useState<MoltyAction | null>(null);

  /** Take down what the last action left up. */
  const clear = () => {
    const a = run.current?.action;
    run.current = null;
    if (a?.kind === "listen") screen.current?.listen(false);
    if (a?.kind === "ask") screen.current?.askEnd();
    if (a?.kind === "scene") screen.current?.sceneClear();
  };

  const play = (a: MoltyAction) => {
    const s = screen.current;
    if (!s) return;
    clear();
    let seconds = ACT_START;
    if (a.kind === "mood") {
      s.state({ mood: a.name, hold_s: MOOD_HOLD });
      seconds = MOOD_HOLD;
    } else if (a.kind === "act") s.state({ act: a.name });
    else if (a.kind === "shake") s.shake();
    else if (a.kind === "listen") {
      s.state({ mood: "listening", hold_s: LISTEN_SECONDS });
      s.listen(true);
      seconds = LISTEN_SECONDS;
    } else if (a.kind === "ask") {
      s.ask(STAGE_QUESTION.question);
      seconds = ASK_SECONDS;
    } else {
      s.scene(a.layout, a.params, a.ttl);
      seconds = a.ttl;
    }
    run.current = { action: a, from: clock.current, until: clock.current + seconds, seen: false, poll: 0 };
    setPlaying(a);
  };

  const onFrame = (seconds: number) => {
    clock.current = seconds;
    const r = run.current;
    if (!r) return;
    if (r.action.kind === "listen") screen.current?.level(madeUpVoice(seconds - r.from));
    if ((r.action.kind === "act" || r.action.kind === "shake") && seconds >= r.poll) {
      r.poll = seconds + ACT_POLL;
      // it plays: over when it stops (one that never starts is over when its wait is)
      if (screen.current?.read()?.act) {
        r.seen = true;
        r.until = Infinity;
      } else if (r.seen) r.until = seconds;
    }
    if (seconds < r.until) return;
    clear();
    setPlaying(null);
  };

  const onAnswer = (index: number) => {
    if (run.current?.action.kind !== "ask") return;
    run.current = null;
    setPlaying(null);
    const then = STAGE_QUESTION.then[index];
    if (then) screen.current?.state(then);
  };

  const onReady = (s: MoltyScreenHandle | null) => {
    screen.current = s;
    setReady(!!s);
  };

  return { ready, playing, play, bind: { onReady, onFrame, onAnswer } };
}
