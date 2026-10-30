// /build/ against its sources of truth: the requirements box agrees with docs/PORTING.md,
// community builds come from the real board.json files, and every Amazon link carries the
// Associates tag (one helper, one constant). Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { AMAZON_TAG, amazonSearch, shopLink } from "../../src/config/affiliate.mjs";
import { requirements, niceToHave, partGroups, readWaitlistResult, waitlistMessage } from "../../src/features/build/content.ts";
import { communityBoards, type BoardJson } from "../../src/features/build/boards.ts";

const ROOT = fileURLToPath(new URL("../../../../", import.meta.url));
const SRC = fileURLToPath(new URL("../../src/", import.meta.url));
const porting = readFileSync(join(ROOT, "docs/PORTING.md"), "utf8");

/** The markdown table under `## <heading>`: first cell → the row's cells. */
function table(heading: string): Map<string, string[]> {
  const section = porting.split(/^## /m).find((s) => s.startsWith(`${heading}\n`));
  assert.ok(section, `docs/PORTING.md has no "## ${heading}"`);
  const rows = new Map<string, string[]>();
  for (const line of section.split("\n")) {
    if (!line.startsWith("|") || /^\|\s*-/.test(line) || /^\|\s*\|/.test(line)) continue;
    const cells = line.split("|").slice(1, -1).map((c) => c.trim());
    rows.set(cells[0]!.replace(/`/g, "").replace(/\s*\(required\)/, ""), cells.slice(1));
  }
  return rows;
}

test("'Will my board work?' needs match docs/PORTING.md Requirements", () => {
  const reqs = table("Requirements");
  assert.deepEqual(new Set(requirements.map((r) => r.row)), new Set(reqs.keys()), "one box row per Requirements row");
  for (const r of requirements) {
    const needed = reqs.get(r.row)![0]!;
    assert.ok(needed.includes(r.need), `${r.row}: "${r.need}" not in PORTING's "${needed}"`);
    // what the box adds in its note must be in PORTING too (e.g. "8 MB recommended")
    const recommended = /\d+ MB recommended/.exec(r.note ?? "")?.[0];
    if (recommended) assert.ok(needed.includes(recommended), `${r.row}: PORTING lacks "${recommended}"`);
  }
  assert.match(reqs.get("Chip")!.join(" "), /C3, C6 and the classic ESP32: not yet/);
});

test("nice-to-haves are PORTING Capabilities and firmware-boards.py caps", () => {
  const caps = table("Capabilities");
  const known = /CAPS\s*=\s*\(([^)]*)\)/.exec(readFileSync(join(ROOT, "tools/scripts/firmware-boards.py"), "utf8"))?.[1] ?? "";
  for (const n of niceToHave) {
    assert.ok(caps.has(n.cap), `${n.cap} is not in PORTING's Capabilities table`);
    assert.ok(known.includes(`"${n.cap}"`), `${n.cap} is not a firmware-boards.py cap`);
  }
});

test("shopLink tags Amazon (and only Amazon) with AMAZON_TAG, rel sponsored", () => {
  const a = shopLink(amazonSearch("ESP32-S3-Touch-LCD-1.46"));
  assert.equal(a.href, `https://www.amazon.de/s?k=ESP32-S3-Touch-LCD-1.46&tag=${AMAZON_TAG}`);
  assert.equal(a.rel, "sponsored noopener");
  assert.equal(shopLink("https://www.amazon.de/s?k=x&tag=someone-else-21").href, `https://www.amazon.de/s?k=x&tag=${AMAZON_TAG}`);
  assert.equal(new URL(shopLink(amazonSearch("1 kΩ resistor")).href).searchParams.get("k"), "1 kΩ resistor");
  const w = shopLink("https://www.waveshare.com/esp32-s3-touch-lcd-1.46b.htm");
  assert.equal(w.href, "https://www.waveshare.com/esp32-s3-touch-lcd-1.46b.htm");
  assert.equal(w.sponsored, false);
  assert.throws(() => shopLink("http://www.amazon.de/s?k=x"));
});

test("every part link to Amazon is an amazon.de search (never a product page that can die)", () => {
  const links = partGroups.flatMap((g) => g.parts.flatMap((p) => p.links ?? []));
  assert.ok(links.some((l) => l.url.includes("amazon.")));
  for (const l of links.filter((l) => l.url.includes("amazon."))) assert.match(l.url, /^https:\/\/www\.amazon\.de\/s\?k=/);
});

test("nothing in src spells the Associates tag or a tagged URL except config/affiliate.mjs", () => {
  const walk = (d: string): string[] => readdirSync(d, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? walk(join(d, e.name)) : [join(d, e.name)]));
  for (const f of walk(SRC).filter((f) => /\.(tsx?|mjs|astro)$/.test(f) && !f.includes("/content/docs/"))) {
    if (relative(SRC, f) === "config/affiliate.mjs") continue;
    const src = readFileSync(f, "utf8");
    assert.doesNotMatch(src, /[?&]tag=/, `${relative(SRC, f)} builds a tagged URL itself`);
    assert.ok(!src.includes(AMAZON_TAG), `${relative(SRC, f)} spells AMAZON_TAG`);
  }
});

// the real board.json files, as repoBoards.ts reads them through Vite
const BOARDS = join(ROOT, "device/firmware/boards");
function entries(): Array<{ dir: string; json: BoardJson }> {
  const out: Array<{ dir: string; json: BoardJson }> = [];
  const walk = (dir: string) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      if (!e.isDirectory()) continue;
      const sub = join(dir, e.name);
      if (existsSync(join(sub, "board.json"))) out.push({ dir: relative(BOARDS, sub), json: JSON.parse(readFileSync(join(sub, "board.json"), "utf8")) });
      else walk(sub);
    }
  };
  walk(BOARDS);
  return out;
}

test("community builds: every real board, no _template, reference first, Amazon buy links tagged", () => {
  const all = entries();
  assert.ok(all.some((e) => e.dir.startsWith("_")), "the template is in the walk (and must be skipped)");
  const boards = communityBoards(all, () => undefined);
  assert.equal(boards.length, all.filter((e) => !e.dir.startsWith("_")).length);
  assert.ok(!boards.some((b) => b.name === "myvendor-mymodel"));
  assert.equal(boards[0]!.status, "reference");
  const ref = boards.find((b) => b.name === "waveshare-esp32-s3-touch-lcd-1.46")!;
  assert.equal(ref.display, "412×412 round · SPD2010 (QSPI)");
  assert.ok(ref.buy.length >= 2, "the 1.46 lists where to buy it");
  for (const l of ref.buy.filter((l) => l.href.includes("amazon."))) {
    assert.equal(new URL(l.href).searchParams.get("tag"), AMAZON_TAG);
    assert.equal(l.rel, "sponsored noopener");
  }
});

test("community builds: photos resolve through the served URL, missing ones are dropped", () => {
  const json: BoardJson = {
    name: "acme-round",
    title: "Acme Round",
    display: { width: 240, height: 240, shape: "round" },
    status: "experimental",
    photos: ["device/firmware/boards/acme/round/a.jpg", "device/firmware/boards/acme/round/missing.jpg"],
  };
  const [b] = communityBoards([{ dir: "acme/round", json }], (p) => (p.endsWith("a.jpg") ? "/_astro/a.123.jpg" : undefined));
  assert.deepEqual(b!.photos, [{ src: "/_astro/a.123.jpg", alt: "Acme Round, photo 1" }]);
  assert.deepEqual(b!.buy, []);
});

test("waitlist: the no-JavaScript redirect's query maps to a state and a message", () => {
  assert.deepEqual(readWaitlistResult("?waitlist=ok"), { kind: "done" });
  assert.deepEqual(readWaitlistResult("?waitlist=error&reason=bad_email"), { kind: "error", reason: "bad_email" });
  assert.equal(readWaitlistResult("?m=x"), null);
  assert.match(waitlistMessage("bad_email"), /email address/);
  assert.match(waitlistMessage("<script>"), /Try again/);
});
