import type { ActionGroup, MoltyAction } from "../../features/molty/actions";
import styles from "./ActionRows.module.css";

const label = (a: MoltyAction) => a.name.replace(/_/g, " ");
const same = (a: MoltyAction, b: MoltyAction | null) => b?.kind === a.kind && b.name === a.name;

/** A stage's buttons (the landing's and the showcase's): a row a group, its label in a column of
 *  its own, thin rules between the groups, each group in its own lines. The one playing is
 *  pressed (an answer to the question plays a copy of a mood, so it matches by kind and name). */
export function ActionRows({
  groups,
  playing,
  onPlay,
}: {
  groups: ActionGroup[];
  playing: MoltyAction | null;
  onPlay: (a: MoltyAction) => void;
}) {
  return (
    <div>
      {groups.map((g) => (
        <div key={g.label} className={styles.group}>
          <h3>{g.label}</h3>
          <div className={styles.lines}>
            {g.rows.map((row, i) => (
              <ul key={i}>
                {row.map((a) => (
                  <li key={a.name}>
                    <button type="button" aria-pressed={same(a, playing)} onClick={() => onPlay(a)}>
                      {label(a)}
                    </button>
                  </li>
                ))}
              </ul>
            ))}
          </div>
        </div>
      ))}
    </div>
  );
}

/** What is playing, under the screen: its name, then the package's own words for it (at rest:
 *  idle). */
export function Playing({ playing, rest }: { playing: MoltyAction | null; rest: string }) {
  return (
    <p className={styles.playing} aria-live="polite">
      <b>{playing ? label(playing) : "idle"}</b> <span>{playing ? playing.about : rest}</span>
    </p>
  );
}
