import type { ActionGroup } from "../../features/molty/actions";
import { useMoltyStage } from "../../hooks/useMoltyStage";
import { ActionRows, Playing } from "../ActionRows";
import { MoltyScreen } from "../MoltyScreen";
import styles from "./MoltyActs.module.css";

/** The cat on its screen and a few things to ask of it: its moods, its own gestures, the
 *  recording ring and a question (the lists come from its package, features/molty/actions), a
 *  labelled row each. Under the screen, what is playing and the package's own words for it.
 *  Without the renderer the buttons do nothing and the still stands. */
export function MoltyActs({ groups, rest }: { groups: ActionGroup[]; rest: string }) {
  const stage = useMoltyStage(groups);
  return (
    <div className={styles.root}>
      <div className={styles.stage} data-stage="gallery">
        <div className={styles.slot} role="img" aria-label="The cat on Molty's round screen">
          <MoltyScreen reveal="coin" {...stage.bind} />
        </div>
        <Playing playing={stage.playing} rest={rest} />
      </div>
      <div className={styles.side}>
        <ActionRows groups={groups} playing={stage.playing} onPlay={stage.play} />
      </div>
    </div>
  );
}
