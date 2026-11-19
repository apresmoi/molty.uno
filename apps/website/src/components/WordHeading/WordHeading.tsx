import type { CSSProperties, ReactNode } from "react";
import { measure } from "../../config/words.mjs";
import styles from "./WordHeading.module.css";

/** A heading whose word the device's renderer draws (`word`: a MoltyWord island playing that
 *  word's layout), as a scene's thin lines with the dust along them. The heading is as wide as
 *  the word is drawn (`--fill` of the round window, `--word-canvas` across: the page's class may
 *  set that) and its own text is sized to fill that width, so it stands in where the renderer
 *  does not run: selectable, read aloud, and stepping aside once the word draws. */
export function WordHeading({
  as: Tag = "h2",
  id,
  text,
  word,
  className,
}: {
  as?: "h1" | "h2";
  id?: string;
  text: string;
  word: ReactNode;
  className?: string;
}) {
  return (
    <Tag
      id={id}
      className={`${styles.heading} ${className ?? ""}`}
      style={{ "--fill": measure(text).half, "--chars": text.length } as CSSProperties}
    >
      {text}
      {word}
    </Tag>
  );
}
