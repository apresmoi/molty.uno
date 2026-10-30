import type { ReactNode } from "react";
import { SITE_LINKS, isSiteLinkActive } from "../../config/navigation";
import { REPO_URL } from "../../config/site.mjs";
import { SiteTitle } from "../SiteTitle";
import styles from "./SiteHeader.module.css";
export interface SiteHeaderProps {
  pathname: string;
  search?: ReactNode;
  theme?: ReactNode;
}
export function SiteHeader({ pathname, search, theme }: SiteHeaderProps) {
  return (
    <div className={styles.header}>
      <SiteTitle />
      <nav className={styles.navigation} aria-label="Site sections">
        {SITE_LINKS.map((link) => {
          const active = isSiteLinkActive(pathname, link.href);
          return (
            <a
              key={link.href}
              href={link.href}
              className={"mobileHidden" in link ? styles.mobileHidden : undefined}
              aria-current={active ? "page" : undefined}
            >
              <span aria-hidden="true">{active ? "[ " : "> "}</span>
              {link.label}
              <span aria-hidden="true">{active ? " ]" : ""}</span>
            </a>
          );
        })}
      </nav>
      {search && <div className={styles.search}>{search}</div>}
      {theme && <div className={styles.theme}>{theme}</div>}
      <a className={styles.repository} href={REPO_URL} target="_blank" rel="noopener noreferrer">
        [ source ↗ ]
      </a>
    </div>
  );
}
