// Header links follow the landing's order (fluid · moltys · bridge · boards), then the two
// places people go next. /setup/ is the browser flasher (owned by the setup page's branch).
export const SITE_LINKS = [
  { href: "/#fluid", label: "fluid", mobileHidden: true },
  { href: "/#moltys", label: "moltys", mobileHidden: true },
  { href: "/#bridge", label: "bridge", mobileHidden: true },
  { href: "/#boards", label: "boards", mobileHidden: true },
  { href: "/setup/", label: "setup" },
  { href: "/getting-started/introduction/", label: "docs" },
] as const;

export function isSiteLinkActive(pathname: string, href: string): boolean {
  if (href.startsWith("/#")) return false;
  if (href === "/setup/") return pathname.startsWith("/setup");
  return pathname !== "/" && pathname !== "" && !pathname.startsWith("/setup");
}
