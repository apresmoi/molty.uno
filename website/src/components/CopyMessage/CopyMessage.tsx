import { useState } from "react";
import styles from "./CopyMessage.module.css";

/** The one pairing message, with a copy action (same message the console hands you). */
export function CopyMessage({ text }: { text: string }) {
  const [copied, setCopied] = useState(false);
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(text);
      setCopied(true);
      setTimeout(() => setCopied(false), 1600);
    } catch {
      setCopied(false);
    }
  };
  return (
    <div className={styles.message} data-fluid-quiet>
      <pre>{text}</pre>
      <button type="button" className={copied ? styles.done : undefined} onClick={copy}>
        {copied ? "[ copied ✓ ]" : "[ copy message ]"}
      </button>
    </div>
  );
}
