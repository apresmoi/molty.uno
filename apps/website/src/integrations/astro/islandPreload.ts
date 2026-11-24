// Module preloads for the islands a page hydrates at once (client:load). Astro asks for an
// island's code only when its inline script runs, after the stylesheets, and for each import only
// once its importer has arrived: a round of requests each. Listed in the page's head, the whole
// graph downloads with the stylesheets instead. Built pages only, from the build's own files;
// dynamic imports stay lazy.
import type { AstroIntegration } from "astro";
import { readFile, readdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ISLAND = /<astro-island\b[^>]*\bclient="load"[^>]*>/g;
const URLS = /\b(?:component|renderer)-url="([^"]+)"/g;
/** A static import or re-export of a sibling chunk (`import"./a.js"`, `from"./a.js"`). */
const STATIC = /(?:\bimport|\bfrom)\s*"(\.{1,2}\/[^"]+\.js)"/g;

/** Every module the page's client:load islands import statically, in the order they are found. */
export async function islandModules(html: string, read: (path: string) => Promise<string>): Promise<string[]> {
  const found = new Set<string>();
  for (const [tag] of html.matchAll(ISLAND)) for (const [, url] of tag.matchAll(URLS)) found.add(url!);
  for (const path of found)
    for (const [, spec] of (await read(path)).matchAll(STATIC)) found.add(new URL(spec!, `http://x${path}`).pathname);
  return [...found];
}

export function islandPreload(): AstroIntegration {
  return {
    name: "molty-island-preload",
    hooks: {
      "astro:build:done": async ({ dir }) => {
        const root = fileURLToPath(dir);
        const pages = (await readdir(root, { recursive: true })).filter((f) => f.endsWith(".html"));
        const read = (path: string) => readFile(join(root, path), "utf8");
        for (const page of pages) {
          const html = await readFile(join(root, page), "utf8");
          const modules = await islandModules(html, read);
          if (!modules.length) continue;
          const links = modules.map((m) => `<link rel="modulepreload" href="${m}">`).join("");
          await writeFile(join(root, page), html.replace("</head>", `${links}</head>`));
        }
      },
    },
  };
}
