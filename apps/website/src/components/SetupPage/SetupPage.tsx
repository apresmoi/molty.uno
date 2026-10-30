import { useEffect, useState } from "react";
import {
  copy,
  FIRMWARE_INDEX,
  PORTING,
  title,
  verdict,
  type SetupBoard,
  type SetupIndex,
} from "../../features/setup/content";
import { findBoard, openInstaller, serialSupported } from "../../services/serial-setup";
import styles from "./SetupPage.module.css";

type Phase =
  | { kind: "idle" }
  | { kind: "busy"; text: string }
  | { kind: "done"; ok: boolean; text: string; board?: SetupBoard };

/** Connect → the page identifies the board on the port (molty firmware over Improv, else the
 *  chip's fingerprint in its ROM bootloader) → ESP Web Tools opens with that board's
 *  firmware only. No board choice: a wrong pick is how boards get the wrong image. */
export function SetupPage() {
  const [index, setIndex] = useState<SetupIndex | null | undefined>(undefined);
  const [phase, setPhase] = useState<Phase>({ kind: "idle" });
  const [supported, setSupported] = useState(true);

  useEffect(() => {
    setSupported(serialSupported());
    fetch(FIRMWARE_INDEX)
      .then((r) => (r.ok ? r.json() : null))
      .then((data: SetupIndex | null) => setIndex(data))
      .catch(() => setIndex(null));
  }, []);

  const connect = async () => {
    if (!index) return;
    setPhase({ kind: "busy", text: "Choose the serial port…" });
    try {
      const found = await findBoard(index.boards, (text) => setPhase({ kind: "busy", text }));
      if (!found) return setPhase({ kind: "done", ok: false, text: copy.noPort });
      const v = verdict(found.result, found.evidence);
      if (!found.result.ok) return setPhase({ kind: "done", ok: false, text: v.text });
      const board = found.result.board;
      if (!board.available) return setPhase({ kind: "done", ok: false, text: `${v.text} ${copy.noFirmware}`, board });
      setPhase({ kind: "done", ok: true, text: v.text, board });
      await openInstaller(found.port, board.manifest);
    } catch (err) {
      setPhase({
        kind: "done",
        ok: false,
        text: `Could not talk to the device: ${String((err as Error)?.message ?? err)}`,
      });
    }
  };

  return (
    <section className={styles.setup}>
      <h1>{title}</h1>
      <p className={styles.note}>{copy.browser}</p>
      <ol className={styles.steps}>
        {copy.steps.map((s) => (
          <li key={s}>{s}</li>
        ))}
      </ol>
      <p className={styles.note}>{copy.nativeUsb}</p>

      {index === null && <p className={styles.warn}>{copy.noIndex}</p>}
      {!supported && <p className={styles.warn}>{copy.unsupported}</p>}

      {index && supported && (
        <div className={styles.action}>
          <button type="button" className={styles.button} disabled={phase.kind === "busy"} onClick={connect}>
            {phase.kind === "done" ? copy.again : copy.connect}
          </button>
          {phase.kind === "busy" && <p className={styles.note}>{phase.text}</p>}
          {phase.kind === "done" && (
            <div className={phase.ok ? styles.found : styles.refused}>
              <p>{phase.text}</p>
              {!phase.ok && (
                <p className={styles.note}>
                  {copy.recoverable} <a href={PORTING}>Porting guide</a>
                </p>
              )}
            </div>
          )}
        </div>
      )}

      {index && (
        <div className={styles.boards}>
          <p className={styles.note}>Supported boards</p>
          <ul>
            {index.boards.map((b) => (
              <li key={b.name}>
                {b.title}
                <small>
                  {b.display.width}×{b.display.height} {b.display.shape} · {b.status}
                  {b.available ? "" : " · no firmware on this site yet"}
                </small>
              </li>
            ))}
          </ul>
          {index.version && <p className={styles.small}>molty.UNO {index.version}</p>}
        </div>
      )}

      <p className={styles.note}>{copy.privacy}</p>
    </section>
  );
}
