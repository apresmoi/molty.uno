import { describe, expect, it } from "vitest";
import { CommandError, validateCommand } from "../src/core/commands.js";

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
    expect(validateCommand("listen", {}).args).toEqual({ ms: 4000 });
    expect(validateCommand("listen", undefined).job).toBe(true);
    expect(validateCommand("say", { text: "x", voice: "luna" })).toEqual({ name: "say", args: { text: "x", voice: "luna" }, job: true });
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
  it("rejects bad input", () => {
    expect(code(() => validateCommand("reboot", {}))).toBe("unknown_command");
    expect(code(() => validateCommand("mood", { mood: "angry" }))).toBe("invalid_args");
    expect(code(() => validateCommand("mood", { mood: "happy", extra: 1 }))).toBe("invalid_args");
    expect(code(() => validateCommand("volume", { volume: 22 }))).toBe("invalid_args");
    expect(code(() => validateCommand("volume", { volume: 1.5 }))).toBe("invalid_args");
    expect(code(() => validateCommand("show", { text: "" }))).toBe("invalid_args");
    expect(code(() => validateCommand("show", { text: "x".repeat(16385) }))).toBe("invalid_args");
    expect(code(() => validateCommand("show", { text: "a\u0000" }))).toBe("invalid_args");
    expect(code(() => validateCommand("listen", { ms: 10001 }))).toBe("invalid_args");
    expect(code(() => validateCommand("say", { text: "x", voice: "../x" }))).toBe("invalid_args");
    expect(code(() => validateCommand("status", { a: 1 }))).toBe("invalid_args");
    expect(code(() => validateCommand("mood", { mood: null }))).toBe("invalid_args");
  });
});
