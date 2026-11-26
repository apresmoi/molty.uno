// What is on Molty's screen (core/screen.ts, docs/AGENT-LINK.md "What is on the screen"): the relay follows
// the scene that is up (an agent's scene.play / queue / clear, the device's `scene` and `tap` events, its status,
// the ttl) and hands it to agents as `device.scene` on every utterance and `relay.scene` in status.
import { describe, expect, it } from "vitest";
import { AGENT_EVENT_TYPES, type AgentEvent, INBOX_LIMITS, agentEventData, eventSentence, webhookWants } from "../src/core/events.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import { SCREEN, type Screen, deviceSaid, held, played, queued, reconnected, sceneChanged, sceneContext, tapped } from "../src/core/screen.js";
import type { Clock } from "../src/core/store.js";
import { DEVICE, type FakeSocket, LINK, connect, flush, makeRoom } from "./helpers.js";

const CAPS = ["say", "listen", "listen_stop", "shake_talk", "pkg", "scene"];
const SIGNS = ["aries", "taurus", "gemini", "cancer", "leo", "virgo", "libra", "scorpio", "sagittarius", "capricorn", "aquarius", "pisces"];
const BODIES = ["sun", "moon", "mercury", "venus", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"];
/** The astrology chart's params, as skills/molty-astro/scripts/sky.mjs builds them: about 6 KB of lists whose elements carry an id and a reading. */
const CHART = {
  bodies: BODIES.map((id, i) => ({ g: `g-${id}`, a: 181.77 + i, r: 0.64, c: "#8fb4ff", say: `${id} in ${SIGNS[i]}: you dream by being bold and going first. Pluto lends a hand.`, id, of: SIGNS[i], da: 195.87, d: "#566c99", on: 4.19, off: 10.56 })),
  aspects: [["sun", "mars"], ["moon", "venus"], ["mercury", "pluto"]].map(([a, b]) => ({ x1: 0.355, y1: 0.456, x2: 0.444, y2: -0.206, px1: 0.344, py1: 0.54, px2: 0.455, py2: -0.291, c: "#ff5a4a", say: `${a} squares ${b}: friction between self and drive.`, id: `${a}-${b}`, lights: `${a}-${b} ${a} ${b}` })),
  signs: SIGNS.map((id, i) => ({ a: (195 + 30 * i) % 360, say: `${id}: bold, quick fire, ruled by Mars. Neptune and Saturn here: by being bold and going first, you live out your dreams and your discipline.`, id, sign: id, c: "#ff9a7a", name: id.toUpperCase(), nx: -0.095, ns: 0.13, ttl: 21 })),
  ruler: SIGNS.map((of) => ({ of, g: "g-mars", x: 0.1, c: "#ff5a4a" })),
};
const SMALL = { moon_lit: 0.499, moon_side: -1, spots: [{ id: "moon", say: "The moon is half lit, waxing, in Leo." }, { id: "leo", say: "Leo: warm and proud." }] };
const bytes = (v: unknown) => new TextEncoder().encode(JSON.stringify(v)).byteLength;

async function setup(opts: { caps?: string[] } = {}) {
  // real timers, a clock that can be pushed ahead: a scene's ttl runs out without waiting for it
  const skew = { ms: 0 };
  const clock: Clock = { now: () => Date.now() + skew.ms, setTimeout: (fn, ms) => setTimeout(fn, ms), clearTimeout: (h) => clearTimeout(h as ReturnType<typeof setTimeout>) };
  const ctx = makeRoom({ clock, timings: { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300 } });
  ctx.voice.sttText = "what does that mean?";
  /** What the device's own status says is up (absent: no scene), and every cmd it got. */
  const dev: { scene?: { pkg: string; layout: string; ttl_left: number }; cmds: string[] } = { cmds: [] };
  let sock: FakeSocket;
  const attach = async () => {
    sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: opts.caps ?? CAPS });
    const s = sock;
    const orig = s.sendText.bind(s);
    s.sendText = (t: string) => {
      orig(t);
      const f = JSON.parse(t);
      if (f.t === "pair.request") setTimeout(() => void ctx.room.socketMessage(s, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
      if (f.t !== "cmd") return;
      dev.cmds.push(f.name);
      const result =
        f.name === "listen" ? { job: f.args.job, ms: 30000 }
        : f.name === "status" ? { fw: "test-fw", creature: "fluid", ...(dev.scene ? { scene: dev.scene } : {}) }
        : f.name === "scene.play" || f.name === "scene.queue" ? { pkg: f.args.pkg, layout: f.args.layout, ttl_s: f.args.ttl_s }
        : {};
      setTimeout(() => void ctx.room.socketMessage(s, JSON.stringify({ t: "res", id: f.id, ok: true, result })), 1);
      if (f.name === "listen") {
        setTimeout(() => {
          void ctx.room.socketMessage(s, encodeAudioFrame({ s: f.args.job, seq: 0, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(3200).fill(8000).buffer)));
          void ctx.room.socketMessage(s, new TextEncoder().encode(JSON.stringify({ s: f.args.job, seq: 1, kind: "pcm16", rate: 16000, end: true, reason: "silence", speech_ms: 900 })));
        }, 15);
      }
    };
    s.sendBinary = (b: Uint8Array) => {
      const fr = decodeAudioFrame(b)!;
      if (fr.header.end) setTimeout(() => void ctx.room.socketMessage(s, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } })), 5);
    };
  };
  await attach();
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "astro" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  await flush();
  const token = out.result.agent_token as string;
  const emit = async (name: string, data: unknown) => {
    await ctx.room.socketMessage(sock, JSON.stringify({ t: "evt", name, data }));
    await flush();
  };
  const inbox = async () => ((await ctx.room.events(token, { wait_ms: 0 })) as any).result.events as AgentEvent[];
  const cmd = async (name: string, args: unknown = {}) => (await ctx.room.command(token, name, args)) as any;
  /** status's `relay.scene`. `said`: what the device's own status reports, when it is to be heard too. */
  const scene = async () => (await cmd("status")).result.relay.scene;
  let shakes = 0;
  /** The human presses record and speaks: the utterance the agents get. */
  const speak = async () => {
    await inbox();
    await emit("shake", { peak_g: 2.4, reversals: 5, ms: 900, listen: `shake-${++shakes}` });
    for (let i = 0; i < 300; i++) {
      const u = (await inbox()).find((e) => e.type === "utterance");
      if (u) return u.data as any;
      await new Promise((r) => setTimeout(r, 5));
    }
    throw new Error("no utterance");
  };
  const later = (ms: number) => void (skew.ms += ms);
  const record = () => ctx.kv.get<Screen>("screen");
  return { ...ctx, dev, token, emit, inbox, cmd, scene, speak, later, record, reconnect: attach };
}

/** A device that says what is up in its status, as the firmware does: the relay's record survives a status call. */
const play = async (w: Awaited<ReturnType<typeof setup>>, layout: string, params: object, ttl_s = 600, op = "scene.play") => {
  const r = await w.cmd(op, { pkg: "astrology", layout, params, ttl_s });
  expect(r).toMatchObject({ ok: true });
  if (op === "scene.play") w.dev.scene = { pkg: "astrology", layout, ttl_left: ttl_s };
};

describe("tap: what Molty did with it", () => {
  it("`opened` (a layout's name) and `closed` (true) reach agents; a malformed one is dropped, the tap is kept", async () => {
    const w = await setup();
    await w.emit("tap", { x: 0.5, y: 0.5, spot: "aries", opened: "sign-closeup" });
    await w.emit("tap", { x: 0.1, y: 0.9, closed: true });
    await w.emit("tap", { x: 0.5, y: 0.5, spot: "aries", opened: "no spaces here", closed: "yes" });
    await w.emit("tap", { x: 0.5, y: 0.5, opened: 7, closed: false, extra: 1 });
    expect((await w.inbox()).map((e) => [e.type, e.data])).toEqual([
      ["tap", { x: 0.5, y: 0.5, spot: "aries", opened: "sign-closeup" }],
      ["tap", { x: 0.1, y: 0.9, closed: true }],
      ["tap", { x: 0.5, y: 0.5, spot: "aries" }],
      ["tap", { x: 0.5, y: 0.5 }],
    ]);
    expect(agentEventData("tap", { x: 0.5, y: 0.5, opened: "x".repeat(33) })).toEqual({ x: 0.5, y: 0.5 });
  });
});

describe("scene event: the device changed its own screen", () => {
  it("is cleaned, reaches agents and reads as one line; a malformed one is dropped", async () => {
    const w = await setup();
    await w.emit("scene", { why: "open", pkg: "astrology", layout: "sign-closeup", spot: "aries", extra: 1 });
    await w.emit("scene", { why: "back", pkg: "astrology", layout: "chart", spot: "not a spot" });
    await w.emit("scene", { why: "end", pkg: "astrology", layout: "globe" });
    await w.emit("scene", { why: "end" });
    for (const bad of [{ why: "started", pkg: "astrology", layout: "chart" }, { pkg: "astrology", layout: "chart" }, { why: "open", pkg: "Astro Logy", layout: "chart" }, { why: "open", pkg: "astrology" }, { why: "end", layout: "chart" }, { why: "open", pkg: "astrology", layout: "a b" }]) await w.emit("scene", bad);
    const got = await w.inbox();
    expect(got.map((e) => [e.type, e.data])).toEqual([
      ["scene", { why: "open", pkg: "astrology", layout: "sign-closeup", spot: "aries" }],
      ["scene", { why: "back", pkg: "astrology", layout: "chart" }],
      ["scene", { why: "end", pkg: "astrology", layout: "globe" }],
      ["scene", { why: "end" }],
    ]);
    expect(got.map(eventSentence)).toEqual([
      "Molty opened sign-closeup of astrology (the human tapped aries)",
      "Molty went back to chart of astrology",
      "Molty's scene ended: globe of astrology is up now",
      "Molty's scene ended: the creature is back",
    ]);
  });

  it("rides the inbox only: no webhook gets it, named or not, and the types a webhook can name are as before", () => {
    expect(webhookWants(undefined, "scene")).toBe(false);
    expect(webhookWants(["scene", "tap"], "scene")).toBe(false);
    expect(webhookWants(undefined, "tap")).toBe(true);
    expect(AGENT_EVENT_TYPES).toEqual(["shake", "tap", "double_tap", "button", "answer", "ring", "rhythm", "utterance", "utterance.partial", "draft.used", "draft.dropped", "creature", "dnd"]);
  });
});

describe("the screen that is up", () => {
  it("follows the agent's play, the device's open, back and end, and a clear", async () => {
    const w = await setup();
    expect(await w.scene()).toBeNull();
    await play(w, "tonight", SMALL);
    expect(await w.scene()).toEqual({ pkg: "astrology", layout: "tonight", params: SMALL });

    // a tap on a spot that opens a layout: the device says so, the relay cannot know that layout's params
    await w.emit("tap", { x: 0.5, y: 0.5, spot: "leo", opened: "sign-closeup" });
    await w.emit("scene", { why: "open", pkg: "astrology", layout: "sign-closeup", spot: "leo" });
    w.dev.scene = { pkg: "astrology", layout: "sign-closeup", ttl_left: 20 };
    expect(await w.scene()).toEqual({ pkg: "astrology", layout: "sign-closeup", params: null, spot: "leo", spot_age_s: 0, reading: "Leo: warm and proud.", under: { layout: "tonight", params: SMALL, spot: "leo" } });

    // a tap beside it: back to the layout under it, with its params and the spot last tapped there
    await w.emit("tap", { x: 0.05, y: 0.5, closed: true });
    await w.emit("scene", { why: "back", pkg: "astrology", layout: "tonight" });
    w.dev.scene = { pkg: "astrology", layout: "tonight", ttl_left: 500 };
    expect(await w.scene()).toEqual({ pkg: "astrology", layout: "tonight", params: SMALL, spot: "leo", spot_age_s: 0, reading: "Leo: warm and proud." });

    // opened again, and this time it runs out by itself: the device's `end` names the layout under it
    await w.emit("scene", { why: "open", pkg: "astrology", layout: "sign-closeup", spot: "leo" });
    await w.emit("scene", { why: "end", pkg: "astrology", layout: "tonight" });
    expect(await w.scene()).toMatchObject({ layout: "tonight", params: SMALL });

    w.later(SCREEN.settleMs);
    await w.emit("scene", { why: "end" });
    w.dev.scene = undefined;
    expect(await w.scene()).toBeNull();

    await play(w, "tonight", SMALL);
    expect(await w.cmd("scene.clear")).toMatchObject({ ok: true });
    expect(await w.record()).toBeUndefined();
  });

  it("text or another state takes the screen from a scene (the firmware drops it without a word)", async () => {
    const w = await setup();
    for (const [name, args] of [["show", { text: "hello" }], ["state", { state: { state: "creature" } }]] as const) {
      await play(w, "tonight", SMALL);
      expect(await w.cmd(name, args), name).toMatchObject({ ok: true });
      expect(await w.record(), name).toBeUndefined();
    }
    // a mood alone keeps the scene, as on the device
    await play(w, "tonight", SMALL);
    expect(await w.cmd("mood", { mood: "happy" })).toMatchObject({ ok: true });
    expect(await w.record()).toMatchObject({ layout: "tonight" });
  });

  it("runs out with its ttl when no `end` arrived", async () => {
    const w = await setup();
    await play(w, "tonight", SMALL, 60);
    w.later(60_000 + SCREEN.graceMs - 2000);
    expect((await w.speak()).device.scene).toMatchObject({ layout: "tonight" });
    // (that turn held its clock a moment: well past it)
    w.later(10_000);
    expect((await w.speak()).device.scene).toBeUndefined();
    w.dev.scene = undefined;
    expect(await w.scene()).toBeNull();
  });

  it("the queue: the device's `end` names the next one and the relay knows its params; without it, the relay does not guess", async () => {
    const w = await setup();
    const MOON = { planet: "moon" }, MARS = { planet: "mars" }, SATURN = { rings: 1 };
    await play(w, "globe", MOON, 30);
    await play(w, "globe", MARS, 30, "scene.queue");
    await play(w, "saturn", SATURN, 30, "scene.queue");
    expect(await w.record()).toMatchObject({ layout: "globe", params: MOON, queue: [{ layout: "globe", params: MARS }, { layout: "saturn", params: SATURN }] });
    w.later(20_000);
    await w.emit("scene", { why: "end", pkg: "astrology", layout: "globe" });
    expect((await w.speak()).device.scene).toEqual({ pkg: "astrology", layout: "globe", params: MARS });
    w.later(20_000);
    await w.emit("scene", { why: "end", pkg: "astrology", layout: "saturn" });
    expect((await w.speak()).device.scene).toEqual({ pkg: "astrology", layout: "saturn", params: SATURN });

    // older firmware says nothing: once the first one's ttl is over the relay cannot tell which is up, and says nothing
    const old = await setup();
    await play(old, "globe", MOON, 30);
    await play(old, "globe", MARS, 30, "scene.queue");
    expect((await old.speak()).device.scene).toEqual({ pkg: "astrology", layout: "globe", params: MOON });
    old.later(30_000 + SCREEN.graceMs + 5000);
    expect((await old.speak()).device.scene).toBeUndefined();
    // ...until the device's own status names a layout only one of them has
    await play(old, "globe", MOON, 30);
    await play(old, "saturn", SATURN, 30, "scene.queue");
    old.later(30_000 + SCREEN.graceMs + 5000);
    old.dev.scene = { pkg: "astrology", layout: "saturn", ttl_left: 12 };
    expect(await old.scene()).toEqual({ pkg: "astrology", layout: "saturn", params: SATURN });
  });

  it("a queued scene plays at once when nothing is up", async () => {
    const w = await setup();
    await play(w, "globe", { planet: "moon" }, 30, "scene.queue");
    expect((await w.speak()).device.scene).toEqual({ pkg: "astrology", layout: "globe", params: { planet: "moon" } });
  });

  it("a reconnect: the scene stays on its ttl, what was queued is no longer followed, and the device's status settles it", async () => {
    const w = await setup();
    await play(w, "globe", { planet: "moon" }, 30);
    await play(w, "globe", { planet: "mars" }, 30, "scene.queue");
    await w.reconnect();
    expect(await w.record()).toMatchObject({ layout: "globe", params: { planet: "moon" }, queue: [] });
    // an `end` may have been lost: the next one's params are not guessed from the old queue
    w.later(5000);
    await w.emit("scene", { why: "end", pkg: "astrology", layout: "globe" });
    expect((await w.speak()).device.scene).toEqual({ pkg: "astrology", layout: "globe", params: null });

    // it came back with no scene (a reboot): its status says so, and the relay's record goes
    await play(w, "tonight", SMALL);
    await w.reconnect();
    w.later(SCREEN.settleMs);
    w.dev.scene = undefined;
    expect(await w.scene()).toBeNull();
    expect((await w.speak()).device.scene).toBeUndefined();
  });

  it("the device's status is the truth: it refreshes the ttl, names a scene the relay did not see, and is not heard a frame after a play", async () => {
    const w = await setup();
    await play(w, "tonight", SMALL, 60);
    w.dev.scene = undefined; // the device's status lags the play by a frame
    expect(await w.scene()).toMatchObject({ layout: "tonight" });
    w.later(50_000);
    w.dev.scene = { pkg: "astrology", layout: "tonight", ttl_left: 100 }; // its clock waited meanwhile
    expect(await w.scene()).toMatchObject({ layout: "tonight", params: SMALL });
    w.later(80_000);
    expect((await w.speak()).device.scene).toMatchObject({ layout: "tonight" });
    w.dev.scene = { pkg: "moons", layout: "phases", ttl_left: 30 }; // put up from the device's own console
    expect(await w.scene()).toEqual({ pkg: "moons", layout: "phases", params: null });
  });

  it("the inbox's rate cap never costs the screen a change", async () => {
    const w = await setup();
    await play(w, "tonight", SMALL);
    for (let i = 0; i < INBOX_LIMITS.perMinute + 5; i++) await w.emit("tap", { x: 0, y: 0 });
    await w.emit("tap", { x: 0.5, y: 0.5, spot: "moon" });
    await w.emit("scene", { why: "open", pkg: "astrology", layout: "globe", spot: "moon" });
    expect((await w.inbox()).filter((e) => e.type === "scene")).toEqual([]); // dropped by the cap
    expect(await w.record()).toMatchObject({ layout: "globe", spot: { id: "moon" }, under: { layout: "tonight" } });
  });

  it("its clock waits while Molty listens and speaks, as the device's does", async () => {
    const w = await setup();
    await play(w, "tonight", SMALL, 60);
    const before = (await w.record())!.until;
    await w.speak();
    const heard = (await w.record())!.until;
    expect(heard).toBeGreaterThan(before);
    const said = await w.cmd("say", { text: "It means the moon is half lit." });
    expect(said).toMatchObject({ ok: true });
    for (let i = 0; i < 200 && (await w.record())!.until === heard; i++) await new Promise((r) => setTimeout(r, 5));
    expect((await w.record())!.until).toBeGreaterThanOrEqual(heard + 1000); // a second of speech
  });
});

describe("an utterance says what the human is looking at", () => {
  it("device.scene: the scene, its params, the spot last tapped and its reading", async () => {
    const w = await setup();
    await play(w, "tonight", SMALL);
    expect((await w.speak()).device.scene).toEqual({ pkg: "astrology", layout: "tonight", params: SMALL });
    await w.emit("tap", { x: 0.5, y: 0.4, spot: "moon" });
    await w.emit("tap", { x: 0.9, y: 0.9 }); // beside every spot: the last spot stays
    const u = await w.speak();
    expect(u.device.scene).toEqual({ pkg: "astrology", layout: "tonight", params: SMALL, spot: "moon", spot_age_s: 0, reading: "The moon is half lit, waxing, in Leo." });
    expect(u.device).toMatchObject({ mood: "thinking", caps: CAPS }); // the rest of `device` as before
    // a spot the params carry no reading for: named all the same
    await w.emit("tap", { x: 0.5, y: 0.4, spot: "horizon" });
    expect((await w.speak()).device.scene).toEqual({ pkg: "astrology", layout: "tonight", params: SMALL, spot: "horizon", spot_age_s: 0 });
  });

  it("the reading rides along only while it was just read; the spot stays", async () => {
    const w = await setup();
    await play(w, "tonight", SMALL);
    await w.emit("tap", { x: 0.5, y: 0.4, spot: "moon" });
    w.later(SCREEN.readingMs + 1000);
    expect((await w.speak()).device.scene).toEqual({ pkg: "astrology", layout: "tonight", params: SMALL, spot: "moon", spot_age_s: 61 });
  });

  it("nothing is attached when no scene is up: the utterance is as before", async () => {
    const w = await setup();
    await w.emit("tap", { x: 0.5, y: 0.4, spot: "moon" }); // a spot without a scene the relay knows
    const u = await w.speak();
    expect(Object.keys(u.device).sort()).toEqual(["caps", "mood", "voice"]);
    expect(Object.keys(u).sort()).toEqual(["device", "filler_in_ms", "ms", "reason", "text", "trigger", "turn_id", "input"].sort());
    expect(await w.record()).toBeUndefined();
  });

  it("big params are cut down to the cap: the tapped element whole, the others by id; pkg, layout and spot always", async () => {
    const w = await setup();
    expect(bytes(CHART)).toBeGreaterThan(5000);
    await play(w, "chart", CHART);
    await w.emit("tap", { x: 0.5, y: 0.4, spot: "sun" });
    const s = (await w.speak()).device.scene;
    expect(bytes(s.params)).toBeLessThanOrEqual(SCREEN.maxParamsBytes);
    expect(s).toEqual({
      pkg: "astrology",
      layout: "chart",
      params: { bodies: [CHART.bodies[0], ...BODIES.slice(1).map((id) => ({ id }))], aspects: CHART.aspects.map(({ id }) => ({ id })), signs: SIGNS.map((id) => ({ id })), ruler: [] },
      spot: "sun",
      spot_age_s: 0,
      reading: CHART.bodies[0].say,
      trimmed: true,
    });

    // a sign's close-up, opened by the device: the chart under it keeps what belongs to that sign (its element, the elements `of` it)
    await w.emit("tap", { x: 0.2, y: 0.2, spot: "aries", opened: "sign-closeup" });
    await w.emit("scene", { why: "open", pkg: "astrology", layout: "sign-closeup", spot: "aries" });
    const up = (await w.speak()).device.scene;
    expect(up).toEqual({
      pkg: "astrology",
      layout: "sign-closeup",
      params: null,
      spot: "aries",
      spot_age_s: 0,
      reading: CHART.signs[0].say,
      under: {
        layout: "chart",
        params: { bodies: [CHART.bodies[0], ...BODIES.slice(1).map((id) => ({ id }))], aspects: CHART.aspects.map(({ id }) => ({ id })), signs: [CHART.signs[0], ...SIGNS.slice(1).map((id) => ({ id }))], ruler: [CHART.ruler[0]] },
        spot: "aries",
      },
      trimmed: true,
    });
    expect(bytes(up)).toBeLessThan(SCREEN.maxParamsBytes + 600);

    // params no list can explain: left out, the rest stays
    const blob = { notes: "n".repeat(3000) };
    const big = sceneContext({ ...played({ pkg: "astrology", layout: "chart", params: blob, ttl_s: 60 }, 0), spot: { id: "sun", at: 0 } }, 1000)!;
    expect(big).toEqual({ pkg: "astrology", layout: "chart", params: null, spot: "sun", spot_age_s: 1, trimmed: true });
    // lists whose kept elements alone are too much lose the ids first... then everything but the tapped one
    const wide = { stars: Array.from({ length: 150 }, (_, i) => ({ id: `star-${i}`, say: "A star." })) };
    const cut = sceneContext({ ...played({ pkg: "sky", layout: "stars", params: wide, ttl_s: 60 }, 0), spot: { id: "star-7", at: 0 } }, 0)!;
    expect(cut).toMatchObject({ params: { stars: [{ id: "star-7", say: "A star." }] }, reading: "A star.", trimmed: true });
  });
});

describe("older firmware (no `scene` events, no `opened`): as today", () => {
  it("taps and utterances keep their shape; the agent's own play and the spot still tell what is up", async () => {
    const w = await setup();
    await play(w, "tonight", SMALL);
    await w.emit("tap", { x: 0.5, y: 0.4, spot: "moon" });
    expect((await w.inbox()).map((e) => [e.type, e.data])).toEqual([["tap", { x: 0.5, y: 0.4, spot: "moon" }]]);
    expect((await w.speak()).device.scene).toMatchObject({ layout: "tonight", spot: "moon", reading: "The moon is half lit, waxing, in Leo." });
  });

  it("a Molty without scenes: nothing is followed, status says null", async () => {
    const w = await setup({ caps: ["say", "listen", "listen_stop", "shake_talk"] });
    expect(await w.cmd("scene.play", { pkg: "astrology", layout: "tonight", params: SMALL })).toMatchObject({ ok: false, error: "unsupported" });
    expect(await w.scene()).toBeNull();
    expect((await w.speak()).device.scene).toBeUndefined();
    expect(await w.record()).toBeUndefined();
  });
});

describe("core/screen.ts", () => {
  const A = { pkg: "astrology", layout: "chart", params: { n: 1 }, ttl_s: 60 }, B = { pkg: "astrology", layout: "globe", params: { n: 2 }, ttl_s: 30 };
  const T = 1_000_000;

  it("up: until its ttl and a grace; a queued scene after one that ran out is not promoted by the clock", () => {
    const s = queued(played(A, T), B, T);
    expect(sceneContext(s, T + 60_000 + SCREEN.graceMs - 1)).toMatchObject({ layout: "chart" });
    expect(sceneContext(s, T + 60_000 + SCREEN.graceMs)).toBeUndefined();
    expect(queued(s, B, T + 200_000)).toMatchObject({ layout: "chart", queue: [B, B] }); // which of them is up is not known: it waits its turn
    expect(queued(played(A, T), B, T + 200_000)).toMatchObject({ layout: "globe", queue: [] }); // nothing is up any more: it plays at once
    expect(queued(undefined, B, T)).toMatchObject({ layout: "globe", params: { n: 2 }, until: T + 30_000 });
  });

  it("a device event the relay cannot place: what is on, without params; a stale `end` right after a play is not heard", () => {
    expect(sceneChanged(undefined, { why: "open", pkg: "astrology", layout: "sign-closeup", spot: "aries" }, T)).toEqual({ pkg: "astrology", layout: "sign-closeup", params: null, until: T + 600_000, spot: { id: "aries", at: T }, queue: [] });
    expect(sceneChanged(played(A, T), { why: "end", pkg: "moons", layout: "phases" }, T + 5000)).toMatchObject({ pkg: "moons", layout: "phases", params: null });
    const s = played(A, T);
    expect(sceneChanged(s, { why: "end" }, T + SCREEN.settleMs - 1)).toBe(s);
    expect(sceneChanged(s, { why: "end" }, T + SCREEN.settleMs)).toBeUndefined();
  });

  it("opened one deep: a layout opened from an opened one takes its place, the first stays under", () => {
    const one = sceneChanged(tapped(played(A, T), "aries", T + 1000), { why: "open", pkg: "astrology", layout: "sign-closeup", spot: "aries" }, T + 1100)!;
    expect(one).toMatchObject({ layout: "sign-closeup", params: null, until: T + 60_000, spot: { id: "aries", at: T + 1000 }, under: { layout: "chart", params: { n: 1 }, spot: { id: "aries", at: T + 1000 } } });
    const two = sceneChanged(one, { why: "open", pkg: "astrology", layout: "globe", spot: "mars" }, T + 5000)!;
    expect(two).toMatchObject({ layout: "globe", spot: { id: "mars", at: T + 5000 }, under: { layout: "chart", spot: { id: "aries" } } });
    expect(sceneChanged(two, { why: "back", pkg: "astrology", layout: "chart" }, T + 6000)).toEqual({ pkg: "astrology", layout: "chart", params: { n: 1 }, until: T + 60_000, spot: { id: "aries", at: T + 1000 }, queue: [] });
    expect(sceneChanged(one, { why: "open", pkg: "astrology", layout: "sign-closeup", spot: "aries" }, T + 1200)).toBe(one); // said twice
  });

  it("held: the clock waits through a listen or a say, only for a scene that was up when it began", () => {
    const s = played(A, T);
    expect(held(s, T + 10_000, T + 18_000)!.until).toBe(s.until + 8000);
    expect(held(s, s.until + SCREEN.graceMs, s.until + SCREEN.graceMs + 5000)).toBe(s);
    expect(held(undefined, T, T + 1)).toBeUndefined();
    const open = sceneChanged(s, { why: "open", pkg: "astrology", layout: "sign-closeup" }, T)!;
    expect(held(open, T, T + 3000)).toMatchObject({ until: s.until + 3000, under: { until: s.until + 3000 } });
  });

  it("reconnected and tapped leave what they do not change alone", () => {
    const s = played(A, T);
    expect(reconnected(s)).toBe(s);
    expect(reconnected(queued(s, B, T))).toEqual(s);
    expect(tapped(s, "sun", T + 200_000)).toBe(s); // it ran out: the tap is on something else
    expect(tapped(undefined, "sun", T)).toBeUndefined();
  });

  it("deviceSaid: two queued scenes of one layout cannot be told apart, so the params stay unknown", () => {
    const s = queued(queued(played(A, T), B, T), { ...B, params: { n: 3 } }, T);
    const now = T + 200_000;
    expect(deviceSaid(s, { pkg: "astrology", layout: "globe", ttl_left: 10 }, now)).toEqual({ pkg: "astrology", layout: "globe", params: null, until: now + 10_000, queue: [] });
    expect(deviceSaid(s, { pkg: "astrology", layout: "chart", ttl_left: 10 }, now)).toMatchObject({ layout: "chart", params: { n: 1 }, until: now + 10_000 }); // it outlived the relay's clock
    expect(deviceSaid(s, { pkg: "Bad Pkg", layout: "chart" }, now)).toBeUndefined();
  });
});
