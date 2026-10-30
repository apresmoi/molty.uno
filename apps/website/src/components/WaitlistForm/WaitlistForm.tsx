import { useEffect, useId, useRef, useState, type FormEvent } from "react";
import { CONTACT_EMAIL, TURNSTILE_SITE_KEY, WAITLIST_URL } from "../../config/site.mjs";
import { readWaitlistResult, waitlistCopy as copy, waitlistMessage } from "../../features/build/content";
import styles from "./WaitlistForm.module.css";

type Phase = { kind: "idle" } | { kind: "sending" } | { kind: "done" } | { kind: "error"; reason: string };

const TURNSTILE_JS = "https://challenges.cloudflare.com/turnstile/v0/api.js";

/** "Want one built?": one email field posting to the relay's POST /waitlist. Without
 *  JavaScript it is a plain form POST; the relay redirects back with ?waitlist=ok#waitlist-ok
 *  and the matching message shows through :target. With JavaScript it posts JSON and
 *  shows idle / sending / done / error in place. `website` is a honeypot. */
export function WaitlistForm({ source, returnPath }: { source: string; returnPath: string }) {
  const [phase, setPhase] = useState<Phase>({ kind: "idle" });
  const [live, setLive] = useState(false);
  const form = useRef<HTMLFormElement>(null);
  const id = useId();

  useEffect(() => {
    setLive(true);
    const back = readWaitlistResult(location.search);
    if (back) {
      setPhase(back);
      const url = new URL(location.href);
      url.searchParams.delete("waitlist");
      url.searchParams.delete("reason");
      history.replaceState(history.state, "", url.pathname + url.search + url.hash);
    }
    if (TURNSTILE_SITE_KEY && !document.querySelector(`script[src="${TURNSTILE_JS}"]`)) {
      const s = document.createElement("script");
      s.src = TURNSTILE_JS;
      s.async = true;
      document.head.appendChild(s);
    }
  }, []);

  const submit = async (e: FormEvent<HTMLFormElement>) => {
    e.preventDefault();
    const data = new FormData(e.currentTarget);
    setPhase({ kind: "sending" });
    try {
      const res = await fetch(WAITLIST_URL, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
          email: data.get("email"),
          website: data.get("website"),
          source,
          turnstile: data.get("cf-turnstile-response") ?? undefined,
        }),
      });
      const out = (await res.json().catch(() => ({}))) as { ok?: boolean; error?: string };
      if (res.ok && out.ok) {
        setPhase({ kind: "done" });
        form.current?.reset();
      } else setPhase({ kind: "error", reason: out.error ?? "default" });
    } catch {
      setPhase({ kind: "error", reason: "default" });
    }
  };

  const sending = phase.kind === "sending";
  const status =
    phase.kind === "sending"
      ? copy.sending
      : phase.kind === "done"
        ? copy.done
        : phase.kind === "error"
          ? waitlistMessage(phase.reason)
          : "";

  return (
    <form
      ref={form}
      className={styles.form}
      method="post"
      action={WAITLIST_URL}
      onSubmit={submit}
      data-live={live ? "" : undefined}
      aria-busy={sending}
    >
      <input type="hidden" name="source" value={source} />
      <input type="hidden" name="return" value={returnPath} />
      {phase.kind !== "done" && (
        <div className={styles.row}>
          <label htmlFor={`${id}-email`} className={styles.label}>
            {copy.label}
          </label>
          <input
            id={`${id}-email`}
            className={styles.input}
            type="email"
            name="email"
            required
            maxLength={254}
            autoComplete="email"
            placeholder={copy.placeholder}
            aria-invalid={phase.kind === "error" && phase.reason === "bad_email" ? true : undefined}
            aria-describedby={`${id}-status ${id}-privacy`}
            disabled={sending}
          />
          <button type="submit" className={styles.submit} disabled={sending}>
            [ {sending ? copy.sending : copy.submit} ]
          </button>
        </div>
      )}
      {/* honeypot: hidden from people and screen readers, filled by bots */}
      <div className={styles.trap} aria-hidden="true">
        <label>
          website
          <input type="text" name="website" tabIndex={-1} autoComplete="off" defaultValue="" />
        </label>
      </div>
      {TURNSTILE_SITE_KEY && <div className="cf-turnstile" data-sitekey={TURNSTILE_SITE_KEY} />}
      <p
        id={`${id}-status`}
        role="status"
        aria-live="polite"
        className={phase.kind === "error" ? styles.error : phase.kind === "done" ? styles.done : styles.status}
      >
        {status}
      </p>
      {/* without JavaScript: the relay's redirect lands on one of these (:target) */}
      <p id="waitlist-ok" className={`${styles.target} ${styles.done}`}>
        {copy.done}
      </p>
      <p id="waitlist-error" className={`${styles.target} ${styles.error}`}>
        {copy.formError}
      </p>
      <p id={`${id}-privacy`} className={styles.privacy}>
        {copy.privacy} <a href={`mailto:${CONTACT_EMAIL}`}>{CONTACT_EMAIL}</a>
      </p>
    </form>
  );
}
