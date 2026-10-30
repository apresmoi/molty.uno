import { describe, expect, it } from "vitest";
import { CommandError, DEFAULT_LISTEN_MS, LISTEN_CEILING_MS, TALK_DEFAULT_LISTEN_MS, validateCommand, validateTalk } from "../src/core/commands.js";
import { MAX_LISTEN_BYTES } from "../src/core/protocol.js";

const code = (f: () => unknown) => {
  try {
    f();
  } catch (e) {
    return (e as CommandError).code;
  }
  return "ok";
};

describe("validateCommand", () => {
  it("applies defaults", () => {
    expect(validateCommand("show", { text: "hi" }).args).toEqual({ text: "hi", wpm: 300 });
    // no ms: none sent, the device applies its own setting (30 s out of the box)
    expect(validateCommand("listen", {}).args).toEqual({});
    expect(validateCommand("listen", undefined).job).toBe(true);
    expect(validateCommand("say", { text: "x", voice: "luna" })).toEqual({ name: "say", args: { text: "x", voice: "luna" }, job: true });
  });
  it("listen window: 1..30000, 0 = unlimited, absent = the device's setting; talk's listen_ms the same", () => {
    expect(validateCommand("listen", { ms: 30000 }).args).toEqual({ ms: 30000 });
    expect(validateCommand("listen", { ms: 1 }).args).toEqual({ ms: 1 });
    expect(validateCommand("listen", { ms: 0 }).args).toEqual({ ms: 0 });
    expect(validateTalk({ listen_ms: 30000 }).listen_ms).toBe(30000);
    expect(validateTalk({ listen_ms: 0 }).listen_ms).toBe(0);
    expect(validateTalk({}).listen_ms).toBeUndefined();
    expect(code(() => validateTalk({ listen_ms: 30001 }))).toBe("invalid_args");
    expect(code(() => validateTalk({ listen_ms: -1 }))).toBe("invalid_args");
    expect(DEFAULT_LISTEN_MS).toBe(30000);
    expect(TALK_DEFAULT_LISTEN_MS).toBe(30000);
    expect(MAX_LISTEN_BYTES).toBeGreaterThanOrEqual((LISTEN_CEILING_MS + 1000) * 32);
  });
  it("state: empty = read, {state:{}} or flat = patch (flat to device)", () => {
    expect(validateCommand("state", {}).args).toEqual({});
    expect(validateCommand("state", { state: { blink: 1 } }).args).toEqual({ blink: 1 });
    expect(validateCommand("state", { blink: 1, x: 0 }).args).toEqual({ blink: 1, x: 0 });
    expect(code(() => validateCommand("state", { state: 5 }))).toBe("invalid_args");
  });
  it("diag/log: optional bounded ints, forwarded as given", () => {
    expect(validateCommand("diag", {})).toEqual({ name: "diag", args: {}, job: false });
    expect(validateCommand("diag", { timeline: 60 }).args).toEqual({ timeline: 60 });
    expect(code(() => validateCommand("diag", { timeline: 181 }))).toBe("invalid_args");
    expect(code(() => validateCommand("diag", { x: 1 }))).toBe("invalid_args");
    expect(validateCommand("log", { since: 12, max: 4096 }).args).toEqual({ since: 12, max: 4096 });
    expect(validateCommand("log", {}).args).toEqual({});
    expect(code(() => validateCommand("log", { max: 100 }))).toBe("invalid_args");
    expect(code(() => validateCommand("log", { since: -1 }))).toBe("invalid_args");
  });
  it("mood: optional hold_s, a finite number 0 <= hold_s <= 3600 (0 = until the next mood, as the firmware reads it), forwarded only when given", () => {
    expect(validateCommand("mood", { mood: "happy" }).args).toEqual({ mood: "happy" });
    expect(validateCommand("mood", { mood: "happy", hold_s: 2.5 }).args).toEqual({ mood: "happy", hold_s: 2.5 });
    expect(validateCommand("mood", { mood: "thinking", hold_s: 3600 }).args).toEqual({ mood: "thinking", hold_s: 3600 });
    expect(validateCommand("mood", { mood: "thinking", hold_s: 0 }).args).toEqual({ mood: "thinking", hold_s: 0 });
    for (const hold_s of [-1, 3600.5, Number.NaN, Number.POSITIVE_INFINITY, "3", null]) {
      expect(code(() => validateCommand("mood", { mood: "happy", hold_s })), String(hold_s)).toBe("invalid_args");
    }
  });
  it("rejects bad input", () => {
    expect(code(() => validateCommand("reboot", {}))).toBe("unknown_command");
    expect(code(() => validateCommand("mood", { mood: "angry" }))).toBe("invalid_args");
    expect(code(() => validateCommand("mood", { mood: "happy", extra: 1 }))).toBe("invalid_args");
    expect(code(() => validateCommand("volume", { volume: 22 }))).toBe("invalid_args");
    expect(code(() => validateCommand("volume", { volume: 1.5 }))).toBe("invalid_args");
    expect(code(() => validateCommand("show", { text: "" }))).toBe("invalid_args");
    expect(code(() => validateCommand("show", { text: "x".repeat(16385) }))).toBe("invalid_args");
    expect(code(() => validateCommand("show", { text: "a\u0000" }))).toBe("invalid_args");
    expect(code(() => validateCommand("listen", { ms: 30001 }))).toBe("invalid_args");
    for (const ms of [-1, 1.5, "30000", null]) expect(code(() => validateCommand("listen", { ms })), String(ms)).toBe("invalid_args");
    expect(code(() => validateCommand("say", { text: "x", voice: "../x" }))).toBe("invalid_args");
    expect(code(() => validateCommand("status", { a: 1 }))).toBe("invalid_args");
    expect(code(() => validateCommand("mood", { mood: null }))).toBe("invalid_args");
  });
});
