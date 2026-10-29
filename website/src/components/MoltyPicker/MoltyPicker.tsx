import { useState } from "react";
import type { Molty } from "../../features/moltys/catalog";
import { GlassDome } from "../GlassDome";
import styles from "./MoltyPicker.module.css";

/** The Moltys, by name. The chosen one emerges from the page fluid in the slot (the fluid
 *  reads data-fluid-molty); its still portrait stands in when the engine cannot run. */
export function MoltyPicker({ moltys, initial }: { moltys: Molty[]; initial: string }) {
  const [selected, setSelected] = useState(moltys.find((m) => m.id === initial) ?? moltys[0]);
  if (!selected) return null;
  return (
    <div className={styles.root}>
      <div
        className={styles.slot}
        data-fluid-molty={selected.id}
        data-fluid-slot="gallery"
        role="img"
        aria-label={`${selected.name}, emerging from the fluid`}
      >
        <img className={styles.still} src={selected.thumb} alt="" width={150} height={150} />
        <GlassDome />
      </div>
      <div className={styles.side}>
        <ul className={styles.names} aria-label={`The ${moltys.length} Moltys`}>
          {moltys.map((m) => (
            <li key={m.id}>
              <button type="button" aria-pressed={m.id === selected.id} onClick={() => setSelected(m)}>
                {m.name.toLowerCase()}
              </button>
            </li>
          ))}
        </ul>
        <p className={styles.file}>
          <a href={selected.file} download={selected.fileName}>
            [ {selected.fileName} ↓ ]
          </a>
        </p>
      </div>
    </div>
  );
}
