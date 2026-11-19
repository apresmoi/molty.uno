import type { ActionGroup } from "../../features/molty/actions";
import { useMoltyStage } from "../../hooks/useMoltyStage";
import { MoltyScreen } from "../MoltyScreen";
import styles from "./MoltyActs.module.css";

/** The cat on its screen and a few things to ask of it: its moods, its own gestures, the
 *  recording ring and a question (the lists come from its package, features/molty/actions).
 *  The line under them is the package's own words for what is playing. Without the renderer
 *  the buttons do nothing and the still stands. */
export function MoltyActs({ groups, rest }: { groups: ActionGroup[]; rest: string }) {
  const stage = useMoltyStage();
  const playing = stage.playing;
  return (
    <div className={styles.root}>
      <div className={styles.slot} data-stage="gallery" role="img" aria-label="The cat on Molty's round screen">
        <MoltyScreen reveal="coin" {...stage.bind} />
      </div>
      <div className={styles.side}>
        {groups.map((g) => (
          <div key={g.label} className={styles.group}>
            <h3>{g.label}</h3>
            <ul className={styles.names}>
              {g.actions.map((a) => (
                <li key={a.name}>
                  <button
                    type="button"
                    aria-pressed={playing?.kind === a.kind && playing.name === a.name}
                    onClick={() => stage.play(a)}
                  >
                    {a.name.replace(/_/g, " ")}
                  </button>
                </li>
              ))}
            </ul>
          </div>
        ))}
        <p className={styles.about} aria-live="polite">
          {playing ? playing.about : rest}
        </p>
      </div>
    </div>
  );
}
