// The landing's skills wall (src/features/molty/skills.ts) shows only what is real: every screen
// is a skill in skills/, asked for by a script that skill ships, playing a screen its package
// draws (from the firmware's own sample commands), and the heading takes the astrology skill's
// own sky blue. The queued scenes are the ones the sample commands queue together.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { sampleQueue, skillScreens } from "../../src/features/molty/skills.ts";
import { WORDS } from "../../src/config/words.mjs";

const ROOT = fileURLToPath(new URL("../../../../", import.meta.url));
const read = (path: string) => readFileSync(join(ROOT, path), "utf8");
const commands = read("device/firmware/test/host/fixtures/astrology-commands.jsonl");
const layouts = Object.keys(JSON.parse(read("packages/astrology/astrology.json")).layouts);
const display = read("skills/molty-astro/display.yaml");

test("every screen on the wall is a real skill's, asked for by its own script", () => {
  const screens = skillScreens(commands);
  assert.equal(screens[0]!.name, "chart", "the chart comes first");
  for (const s of screens) {
    assert.ok(existsSync(join(ROOT, "skills", s.skill, "SKILL.md")), `${s.name}: skills/${s.skill} is a skill`);
    assert.ok(existsSync(join(ROOT, "skills", s.skill, "scripts", s.script)), `${s.name}: ${s.skill} ships ${s.script}`);
    assert.ok(s.scene || s.play, `${s.name} shows something`);
  }
});

test("every scene on the wall is one the astrology package draws and its skill names", () => {
  for (const s of skillScreens(commands)) {
    const played = [s.scene?.layout, ...(s.play?.kind === "scenes" ? s.play.scenes.map((x) => x.layout) : [])];
    for (const layout of played.filter(Boolean) as string[]) {
      assert.ok(layouts.includes(layout), `${s.name}: the package has no layout ${layout}`);
      assert.match(display, new RegExp(`^  ${layout}: `, "m"), `${s.name}: molty-astro's display.yaml has no screen ${layout}`);
    }
  }
});

test("a queue is the scene the samples play and the ones queued right behind it", () => {
  const q = sampleQueue(commands, "globe");
  assert.equal(q[0]!.params.img, "moon");
  assert.deepEqual(
    q.map((a) => a.layout),
    ["globe", "globe", "globe", "globe", "globe", "saturn", "sign"],
  );
  assert.throws(() => sampleQueue(commands, "nothing"));
});

test("the skills heading takes the astrology skill's sky blue", () => {
  const sky = /^\s+sky: "(#[0-9a-f]{6})"/m.exec(display)![1];
  assert.equal(WORDS.find((w) => w[0] === "skills")![2], sky);
});
