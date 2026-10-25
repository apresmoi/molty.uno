import type { ReactNode } from "react";
import { SITE_LINKS, isSiteLinkActive } from "../../config/navigation";
import { REPO_URL } from "../../config/site.mjs";
import { SiteTitle } from "../SiteTitle";
import styles from "./SiteHeader.module.css";
export interface SiteHeaderProps {
  pathname: string;
  search?: ReactNode;
}
export function SiteHeader({ pathname, search }: SiteHeaderProps) {
  return (
    <div className={styles.header}>
      <SiteTitle />
      <nav className={styles.navigation} aria-label="Site sections">
        {SITE_LINKS.map((link) => (
          <a
            key={link.href}
            href={link.href}
            className={"mobileHidden" in link ? styles.mobileHidden : undefined}
            aria-current={isSiteLinkActive(pathname, link.href) ? "page" : undefined}
          >
            {link.label}
          </a>
        ))}
      </nav>
      {search && <div className={styles.search}>{search}</div>}
      <a className={styles.repository} href={REPO_URL} target="_blank" rel="noopener noreferrer">
        GitHub ↗
      </a>
    </div>
  );
}
