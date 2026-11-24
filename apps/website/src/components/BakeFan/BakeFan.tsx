import { useRef } from "react";
import { INK_SCREEN } from "../../features/device/ink";
import { boards } from "../../features/home/content";
import type { MoltyScreenHandle } from "../../services/molty-screen";
import { InkScreen } from "../InkScreen";
import { MoltyScreen } from "../MoltyScreen";
import styles from "./BakeFan.module.css";

/** bake: the same cat on three screens lying in a row like cards in a hand, each its true size
 *  (BakeFan.module.css): the round 1.28 at its own 240 pixels, the round 1.75 in front, and the
 *  ink screen (a concept), which shows the 1.75's picture in 1-bit a few times a second (only
 *  while the 1.75 runs: out of view both keep their picture). Each is a device below the fold:
 *  it brings its picture with it. */
/** The 1.28's panel, pixels a side. */
const SMALL = boards.find((b) => b.tag === "1.28")!.px;

export function BakeFan() {
  const big = useRef<MoltyScreenHandle | null>(null);
  const ink = useRef<((source: HTMLCanvasElement) => void) | null>(null);
  const inked = useRef(-Infinity);
  const paint = () => big.current && ink.current?.(big.current.canvas);

  return (
    <div className={styles.fan}>
      <div className={`${styles.place} ${styles.small}`}>
        <MoltyScreen reveal="turn" px={SMALL} />
      </div>
      <div className={`${styles.place} ${styles.ink}`}>
        <InkScreen
          reveal="turn"
          onReady={(draw) => {
            ink.current = draw;
            paint();
          }}
        />
      </div>
      <div className={`${styles.place} ${styles.big}`}>
        <MoltyScreen
          reveal="coin"
          onReady={(s) => {
            big.current = s;
            paint();
          }}
          onFrame={(t) => {
            if (t >= inked.current && t - inked.current < INK_SCREEN.every) return;
            inked.current = t;
            paint();
          }}
        />
      </div>
      <span className={`${styles.label} ${styles.small}`}>1.28 round</span>
      <span className={`${styles.label} ${styles.big}`}>1.75 round</span>
      <span className={`${styles.label} ${styles.ink}`}>ink · concept</span>
    </div>
  );
}
