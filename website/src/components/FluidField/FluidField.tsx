import { useEffect, useRef } from "react";
import { mountPageFluid, type PageFluidHandle } from "../../services/molty-stage";
import styles from "./FluidField.module.css";

/** The one live fluid behind the landing page. It reads its targets (headings, Molty slots,
 *  quiet and pool zones) from data-fluid-* attributes in the page, so the page stays static
 *  HTML that reads fine without it. Sets <html data-fluid="live"> once it draws (@glyphcss/fluid), and
 *  data-fluid-moltys="live" once the Moltys can emerge (their three.js rig loaded). */
export function FluidField() {
  const host = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const root = document.documentElement;
    let handle: PageFluidHandle | null = null;
    let alive = true;
    mountPageFluid(host.current!, {
      reducedMotion: matchMedia("(prefers-reduced-motion: reduce)").matches,
      onState: (state) => {
        if (state === "live") root.dataset.fluid = "live";
        else root.dataset.fluidMoltys = "live";
      },
    }).then(
      (h) => (alive ? (handle = h) : h.destroy()),
      () => {}, // engine unavailable: the page keeps its plain text and still portraits
    );
    return () => {
      alive = false;
      handle?.destroy();
      delete root.dataset.fluid;
      delete root.dataset.fluidMoltys;
    };
  }, []);
  return <div ref={host} className={styles.field} data-fluid-field="" aria-hidden="true" />;
}
