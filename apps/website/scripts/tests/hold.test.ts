// A device below the fold (src/features/device/hold.ts) never begins to appear before its
// picture is ready, short of the cap; then it appears once, at once. Run: pnpm test.
import { test, mock } from "node:test";
import assert from "node:assert/strict";
import { HOLD_S, holdTurn } from "../../src/features/device/hold.ts";

mock.timers.enable({ apis: ["setTimeout"] });

test("well in view, it waits for its picture and turns in the moment it is ready", () => {
  let went = 0;
  const hold = holdTurn(() => went++);
  hold.want();
  mock.timers.tick(HOLD_S * 1000 - 1);
  assert.equal(went, 0, "no picture yet, under the cap: still unseen");
  hold.ready();
  assert.equal(went, 1);
  mock.timers.tick(HOLD_S * 1000);
  hold.ready();
  hold.want();
  assert.equal(went, 1, "once only");
});

test("a picture ready before the device is in view: it turns in as soon as it is wanted", () => {
  let went = 0;
  const hold = holdTurn(() => went++);
  hold.ready();
  assert.equal(went, 0, "not wanted yet: it does not appear");
  hold.want();
  assert.equal(went, 1);
});

test("at the cap it turns in without its picture, once", () => {
  let went = 0;
  const hold = holdTurn(() => went++);
  hold.want();
  mock.timers.tick(HOLD_S * 1000);
  assert.equal(went, 1);
  hold.ready();
  assert.equal(went, 1);
});

test("gone (unmounted) before any of it: it never turns in", () => {
  let went = 0;
  const hold = holdTurn(() => went++);
  hold.want();
  hold.cancel();
  mock.timers.tick(HOLD_S * 1000);
  hold.ready();
  assert.equal(went, 0);
});
