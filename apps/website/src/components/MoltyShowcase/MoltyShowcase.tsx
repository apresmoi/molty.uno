import { useEffect, useMemo, useRef, useState } from "react";
import { everyAction, type ActionGroup, type MoltyAction } from "../../features/molty/actions";
import { AUTOPLAY_GAP, readShowcaseUrl, writeShowcaseUrl } from "../../features/showcase/content";
import { useMoltyStage } from "../../hooks/useMoltyStage";
import { ActionRows, Playing } from "../ActionRows";
import { MoltyScreen } from "../MoltyScreen";
import styles from "./MoltyShowcase.module.css";

/** The showcase stage: the cat on a big screen, what is playing under it (the package's own words
 *  for it), and every action it has, on demand, a labelled row a group, as on the landing (the
 *  lists come from its package and the scenes package, features/molty/actions). URL: ?a=<action>. */
export function MoltyShowcase({ groups, rest }: { groups: ActionGroup[]; rest: string }) {
  const stage = useMoltyStage(groups);
  const [auto, setAuto] = useState(false);
  const slot = useRef<HTMLDivElement>(null);
  const last = useRef<MoltyAction | null>(null);
  const all = useMemo(() => everyAction(groups), [groups]);
  const playing = stage.playing;

  /** Bring the stage back on screen when a tap far below it changed it (phones). */
  const reveal = () => {
    const r = slot.current?.getBoundingClientRect();
    if (r && (r.bottom < r.height * 0.5 || r.top > innerHeight - r.height * 0.5))
      slot.current!.scrollIntoView({ block: "center", behavior: "smooth" });
  };
  const play = (a: MoltyAction) => {
    last.current = a;
    stage.play(a);
    history.replaceState(
      history.state,
      "",
      location.pathname + writeShowcaseUrl(location.search, a.name) + location.hash,
    );
  };
  const random = () => {
    const pool = all.filter((a) => a !== last.current);
    play(pool[Math.floor(Math.random() * pool.length)] ?? all[0]!);
  };

  // a link opens the cat doing an action: ?a=<action> (malformed or unknown: ignored)
  useEffect(() => {
    if (!stage.ready) return;
    const name = readShowcaseUrl(location.search);
    const a = all.find((x) => x.name === name);
    if (a) play(a);
  }, [stage.ready]);

  // autoplay: the next action a moment after one ends
  useEffect(() => {
    if (!auto || !stage.ready || playing) return;
    const id = setTimeout(random, AUTOPLAY_GAP * 1000);
    return () => clearTimeout(id);
  }, [auto, stage.ready, playing]);

  return (
    <div className={styles.root}>
      <div className={styles.stage}>
        <div
          ref={slot}
          className={styles.screen}
          data-stage="showcase"
          role="img"
          aria-label="The cat on Molty's round screen"
        >
          <MoltyScreen reveal="turn" scenes {...stage.bind} />
        </div>
        <Playing playing={playing} rest={rest} />
      </div>

      <div className={styles.panel}>
        <div className={styles.play}>
          <button
            type="button"
            onClick={() => {
              random();
              reveal();
            }}
          >
            random
          </button>
          <button type="button" aria-pressed={auto} onClick={() => setAuto((x) => !x)}>
            autoplay: {auto ? "on" : "off"}
          </button>
        </div>
        <ActionRows
          groups={groups}
          playing={playing}
          onPlay={(a) => {
            play(a);
            reveal();
          }}
        />
        <p className={styles.more}>More creatures are coming.</p>
      </div>
    </div>
  );
}
