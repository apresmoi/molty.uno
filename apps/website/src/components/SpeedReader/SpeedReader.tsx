import { useEffect, useRef, useState, type FormEvent, type MouseEvent } from "react";
import { CHOICES, mountSpeedReader, readingMs, type Choice, type SpeedReaderHandle } from "../../services/molty-stage";
import { Device } from "../Device";
import styles from "./SpeedReader.module.css";

const SPEEDS = [250, 400, 600] as const;
const LISTEN_MS = 9000; // no tap: ask again
const REASK_MS = 1400;
const ANSWER_DELAY = 700; // the tapped word holds, then the answer starts
const REST_MS = 2600; // after the answer (or a plain line), before it starts over
/** What it says back to a typed question. */
const SAID: Record<Choice, string> = {
  yes: "On it.",
  no: "Okay, I won't.",
  idk: "Fair, I'll hold off.",
};

type Phase = "say" | "listen" | "answer" | "rest";

/** What an agent says through Molty, on its round screen: the question one word at a time
 *  out of the fluid, then the same screen listens with "yes", "no" and "idk" in three sectors to tap;
 *  the answer is read back. With reduced motion (or no engine) the lines simply sit there and
 *  the choices still work. */
export function SpeedReader({ text, answers: own }: { text: string; answers: Record<Choice, string> }) {
  const readerHost = useRef<HTMLDivElement>(null);
  const words = useRef<Partial<Record<Choice, HTMLSpanElement | null>>>({});
  const handle = useRef<SpeedReaderHandle | null>(null);
  const timer = useRef(0);
  const gen = useRef(0);
  const wpmNow = useRef<number>(SPEEDS[0]);
  const [live, setLive] = useState(false);
  const [wpm, setWpm] = useState<number>(SPEEDS[0]);
  const [question, setQuestion] = useState(text);
  const [round, setRound] = useState(0); // a re-typed line starts over too
  const [shown, setShown] = useState(text);
  const [phase, setPhase] = useState<Phase>("say");
  const [picked, setPicked] = useState<Choice | null>(null);
  const [held, setHeld] = useState<Choice | null>(null); // the tapped word, until the answer starts
  const [typing, setTyping] = useState(false);
  const [draft, setDraft] = useState("");
  const [engine, setEngine] = useState<"pending" | "live" | "still">("pending");

  // the engine: one fluid on the round screen, mounted once
  useEffect(() => {
    if (matchMedia("(prefers-reduced-motion: reduce)").matches) return setEngine("still");
    let alive = true;
    const hosts = {
      reader: readerHost.current!,
      choices: words.current as Record<Choice, HTMLElement>,
    };
    mountSpeedReader(hosts, SPEEDS[0]).then(
      (h) => {
        if (!alive) return h.destroy();
        handle.current = h;
        setLive(true);
        setEngine("live");
      },
      () => alive && setEngine("still"), // no engine: the lines stay as text
    );
    return () => {
      alive = false;
      handle.current?.destroy();
      handle.current = null;
    };
  }, []);

  // the conversation: say the question → listen (yes / no / idk) → tap → say the answer → rest
  const answers = question === text ? own : SAID;
  const asks = question.trim().endsWith("?");
  const later = (ms: number, fn: () => void) => {
    const g = gen.current;
    clearTimeout(timer.current);
    timer.current = window.setTimeout(() => g === gen.current && fn(), ms);
  };
  const say = (line: string, then: () => void) => {
    const g = gen.current;
    setShown(line);
    setPhase((p) => (p === "answer" ? p : "say"));
    const done = () => g === gen.current && then();
    if (handle.current) handle.current.say(line, done);
    else later(readingMs(line, wpmNow.current), done);
  };
  const rest = (ms: number) => {
    setPhase("rest");
    later(ms, ask);
  };
  const ask = () => {
    setPicked(null);
    setPhase("say");
    say(question, () => {
      if (!asks) return rest(REST_MS);
      setPhase("listen");
      handle.current?.listen(true);
      // nobody tapped: let it go and ask again (the still demo just waits)
      if (handle.current)
        later(LISTEN_MS, () => {
          handle.current?.listen(false);
          rest(REASK_MS);
        });
    });
  };
  useEffect(() => {
    if (engine === "pending") return;
    gen.current++;
    setHeld(null);
    handle.current?.listen(false);
    ask();
    return () => {
      gen.current++;
      clearTimeout(timer.current);
    };
  }, [engine, question, round]); // restarts on a (re-)typed line only

  const tap = (choice: Choice, e: MouseEvent<HTMLButtonElement>) => {
    if (phase !== "listen") return;
    // a keyboard press has no point: ripple from the sector's word
    const r = (words.current[choice] ?? e.currentTarget).getBoundingClientRect();
    const x = e.detail ? e.clientX : r.left + r.width / 2;
    const y = e.detail ? e.clientY : r.top + r.height / 2;
    handle.current?.tap(choice, x, y);
    setPicked(choice);
    setHeld(choice);
    setPhase("answer");
    later(ANSWER_DELAY, () => {
      setHeld(null);
      say(answers[choice], () => rest(REST_MS));
    });
  };

  const pickWpm = (v: number) => {
    setWpm(v);
    wpmNow.current = v;
    handle.current?.setWpm(v);
  };
  const submit = (e: FormEvent) => {
    e.preventDefault();
    const next = draft.trim();
    if (!next) return;
    setQuestion(next);
    setShown(next); // the side quote follows at once; the loop starts over with it
    setRound((n) => n + 1);
  };

  const listening = phase === "listen";
  const status = listening ? "listening… or tap yes / no / idk" : picked ? `you tapped ${picked}` : "";

  return (
    <div className={styles.root} data-speed-reader="">
      <div
        className={styles.glass}
        data-reader-glass=""
        data-screen=""
        data-live={live || undefined}
        data-listening={listening || undefined}
        data-held={held ?? undefined}
      >
        <div ref={readerHost} className={styles.host} aria-hidden="true" />
        {!live && !listening && <p className={styles.still}>{shown}</p>}
        {/* three 120° sectors are the answer buttons; their words are what the fluid draws */}
        <div className={styles.sectors} role="group" aria-label="Answer on Molty's screen">
          {CHOICES.map((c) => (
            <button
              key={c}
              type="button"
              className={styles.sector}
              data-choice={c}
              disabled={!listening}
              aria-label={`Tap ${c}`}
              onClick={(e) => tap(c, e)}
            >
              <span className={styles.spot}>
                <span ref={(el) => void (words.current[c] = el)} className={styles.choice}>
                  {c}
                </span>
                <span className={styles.hint} aria-hidden="true">
                  tap
                </span>
              </span>
            </button>
          ))}
        </div>
        <Device reveal="lid" />
      </div>
      <div className={styles.side} data-fluid-quiet="">
        <p className={styles.said} aria-live="polite">
          “{shown}”
        </p>
        <p className={styles.status} aria-live="polite">
          {status}
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
