import { useEffect, useRef, useState } from "react";
import { acquireStage, attachWhileVisible, type ScreenCue } from "../../services/molty-stage";
import styles from "./MoltyScreen.module.css";

export interface MoltyScreenProps {
  /** One live stage per slot; remounts reuse it. */
  slot: string;
  /** What the fluid should be right now. */
  cue: ScreenCue;
  /** Shown inside the glass if the engine cannot load (offline, CDN blocked). */
  fallbackImage?: string;
  label: string;
  size?: "hero" | "panel";
}

/** The round Molty glass, running the real fluid engine from creatures/js. */
export function MoltyScreen({ slot, cue, fallbackImage, label, size = "panel" }: MoltyScreenProps) {
  const host = useRef<HTMLDivElement>(null);
  const [stage, setStage] = useState<Awaited<ReturnType<typeof acquireStage>> | null>(null);
  const [state, setState] = useState<"loading" | "live" | "failed">("loading");

  useEffect(() => {
    let alive = true;
    let detach = () => {};
    acquireStage(slot).then(
      (s) => {
        if (!alive || !host.current) return;
        detach = attachWhileVisible(s, host.current);
        setStage(s);
        setState("live");
      },
      () => alive && setState("failed"),
    );
    return () => {
      alive = false;
      detach();
    };
  }, [slot]);

  useEffect(() => {
    if (!stage) return;
    if (cue.kind === "molty") stage.show(cue.id);
    else if (cue.kind === "word") stage.word(cue.word);
    else stage.blob();
  }, [stage, cue]);

  return (
    <figure className={`${styles.root} ${size === "hero" ? styles.hero : styles.panel}`} data-state={state}>
      <div className={styles.glass} role="img" aria-label={label}>
        <div ref={host} className={styles.host} />
        {state === "loading" && (
          <span className={styles.wait} aria-hidden="true">
            ⠶
          </span>
        )}
        {state === "failed" && fallbackImage && <img className={styles.fallback} src={fallbackImage} alt="" />}
      </div>
    </figure>
  );
}
