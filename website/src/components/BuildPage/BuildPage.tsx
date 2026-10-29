import type { ReactNode } from "react";
import { AMAZON_DISCLOSURE, shopLink } from "../../config/affiliate.mjs";
import { REPO_URL } from "../../config/site.mjs";
import type { CommunityBoard } from "../../features/build/boards";
import {
  BUILD_INK,
  PORTING_HREF,
  SUBMIT_HREF,
  intro,
  niceToHave,
  partGroups,
  requirements,
  waitlistCopy,
  type PartLink,
} from "../../features/build/content";
import styles from "./BuildPage.module.css";

function Out({ link }: { link: PartLink }) {
  const shop = shopLink(link.url);
  return (
    <span className={styles.out}>
      <a href={shop.href} rel={shop.rel} target="_blank">
        [ {link.label} ↗ ]
      </a>
      {link.note && <small>{link.note}</small>}
    </span>
  );
}

/** /build/: will my board work, the reference build's parts, community builds (from every
 *  board.json), and the waitlist (the `waitlist` island). Static apart from the waitlist;
 *  the heading is drawn by the page fluid behind it. */
export function BuildPage({
  fluid,
  waitlist,
  boards,
}: {
  fluid: ReactNode;
  waitlist: ReactNode;
  boards: CommunityBoard[];
}) {
  return (
    <div className={styles.root}>
      {fluid}

      <section className={styles.hero} aria-labelledby="build">
        <h1 id="build" className={styles.word} data-fluid-word="" data-fluid-color={BUILD_INK} data-fluid-delay="0.3">
          build
        </h1>
        <p className={styles.lede} data-fluid-quiet="">
          {intro}
        </p>
        <nav className={styles.links} aria-label="On this page" data-fluid-quiet="">
          <a href="#fit">[ will my board work? ]</a>
          <a href="#parts">[ parts ]</a>
          <a href="#community">[ community builds ]</a>
          <a href="#waitlist">[ get one built ]</a>
        </nav>
      </section>

      <section className={styles.section} aria-labelledby="fit" data-fluid-quiet="">
        <div className={styles.box}>
          <h2 id="fit" className={styles.title}>
            will my board work?
          </h2>
          <p className={styles.kicker}>needs</p>
          <dl className={styles.rows}>
            {requirements.map((r) => (
              <div key={r.row}>
                <dt>{r.label}</dt>
                <dd>
                  {r.need}
                  {r.note && <small>{r.note}</small>}
                </dd>
              </div>
            ))}
          </dl>
          <p className={styles.kicker}>nice to have · works without, minus that feature</p>
          <dl className={styles.rows}>
            {niceToHave.map((n) => (
              <div key={n.cap}>
                <dt>{n.label}</dt>
                <dd>
                  <small>{n.without}</small>
                </dd>
              </div>
            ))}
          </dl>
          <p className={styles.links}>
            <a href={PORTING_HREF}>[ have one that fits? port it → ]</a>
          </p>
        </div>
      </section>

      <section className={styles.section} aria-labelledby="parts" data-fluid-quiet="">
        <h2 id="parts" className={styles.title}>
          reference build
        </h2>
        <p className={styles.text}>The board everything is tested on. The rest is optional.</p>
        {partGroups.map((g) => (
          <div key={g.id} className={styles.group}>
            <h3 className={styles.groupTitle}>
              {g.title}
              {g.tag && <span className={g.id === "vibration" ? styles.untested : styles.tag}>{g.tag}</span>}
            </h3>
            {g.note && <p className={styles.note}>{g.note}</p>}
            {g.parts.length > 0 && (
              <ul className={styles.parts}>
                {g.parts.map((p) => (
                  <li key={p.name}>
                    <span className={styles.partName}>{p.name}</span>
                    {p.spec && <small>{p.spec}</small>}
                    {p.links && (
                      <span className={styles.outs}>
                        {p.links.map((l) => (
                          <Out key={l.url} link={l} />
                        ))}
                      </span>
                    )}
                    {p.warn && (
                      <p className={styles.warn} role="note">
                        ! {p.warn}
                      </p>
                    )}
                  </li>
                ))}
              </ul>
            )}
            {g.wiring && (
              <figure className={styles.wiring}>
                <pre role="img" aria-label={g.wiring.description}>
                  {g.wiring.diagram}
                </pre>
                <figcaption>
                  <ol>
                    {g.wiring.steps.map((s) => (
                      <li key={s}>{s}</li>
                    ))}
                  </ol>
                </figcaption>
              </figure>
            )}
          </div>
        ))}
        <p className={styles.disclosure}>{AMAZON_DISCLOSURE}</p>
      </section>

      <section className={styles.section} aria-labelledby="community" data-fluid-quiet="">
        <h2 id="community" className={styles.title}>
          community builds
        </h2>
        <p className={styles.text}>Every board in the repo, straight from its board.json.</p>
        <ul className={styles.boards}>
          {boards.map((b) => (
            <li key={b.name}>
              <div className={styles.boardHead}>
                <span className={styles.partName}>
                  {b.url ? (
                    <a href={b.url} target="_blank" rel="noopener noreferrer">
                      {b.title}
                    </a>
                  ) : (
                    b.title
                  )}
                </span>
                <i className={styles.status}>{b.status}</i>
              </div>
              <small>
                {b.display}
                {b.extras && ` · ${b.extras}`}
              </small>
              {b.maintainer && <small>maintained by {b.maintainer}</small>}
              {b.buy.length > 0 && (
                <span className={styles.outs}>
                  {b.buy.map((l) => (
                    <Out key={l.href} link={{ label: l.label, url: l.href }} />
                  ))}
                </span>
              )}
              {b.photos.length > 0 && (
                <span className={styles.photos}>
                  {b.photos.map((p) => (
                    <img key={p.src} src={p.src} alt={p.alt} loading="lazy" width={160} height={160} />
                  ))}
                </span>
              )}
            </li>
          ))}
          <li>
            <div className={styles.boardHead}>
              <span className={styles.partName}>yours</span>
              <a href={SUBMIT_HREF}>[ submit your board → ]</a>
            </div>
            <small>port it, prove it on video, open a pull request</small>
          </li>
        </ul>
      </section>

      <section className={styles.section} aria-labelledby="waitlist" data-fluid-quiet="">
        <div className={styles.box}>
          <h2 id="waitlist" className={styles.title}>
            {waitlistCopy.title}
          </h2>
          <p className={styles.text}>{waitlistCopy.lede}</p>
          {waitlist}
        </div>
      </section>

      <footer className={styles.footer}>
        <a href="/#boards">[ ← bake ]</a>
        <a href="/setup/">[ flash from the browser → ]</a>
        <a href={PORTING_HREF}>[ porting guide → ]</a>
        <a href={REPO_URL} target="_blank" rel="noopener noreferrer">
          [ source ↗ ]
        </a>
      </footer>
    </div>
  );
}
