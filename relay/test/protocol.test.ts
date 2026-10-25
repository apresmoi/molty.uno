import { describe, expect, it } from "vitest";
import { b64Decode, b64Encode, b64urlEncode, randomLinkCode, safeEqual, sha256Hex } from "../src/core/crypto.js";
import { LINK_CODE_RE, MAX_AUDIO_PAYLOAD_BYTES, decodeAudioFrame, encodeAudioFrame, parseHello, parseTextFrame } from "../src/core/protocol.js";
import { jobDevice, tokenDevice, bearer } from "../src/core/api.js";

describe("audio frames", () => {
  const header = { s: "molty-k7m2q9xa4c.j_abcdefgh", seq: 3, kind: "pcm16" as const, rate: 16000, end: false };
  it("round-trips header + payload byte-exactly", () => {
    const payload = Uint8Array.from([0, 10, 255, 128, 10]);
    const frame = encodeAudioFrame(header, payload);
    const text = new TextDecoder().decode(frame.subarray(0, frame.indexOf(10)));
    expect(JSON.parse(text)).toEqual(header);
    const back = decodeAudioFrame(frame)!;
    expect(back.header).toEqual(header);
    expect(Array.from(back.payload)).toEqual(Array.from(payload));
  });
  it("accepts header-only frames and defaults end=false", () => {
    const f = new TextEncoder().encode(JSON.stringify({ s: "x", seq: 0, kind: "pcm16", rate: 16000, end: true }));
    expect(decodeAudioFrame(f)?.header.end).toBe(true);
    expect(decodeAudioFrame(f)?.payload.byteLength).toBe(0);
  });
  it("enforces the 16 KB payload and 512 B header caps", () => {
    expect(() => encodeAudioFrame(header, new Uint8Array(MAX_AUDIO_PAYLOAD_BYTES + 1))).toThrow();
    const big = new Uint8Array(600).fill(32);
    expect(decodeAudioFrame(big)).toBeUndefined();
    const okFrame = encodeAudioFrame(header, new Uint8Array(MAX_AUDIO_PAYLOAD_BYTES));
    expect(decodeAudioFrame(okFrame)).toBeDefined();
    const over = new Uint8Array(okFrame.byteLength + 1);
    over.set(okFrame);
    expect(decodeAudioFrame(over)).toBeUndefined();
  });
  it("rejects wrong kind/rate", () => {
    expect(decodeAudioFrame(new TextEncoder().encode('{"s":"x","seq":0,"kind":"opus","rate":16000}'))).toBeUndefined();
    expect(decodeAudioFrame(new TextEncoder().encode('{"s":"x","seq":0,"kind":"pcm16","rate":8000}'))).toBeUndefined();
  });
});

describe("text frames and hello", () => {
  it("parses only JSON objects with a string t", () => {
    expect(parseTextFrame('{"t":"ping"}')).toEqual({ t: "ping" });
    expect(parseTextFrame("[1]")).toBeUndefined();
    expect(parseTextFrame("nope")).toBeUndefined();
  });
  it("validates hello fields", () => {
    expect(parseHello({ t: "hello", id: "molty-k7m2q9xa4c", key: "AAAA", link_code: "ABCD2345" })).toBeDefined();
    expect(parseHello({ t: "hello", id: "molty-XYZ", key: "AAAA" })).toBeUndefined();
    expect(parseHello({ t: "hello", id: "molty-k7m2q9xa4c", key: "AAAA", link_code: "abcd2345" })).toBeUndefined();
    expect(parseHello({ t: "hello", id: "molty-k7m2q9xa4c", key: "AAAA", caps: [1] })).toBeUndefined();
  });
});

describe("crypto + ids", () => {
  it("base64 variants round-trip", () => {
    const b = Uint8Array.from({ length: 33 }, (_, i) => i * 7);
    expect(Array.from(b64Decode(b64Encode(b))!)).toEqual(Array.from(b));
    expect(Array.from(b64Decode(b64urlEncode(b))!)).toEqual(Array.from(b));
    expect(b64Decode("not base64!")).toBeUndefined();
  });
  it("link codes use [A-Z2-9]{8}", () => {
    for (let i = 0; i < 200; i++) expect(randomLinkCode()).toMatch(LINK_CODE_RE);
  });
  it("sha256 + safeEqual", async () => {
    expect(await sha256Hex("abc")).toBe("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect(safeEqual("abc", "abc")).toBe(true);
    expect(safeEqual("abc", "abd")).toBe(false);
    expect(safeEqual("abc", "ab")).toBe(false);
  });
  it("token / job / bearer parsing", () => {
    const tok = `molty-k7m2q9xa4c.${"A".repeat(43)}`;
    expect(tokenDevice(tok)).toBe("molty-k7m2q9xa4c");
    expect(tokenDevice("molty-k7m2q9xa4c.short")).toBeUndefined();
    expect(jobDevice("molty-k7m2q9xa4c.j_abcdefghijkl")).toBe("molty-k7m2q9xa4c");
    expect(jobDevice("../etc")).toBeUndefined();
    expect(bearer(`Bearer ${tok}`)).toBe(tok);
    expect(bearer("Basic x")).toBeUndefined();
  });
});
