import { useEffect, useRef, useState, type FormEvent } from "react";
import type { MoltyScreenHandle } from "../../services/molty-screen";
import { MoltyScreen } from "../MoltyScreen";
import styles from "./SpeedReader.module.css";

export type Choice = "yes" | "no" | "idk";
const CHOICES: readonly Choice[] = ["yes", "no", "idk"];
const SPEEDS = [250, 400, 600] as const;
const LISTEN_S = 12; // no tap: the question lets go and is asked again
const REASK_S = 1.4;
const REST_S = 2.6; // after the answer (or a plain line), before it starts over
const TITLE_MAX = 40; // the longest question the rim's title takes (fluid_ask.h)
/** What it says back to a typed question. */
const SAID: Record<Choice, string> = {
  yes: "On it.",
  no: "Okay, I won't.",
  idk: "Fair, I'll hold off.",
};

type Phase = "say" | "listen" | "answer" | "rest";

/** The words of a line that fit the question's title: its last sentence, if it asks. */
function titleOf(line: string): string | undefined {
  const last =
    line
      .trim()
      .split(/(?<=[.!?])\s+/)
      .pop() ?? "";
  return last.endsWith("?") && last.length <= TITLE_MAX && /^[\x20-\x7e]+$/.test(last) ? last.toLowerCase() : undefined;
}
/** About how long a line takes to read, for the conversation without the renderer. */
const readingSeconds = (line: string, wpm: number) => 0.55 + (line.trim().split(/\s+/).length * 60) / wpm + 1.5;

/** What an agent says through Molty, on its round screen, as the device does it: the line one
 *  word at a time (the reader), then the cat is back and the question stands on the rim with
 *  yes, no and idk to tap on the glass; the answer is read back. The conversation runs on the
 *  screen's own clock. With reduced motion (or no renderer) the lines sit beside the screen and
 *  the choices still work, here and as buttons under the line. */
export function SpeedReader({ text, answers: own }: { text: string; answers: Record<Choice, string> }) {
  const screen = useRef<MoltyScreenHandle | null>(null);
  const clock = useRef(0);
  const next = useRef<{ at: number; poll: number; reading: boolean; then: () => void } | null>(null);
  const timer = useRef(0);
  const wpmNow = useRef<number>(SPEEDS[0]);
  const driven = useRef(false); // the renderer plays the words (else a timer stands in)
  const [engine, setEngine] = useState<"pending" | "live" | "still">("pending");
  const [wpm, setWpm] = useState<number>(SPEEDS[0]);
  const [question, setQuestion] = useState(text);
  const [round, setRound] = useState(0); // a re-typed line starts over too
  const [shown, setShown] = useState(text);
  const [phase, setPhase] = useState<Phase>("say");
  const [picked, setPicked] = useState<Choice | null>(null);
  const [typing, setTyping] = useState(false);
  const [draft, setDraft] = useState("");

  const answers = question === text ? own : SAID;
  const asks = question.trim().endsWith("?");

  /** `then` after `seconds`, or (reading) once the reader has left its last word. */
  const after = (seconds: number, then: () => void, reading = false) => {
    clearTimeout(timer.current);
    next.current = null;
    if (driven.current) next.current = { at: clock.current + seconds, poll: clock.current + 0.6, reading, then };
    else timer.current = window.setTimeout(then, seconds * 1000);
  };
  const say = (line: string, then: () => void) => {
    setShown(line);
    if (driven.current) screen.current!.state({ state: "text", text: line, wpm: wpmNow.current });
    after(readingSeconds(line, wpmNow.current) + (driven.current ? 4 : 0), then, true);
  };
  const rest = (seconds: number) => {
    setPhase("rest");
    after(seconds, ask);
  };
  const ask = () => {
    setPicked(null);
    setPhase("say");
    say(question, () => {
      if (!asks) return rest(REST_S);
      setPhase("listen");
      screen.current?.ask({ title: titleOf(question), options: [...CHOICES] });
      // nobody tapped: let it go and ask again (without the renderer it just waits)
      if (driven.current)
        after(LISTEN_S, () => {
          screen.current?.askEnd();
          rest(REASK_S);
        });
    });
  };
  const answered = (i: number) => {
    const choice = CHOICES[i];
    if (!choice) return;
    setPicked(choice);
    setPhase("answer");
    after(driven.current ? 1.4 : 0.7, () => say(answers[choice], () => rest(REST_S))); // its lines blow away first
  };

  // the screen's clock runs the conversation
  const onFrame = (seconds: number) => {
    clock.current = seconds;
    const n = next.current;
    if (!n) return;
    if (n.reading && seconds >= n.poll) {
      n.poll = seconds + 0.2;
      if (screen.current?.read()?.state !== "text") n.at = seconds; // the cat is back
    }
    if (seconds < n.at) return;
    next.current = null;
    n.then();
  };

  useEffect(() => {
    if (engine === "pending") return;
    ask();
    return () => {
      clearTimeout(timer.current);
      next.current = null;
      screen.current?.askEnd();
    };
  }, [engine, question, round]); // restarts on a (re-)typed line only

  // no renderer after a while (it failed, or the screen is far off): the lines still play
  useEffect(() => {
    const id = setTimeout(() => setEngine((e) => (e === "pending" ? "still" : e)), 4000);
    return () => clearTimeout(id);
  }, []);

  const pickWpm = (v: number) => {
    setWpm(v);
    wpmNow.current = v;
    screen.current?.state({ wpm: v });
  };
  const submit = (e: FormEvent) => {
    e.preventDefault();
    const line = draft.trim();
    if (!line) return;
    setQuestion(line);
    setShown(line); // the side quote follows at once; the loop starts over with it
    setRound((n) => n + 1);
  };

  const listening = phase === "listen";
  const status = listening ? "listening… tap yes / no / idk" : picked ? `you tapped ${picked}` : "";

  return (
    <div className={styles.root} data-speed-reader="" data-phase={phase}>
      <div className={styles.glass} data-reader-glass="">
        <MoltyScreen
          reveal="lid"
          onReady={(s) => {
            screen.current = s;
            // reduced motion: the screen shows stills; the lines stay beside it
            driven.current = !!s && !matchMedia("(prefers-reduced-motion: reduce)").matches;
            setEngine(s ? "live" : "still");
          }}
          onFrame={onFrame}
          onAnswer={answered}
        />
      </div>
      <div className={styles.side}>
        <p className={styles.said} aria-live="polite">
          “{shown}”
        </p>
        <p className={styles.status} aria-live="polite">
          {status}
        </p>
        {/* always laid out, so the page holds still when it starts to listen */}
        <p className={styles.row} role="group" aria-label="Answer Molty" data-off={!listening || undefined}>
          {CHOICES.map((c, i) => (
            <button
              key={c}
              type="button"
              className={styles.answer}
              onClick={() => (screen.current ? screen.current.pick(i) : answered(i))}
            >
              {c}
            </button>
          ))}
        </p>
        <p className={styles.row} role="group" aria-label="Words per minute">
          <span>wpm</span>
          {SPEEDS.map((v) => (
            <button key={v} type="button" aria-pressed={v === wpm} onClick={() => pickWpm(v)}>
              {v}
            </button>
          ))}
        </p>
        {typing ? (
          <form className={styles.form} onSubmit={submit}>
            <input
              autoFocus
              value={draft}
              maxLength={160}
              onChange={(e) => setDraft(e.target.value)}
              placeholder="say something… end with ? to ask"
              aria-label="Text for Molty to read"
            />
            <button type="submit">[ read ]</button>
          </form>
        ) : (
          <button type="button" className={styles.own} onClick={() => setTyping(true)}>
            [ type your own ]
          </button>
        )}
      </div>
    </div>
  );
}
