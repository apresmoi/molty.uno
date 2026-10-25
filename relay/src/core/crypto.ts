// Web Crypto helpers. `globalThis.crypto` exists on Workers and Node >= 20,
// so core stays host-neutral.
import { LINK_CODE_ALPHABET } from "./protocol.js";

export function randomBytes(n: number): Uint8Array {
  const out = new Uint8Array(n);
  crypto.getRandomValues(out);
  return out;
}

export function b64urlEncode(bytes: Uint8Array): string {
  let bin = "";
  for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
  return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function b64Encode(bytes: Uint8Array): string {
  let bin = "";
  const CHUNK = 0x8000;
  for (let i = 0; i < bytes.length; i += CHUNK) bin += String.fromCharCode(...bytes.subarray(i, i + CHUNK));
  return btoa(bin);
}

/** Decodes standard or URL-safe base64, with or without padding. Undefined on malformed input. */
export function b64Decode(text: string): Uint8Array | undefined {
  if (!/^[A-Za-z0-9+/_-]*={0,2}$/.test(text)) return undefined;
  const std = text.replace(/-/g, "+").replace(/_/g, "/");
  const padded = std + "===".slice((std.length + 3) % 4);
  try {
    const bin = atob(padded);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  } catch {
    return undefined;
  }
}

export async function sha256Hex(data: Uint8Array | string): Promise<string> {
  const bytes = typeof data === "string" ? new TextEncoder().encode(data) : data;
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return Array.from(digest, (b) => b.toString(16).padStart(2, "0")).join("");
}

/** Constant-time comparison for equal-length strings (hashes, codes). */
export function safeEqual(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

/** Uniform random string from an alphabet (rejection sampling, no modulo bias). */
export function randomFrom(alphabet: string, length: number): string {
  const limit = 256 - (256 % alphabet.length);
  let out = "";
  while (out.length < length) {
    for (const b of randomBytes(length * 2)) {
      if (b < limit && out.length < length) out += alphabet[b % alphabet.length];
    }
  }
  return out;
}

export const randomDigits = (n: number) => randomFrom("0123456789", n);
export const randomLinkCode = () => randomFrom(LINK_CODE_ALPHABET, 8);
