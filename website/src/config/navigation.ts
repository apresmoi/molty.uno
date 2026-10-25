export const SITE_LINKS = [
  { href: "/#moltys", label: "Moltys" },
  { href: "/#agents", label: "Agents", mobileHidden: true },
  { href: "/#build", label: "Build", mobileHidden: true },
  { href: "/getting-started/introduction/", label: "Docs" },
] as const;
export function isSiteLinkActive(pathname: string, href: string): boolean {
  if (href.startsWith("/#")) return false;
  return pathname !== "/" && pathname !== "";
}
