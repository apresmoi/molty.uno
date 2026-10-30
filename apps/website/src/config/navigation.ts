// Header links follow the landing's order (talk · moltys · bridge · bake), with /showcase/
// (every Molty and its actions) after moltys, then the two places people go next. /setup/ is
// the browser flasher (owned by the setup page's branch); /build/ is the parts list,
// community boards and the waitlist (desktop only: the phone header keeps setup and docs).
export const SITE_LINKS = [
  { href: "/#talk", label: "talk", mobileHidden: true },
  { href: "/#moltys", label: "moltys", mobileHidden: true },
  { href: "/showcase/", label: "showcase", mobileHidden: true },
  { href: "/#bridge", label: "bridge", mobileHidden: true },
  { href: "/#boards", label: "bake", mobileHidden: true },
  { href: "/build/", label: "build", mobileHidden: true },
  { href: "/setup/", label: "setup" },
  { href: "/getting-started/introduction/", label: "docs" },
] as const;

export function isSiteLinkActive(pathname: string, href: string): boolean {
  if (href.startsWith("/#")) return false;
  if (href === "/setup/") return pathname.startsWith("/setup");
  if (href === "/showcase/") return pathname.startsWith("/showcase");
  if (href === "/build/") return pathname.startsWith("/build");
  return (
    pathname !== "/" &&
    pathname !== "" &&
    !pathname.startsWith("/setup") &&
    !pathname.startsWith("/showcase") &&
    !pathname.startsWith("/build")
  );
}
