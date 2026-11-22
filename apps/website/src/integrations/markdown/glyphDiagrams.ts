// Diagrams in the docs are written as ```mermaid blocks (GitHub draws those natively) and drawn
// here, at build time, by @glyphcss/diagrams: each block becomes a <figure> of plain text at two
// widths (starlight.css shows the one that fits). Nothing ships to the browser but that text:
// no script, no font, no request. A source that cannot be drawn fails the build.
import { renderGlyphDiagram } from "@glyphcss/diagrams";
import { renderGlyphSequence } from "@glyphcss/diagrams/sequence";

/** Columns of the two renders: the docs' content column, and a 390 px phone. */
export const DIAGRAM_WIDTHS = { wide: 78, narrow: 44 } as const;
/** A render this narrow already fits a phone: it is the only one sent. */
export const FITS_A_PHONE = 48;

export const columns = (text: string) => Math.max(...text.split("\n").map((row) => [...row].length));
/** Ledger codes that mean the drawing lost or mangled something: the build stops on them. */
const BROKEN =
  /^(unroutable|layout-overflow|sequence-layout-overflow|simplified|render-work-limited|route-conflict|label-dropped|label-abbreviated|word-cut)$/;
const ACC = /^\s*acc(Title|Descr)\s*:\s*(.*)$/;
const LINES = /[\u2500-\u257f]+/g;
const HEADS = /[\u25b6\u25c0\u25b2\u25bc]/g;

export interface GlyphDiagram {
  title: string;
  wide: string;
  narrow: string;
  codes: string[];
}

const escape = (s: string) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");

/** The render as the page shows it: no blank edges, no common indent. */
function trim(text: string): string {
  const rows = text.split("\n").map((r) => r.replace(/\s+$/, ""));
  while (rows.length && !rows[0]) rows.shift();
  while (rows.length && !rows[rows.length - 1]) rows.pop();
  const indent = Math.min(...rows.filter(Boolean).map((r) => r.length - r.trimStart().length));
  return rows.map((r) => r.slice(indent)).join("\n");
}

async function draw(source: string, width: number): Promise<{ text: string; codes: string[] }> {
  const options = { target: "web", charset: "box", color: "none", width, height: 8, overflow: "expand" } as const;
  const result = /^\s*sequenceDiagram\b/.test(source)
    ? await renderGlyphSequence(source, options)
    : await renderGlyphDiagram(source, options);
  return { text: trim(result.text), codes: (result.report?.ledger ?? []).map((entry) => entry.code) };
}

/** Mermaid source to its two text renders. `accTitle:` names it for screen readers (Mermaid's own
 *  line, kept out of the drawing). Throws when the source is not drawable or loses an edge. */
export async function glyphDiagram(source: string): Promise<GlyphDiagram> {
  let title = "Diagram";
  const body = source
    .split("\n")
    .filter((line) => {
      const acc = ACC.exec(line);
      if (acc?.[1] === "Title" && acc[2]) title = acc[2].trim();
      return !acc;
    })
    .join("\n");
  const [wide, narrow] = await Promise.all([draw(body, DIAGRAM_WIDTHS.wide), draw(body, DIAGRAM_WIDTHS.narrow)]);
  const codes = [...new Set([...wide.codes, ...narrow.codes])];
  const broken = codes.filter((code) => BROKEN.test(code));
  if (broken.length) throw new Error(`diagram "${title}" does not fit or lost a connection (${broken.join(", ")})`);
  return { title, wide: wide.text, narrow: narrow.text, codes };
}

/** Lines dim, arrowheads in the accent, words in the ink (starlight.css). */
const paint = (text: string) =>
  escape(text)
    .replace(LINES, (run) => `<i>${run}</i>`)
    .replace(HEADS, (head) => `<b>${head}</b>`);

export function glyphFigure(diagram: GlyphDiagram): string {
  const pre = (kind: "wide" | "narrow", only = false) =>
    `<pre${only ? "" : ` class="glyph-diagram-${kind}"`} style="--cols:${columns(diagram[kind])}" aria-hidden="true">${paint(diagram[kind])}</pre>`;
  const one = columns(diagram.wide) <= FITS_A_PHONE || diagram.wide === diagram.narrow;
  const body = one ? pre("wide", true) : pre("wide") + pre("narrow");
  return `<figure class="glyph-diagram not-content" role="img" aria-label="${escape(diagram.title).replace(/"/g, "&quot;")}"><div>${body}</div></figure>`;
}

interface Node {
  type: string;
  lang?: string | null;
  value?: string;
  children?: Node[];
}

/** remark plugin: every ```mermaid block becomes its glyphcss figure. */
export function remarkGlyphDiagrams() {
  return async (tree: Node, file: { path?: string }) => {
    const blocks: Node[] = [];
    const walk = (node: Node) => {
      if (node.type === "code" && node.lang === "mermaid") blocks.push(node);
      node.children?.forEach(walk);
    };
    walk(tree);
    for (const block of blocks) {
      try {
        const html = glyphFigure(await glyphDiagram(block.value ?? ""));
        Object.assign(block, { type: "html", value: html, lang: undefined });
      } catch (error) {
        throw new Error(`${file.path ?? "markdown"}: ${(error as Error).message}`);
      }
    }
  };
}
