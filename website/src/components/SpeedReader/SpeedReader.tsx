import { useEffect, useRef, useState, type FormEvent } from "react";
import { mountSpeedReader, type SpeedReaderHandle } from "../../services/molty-stage";
import styles from "./SpeedReader.module.css";

const SPEEDS = [250, 400, 600] as const;

/** What an agent says through Molty: one word at a time, out of the fluid, on the device's
 *  own round screen. With reduced motion (or no engine) the sentence simply sits there. */
export function SpeedReader({ text }: { text: string }) {
  const host = useRef<HTMLDivElement>(null);
  const reader = useRef<SpeedReaderHandle | null>(null);
  const [live, setLive] = useState(false);
  const [wpm, setWpm] = useState<number>(SPEEDS[0]);
  const [shown, setShown] = useState(text);
  const [typing, setTyping] = useState(false);
  const [draft, setDraft] = useState("");

  useEffect(() => {
    if (matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    let alive = true;
    mountSpeedReader(host.current!, text, SPEEDS[0]).then(
      (r) => {
        if (!alive) return r.destroy();
        reader.current = r;
        setLive(true);
      },
      () => {}, // no engine: the sentence stays as text
    );
    return () => {
      alive = false;
      reader.current?.destroy();
      reader.current = null;
    };
  }, [text]);

  const pickWpm = (v: number) => {
    setWpm(v);
    reader.current?.setWpm(v);
  };
  const submit = (e: FormEvent) => {
    e.preventDefault();
    const next = draft.trim();
    if (!next) return;
    setShown(next);
    reader.current?.setText(next);
  };

  return (
    <div className={styles.root} data-speed-reader="">
      <div className={styles.glass} data-live={live || undefined}>
        <div ref={host} className={styles.host} aria-hidden="true" />
        {!live && <p className={styles.still}>{shown}</p>}
      </div>
      <div className={styles.side} data-fluid-quiet="">
        <p className={styles.said} aria-live="polite">
          “{shown}”
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
              placeholder="say something…"
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
