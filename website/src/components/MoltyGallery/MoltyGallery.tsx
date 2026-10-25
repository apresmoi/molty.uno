import { useMemo, useState } from "react";
import type { Molty } from "../../features/moltys/catalog";
import { MoltyScreen } from "../MoltyScreen";
import styles from "./MoltyGallery.module.css";

export function MoltyGallery({ moltys }: { moltys: Molty[] }) {
  const [selected, setSelected] = useState(moltys[0]);
  const cue = useMemo(() => ({ kind: "molty" as const, id: selected?.id ?? "" }), [selected]);
  if (!selected) return null;
  return (
    <div className={styles.root}>
      <div className={styles.preview}>
        <MoltyScreen
          slot="gallery"
          cue={cue}
          fallbackImage={selected.thumb}
          label={`${selected.name}, live in the fluid`}
        />
        <p className={styles.caption}>
          <span className={styles.name}>{selected.name}</span>
          <a href={selected.file} download={selected.fileName}>
            {selected.fileName} ↓
          </a>
        </p>
      </div>
      <ul className={styles.grid} aria-label="The 26 Moltys">
        {moltys.map((m) => (
          <li key={m.id}>
            <button
              type="button"
              className={styles.tile}
              aria-pressed={m.id === selected.id}
              onClick={() => setSelected(m)}
            >
              <img src={m.thumb} alt="" width={72} height={72} loading="lazy" />
              <span>{m.name}</span>
            </button>
          </li>
        ))}
      </ul>
    </div>
  );
}
