import type { SkillScreen } from "../../features/molty/skills";
import { MoltyScreen } from "../MoltyScreen";
import styles from "./SkillsWall.module.css";

/** The wall's screens take every other of the page's frames, in turns: six at once cost what three
 *  would, and a slow page slows their clocks instead of piling up work. */
const PACE = 2;

/** skills: a wall of Molty screens, one for each thing the two real skills show (`screens`:
 *  features/molty/skills.ts), the chart first and big, and a place for the next skill that links to how a skill
 *  is made (`next`). Each screen is a device below the fold (it brings its picture with it); its
 *  caption says what it is and which skill shows it. */
export function SkillsWall({ screens, next }: { screens: SkillScreen[]; next: string }) {
  return (
    <ul className={styles.wall}>
      {screens.map((s) => (
        <li key={s.name} className={s.wide ? styles.wide : undefined}>
          <figure className={styles.tile}>
            <span className={styles.screen} aria-hidden="true">
              <MoltyScreen reveal={s.name === "chart" ? "coin" : "turn"} scene={s.scene} play={s.play} pace={PACE} />
            </span>
            <figcaption>
              <b>
                {s.name} <span data-skill={s.skill}>· {s.skill}</span>
              </b>
              <span>{s.about}</span>
            </figcaption>
          </figure>
        </li>
      ))}
      <li>
        <figure className={styles.tile}>
          <a className={styles.next} href={next}>
            your skill here
          </a>
          <figcaption>
            <b>next</b>
            <span>a SKILL.md and the screens it draws</span>
          </figcaption>
        </figure>
      </li>
    </ul>
  );
}
