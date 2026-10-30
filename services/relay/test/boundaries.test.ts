// Structural guarantees that behavioural tests cannot see:
// module seams and the hibernation rule.
import { readFileSync, readdirSync } from "node:fs";
import { describe, expect, it } from "vitest";

const src = (p: string) => readFileSync(new URL(`../src/${p}`, import.meta.url), "utf8");
const dir = (p: string) => readdirSync(new URL(`../src/${p}`, import.meta.url)).map((f) => `${p}/${f}`);

describe("module seams", () => {
  it("core, surfaces and voice import nothing from Cloudflare", () => {
    for (const f of [...dir("core"), ...dir("surfaces"), ...dir("voice")]) {
      expect(src(f), f).not.toMatch(/cloudflare:|@cloudflare\/|DurableObject|WebSocketPair/);
    }
  });
  it("core does not import surfaces, hosts or concrete voice providers", () => {
    for (const f of dir("core")) {
      expect(src(f), f).not.toMatch(/from "\.\.\/(surfaces|worker|device-object)|voice\/(index|workers-ai|openai|none)/);
    }
  });
});

describe("hibernation rule", () => {
  const room = src("core/room.ts");
  it("DeviceRoom keeps no socket in an instance field: the live device is read from socket metadata", () => {
    const fields = [...room.matchAll(/^ {2}private (?:readonly )?(\w+)\s*[:=]\s*([^;]+);/gm)].map((m) => `${m[1]}: ${m[2]}`);
    expect(fields.length).toBeGreaterThan(0);
    for (const f of fields) expect(f).not.toMatch(/DeviceSocket|WebSocket/);
    expect(room).toMatch(/getMeta\(\)\?\.stage === "live"/);
  });
  it("the Durable Object keeps hibernation-compatible sockets and only a rebuildable room cache", () => {
    const dobj = src("device-object.ts");
    expect(dobj).toMatch(/this\.ctx\.acceptWebSocket\(server\)/);
    expect(dobj).toMatch(/serializeAttachment/);
    expect(dobj).toMatch(/setWebSocketAutoResponse/);
    const cls = dobj.slice(dobj.indexOf("export class MoltyDevice"));
    const fields = [...cls.matchAll(/^ {2}(?!static )(?:private |readonly )*#?[A-Za-z_$][\w$]*\s*[:=]/gm)].map((m) => m[0].trim());
    // #releases: only in the `releases` singleton, a cache of storage-backed state (releases-host.ts).
    expect(fields).toEqual(["#room:", "#releases:"]);
  });
});
