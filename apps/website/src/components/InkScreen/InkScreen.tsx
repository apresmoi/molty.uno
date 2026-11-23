import { useEffect, useRef, useState } from "react";
import { INK_SCREEN, toInk } from "../../features/device/ink";
import { CAT_STILL } from "../../features/molty/content";
import type { RevealKind } from "../../features/device/reveal";
import { Device } from "../Device";
import styles from "./InkScreen.module.css";

/** The ink screen (a concept): the ink board's frame (Device, shape "ink") round a square of
 *  paper that shows another screen's picture in 1-bit (features/device/ink.ts). It draws only
 *  when handed a picture: `onReady` gives the parent the call that does (a canvas to copy from,
 *  the round screen's own). Its first picture readies the device, which then turns in with it on
 *  its face, as every device below the fold does. Before that (and without JavaScript) the cat's
 *  still stands in, inked by CSS. */
export function InkScreen({
  reveal,
  onReady,
}: {
  reveal: RevealKind;
  onReady: (draw: (source: HTMLCanvasElement) => void) => void;
}) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const [picture, setPicture] = useState<"wait" | "lit">("wait");

  useEffect(() => {
    const ctx = canvas.current!.getContext("2d", { willReadFrequently: true })!;
    const [x, y, side] = INK_SCREEN.crop;
    const px = INK_SCREEN.px;
    ctx.imageSmoothingQuality = "high";
    onReady((source) => {
      ctx.drawImage(source, x, y, side, side, 0, 0, px, px);
      const image = ctx.getImageData(0, 0, px, px);
      toInk(image.data);
      ctx.putImageData(image, 0, 0);
      setPicture("lit");
    });
  }, []);

  return (
    <span className={styles.root} data-ink={picture}>
      <Device reveal={reveal} shape="ink" picture={picture}>
        <span className={styles.paper}>
          <img className={styles.still} src={CAT_STILL} alt="" />
          <canvas ref={canvas} className={styles.canvas} width={INK_SCREEN.px} height={INK_SCREEN.px} />
        </span>
      </Device>
    </span>
  );
}
