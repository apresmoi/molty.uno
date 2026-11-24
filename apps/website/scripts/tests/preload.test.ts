// The islands a page hydrates at once have their whole static import graph preloaded
// (src/integrations/astro/islandPreload.ts); dynamic imports and later islands stay lazy.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { islandModules } from "../../src/integrations/astro/islandPreload.ts";

const FILES: Record<string, string> = {
  "/_astro/client.A.js": 'import{a}from"./react.B.js";export{a};',
  "/_astro/react.B.js": "export const a=1;",
  "/_astro/Word.C.js": 'import{a as b}from"./react.B.js";import"./side.D.js";const l=()=>import("./lazy.E.js");',
  "/_astro/side.D.js": 'export{x}from"../_astro/deep.F.js";',
  "/_astro/deep.F.js": "export const x=2;",
  "/_astro/Idle.G.js": 'import"./idle.H.js";',
};
const read = async (path: string) => FILES[path] ?? assert.fail(`read ${path}`);
const island = (url: string, client: string) =>
  `<astro-island uid="u" component-url="${url}" renderer-url="/_astro/client.A.js" ssr client="${client}">`;

test("a page's client:load islands: their static graph, once each; dynamic imports and later islands not", async () => {
  const html = `<html><head></head><body>${island("/_astro/Word.C.js", "load")}${island("/_astro/Word.C.js", "load")}${island("/_astro/Idle.G.js", "idle")}</body></html>`;
  assert.deepEqual((await islandModules(html, read)).sort(), [
    "/_astro/Word.C.js",
    "/_astro/client.A.js",
    "/_astro/deep.F.js",
    "/_astro/react.B.js",
    "/_astro/side.D.js",
  ]);
  assert.deepEqual(await islandModules("<html><head></head><body>no islands</body></html>", read), []);
});
