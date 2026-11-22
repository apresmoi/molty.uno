// The docs' diagrams: every ```mermaid block in the files people read draws cleanly with the
// site's own renderer at both widths, and no hand-drawn diagram is left beside them.
import assert from "node:assert/strict";
import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";
import { SYNCED_DOCS } from "../../src/config/docs.mjs";
import { DIAGRAM_WIDTHS, columns, glyphDiagram, glyphFigure } from "../../src/integrations/markdown/glyphDiagrams.ts";

const ROOT = fileURLToPath(new URL("../../../../", import.meta.url));
const walk = (dir: string): string[] =>
  readdirSync(join(ROOT, dir)).flatMap((name) => {
    const path = join(dir, name);
    return statSync(join(ROOT, path)).isDirectory() ? (name === "repo" ? [] : walk(path)) : path.endsWith(".md") ? [path] : [];
  });
const FILES = [
  ...new Set([
    ...walk("docs").filter((f) => !f.endsWith("DOCS-UPDATE-REPORT.md")),
    "README.md",
    "CONTRIBUTING.md",
    "device/creatures/MOLTY-FORMAT.md",
    ...SYNCED_DOCS.map((d) => d.source),
    ...walk("apps/website/src/content/docs"),
    ...walk("skills"),
  ]),
].filter((f) => existsSync(join(ROOT, f)));

/** A phone shows the narrow drawing down to 9.5 px type: this many columns still fit 390 px. */
const PHONE_COLUMNS = 58;
const blocks = FILES.flatMap((file) =>
  [...readFileSync(join(ROOT, file), "utf8").matchAll(/^```mermaid\n([\s\S]*?)^```/gm)].map((m, i) => ({ file, n: i + 1, source: m[1]! })),
);

test("the docs have diagrams to draw", () => {
  assert.ok(blocks.length >= 14, `found ${blocks.length}`);
});

for (const { file, n, source } of blocks)
  test(`${file} diagram ${n} draws cleanly and fits`, async () => {
    const diagram = await glyphDiagram(source);
    assert.notEqual(diagram.title, "Diagram", "give it an accTitle: line (what a screen reader says)");
    assert.ok(columns(diagram.wide) <= DIAGRAM_WIDTHS.wide, `wide render is ${columns(diagram.wide)} columns`);
    assert.ok(columns(diagram.narrow) <= PHONE_COLUMNS, `narrow render is ${columns(diagram.narrow)} columns`);
    const html = glyphFigure(diagram);
    assert.match(html, /^<figure class="glyph-diagram not-content" role="img" aria-label="[^"]+">/);
    assert.ok(!/<script|style="(?!--cols:\d+")/.test(html), "the figure is text and one number, nothing else");
  });

test("a source that cannot be drawn stops the build", async () => {
  await assert.rejects(glyphDiagram("sequenceDiagram\n  this is not a message"));
});

test("no hand-drawn diagram is left in the docs", () => {
  // box-drawing and arrow glyphs outside directory listings; mermaid source has none of them
  const drawn = /[─│┌┐└┘├┤┬┴┼▶▼◀▲╌╎]|\+--|-{3,}>|<-{3,}/;
  const listing = /^[│ ]*([├└]── |│ {2,})/;
  const left = FILES.flatMap((file) =>
    readFileSync(join(ROOT, file), "utf8")
      .split("\n")
      .flatMap((line, i) => (drawn.test(line) && !listing.test(line) ? [`${file}:${i + 1}: ${line}`] : [])),
  );
  assert.deepEqual(left, []);
});
