// The site's Molty is the firmware's renderer (device/firmware/web/built/molty.wasm, built by
// `make web-molty` from device/firmware/main and checked there, frame by frame, against a native
// build of the same sources). Here, with Node alone: the committed module is the one its
// manifest describes, the sources it was built from have not moved on without it, and it still
// draws the frames it was checked on. The still and the heading colour come from it too.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import sharp from "sharp";
import { listing } from "../../../../tools/web-molty/fidelity.mjs";
import { STILL, renderStill } from "../molty-still.mjs";
import { WORDS } from "../../src/config/words.mjs";
import { landingActions, sampleScene, showcaseActions, showcaseScenes, type CreaturePackage } from "../../src/features/molty/actions.ts";

const ROOT = fileURLToPath(new URL("../../../../", import.meta.url));
const read = (path: string) => readFileSync(join(ROOT, path));
const sha = (data: Buffer | string) => createHash("sha256").update(data).digest("hex");
const manifest = JSON.parse(read("device/firmware/web/built/molty.json").toString());
const wasm = read("device/firmware/web/built/molty.wasm");
const cat = JSON.parse(read("packages/cat/cat.json").toString()) as CreaturePackage & { creature: { rig: { palette: { body: string } } } };
const stale = "run `make web-molty` (docs/WEB-MOLTY-REPORT.md)";

test("the committed module is the one its manifest describes", () => {
  assert.equal(sha(wasm), manifest.wasm.sha256, `built/molty.wasm changed without its manifest: ${stale}`);
  assert.equal(wasm.length, manifest.wasm.bytes);
});

test("the firmware sources have not moved on without the module", () => {
  const h = createHash("sha256");
  for (const f of manifest.sources.files as string[]) h.update(f).update("\0").update(read(f)).update("\0");
  assert.equal(h.digest("hex"), manifest.sources.sha256, `a source of the web Molty changed since it was built: ${stale}`);
});

test("the module still draws the frames it was checked on against the native build", async () => {
  const cwd = process.cwd();
  process.chdir(ROOT); // the script's paths
  try {
    const frames = await listing(wasm, read(manifest.fidelity.script).toString());
    assert.equal(frames.trimEnd().split("\n").length, manifest.fidelity.frames);
    assert.equal(sha(frames), manifest.fidelity.sha256, `the fidelity script, or a package it plays, changed: ${stale}`);
  } finally {
    process.chdir(cwd);
  }
});

test("the still is a frame of the module (node scripts/molty-still.mjs)", async () => {
  const still = await sharp(STILL).ensureAlpha().raw().toBuffer();
  assert.ok(still.equals(await renderStill()), "src/assets/molty/cat.webp is not what the module draws: node scripts/molty-still.mjs");
});

test("the cat's headings take its own body colour", () => {
  for (const name of ["moltys", "showcase"]) assert.equal(WORDS.find((w) => w[0] === name)![2], cat.creature.rig.palette.body);
});

test("the stage's lists are the package's own: every mood, every gesture, its words for each", () => {
  const commands = read("device/firmware/test/host/fixtures/astrology-commands.jsonl").toString();
  const all = showcaseActions(cat, showcaseScenes(commands)).flatMap((g) => g.actions);
  for (const mood of Object.keys(cat.creature.moods)) assert.ok(all.some((a) => a.kind === "mood" && a.name === mood), mood);
  for (const g of Object.keys(cat.creature.gestures))
    assert.ok(all.some((a) => (g === "shaken" ? a.kind === "shake" : a.kind === "act" && a.name === g)), g);
  for (const a of all) assert.ok(a.about.length > 10, `${a.name} is described`);
  const own = landingActions(cat).find((g) => g.label === "its own")!.actions.map((a) => a.name);
  assert.deepEqual(own, ["eat", "yarn", "walk", "shake"]);
  assert.equal(sampleScene(commands, "chart").layout, "chart");
});
