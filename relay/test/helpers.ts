import { DeviceRoom, type DeviceSocket, type RoomDeps, type RoomTimings, type SocketMeta } from "../src/core/room.js";
import { MemoryKV, realClock } from "../src/core/store.js";
import { b64Encode } from "../src/core/crypto.js";
import type { VoiceProvider } from "../src/voice/types.js";

export const DEVICE = "molty-k7m2q9xa4c";
export const KEY = b64Encode(new Uint8Array(32).fill(7));
export const LINK = "ABCD2345";

export class FakeSocket implements DeviceSocket {
  meta: SocketMeta | undefined;
  texts: string[] = [];
  bins: Uint8Array[] = [];
  closed: { code: number; reason: string } | undefined;
  constructor(private readonly host: FakeHost) {}
  sendText(t: string) {
    if (this.closed) throw new Error("closed");
    this.texts.push(t);
  }
  sendBinary(b: Uint8Array) {
    if (this.closed) throw new Error("closed");
    this.bins.push(b);
  }
  close(code: number, reason: string) {
    if (this.closed) return;
    this.closed = { code, reason };
    this.host.onClose?.(this);
  }
  getMeta() {
    return this.meta;
  }
  setMeta(m: SocketMeta) {
    this.meta = structuredClone(m);
  }
  same(o: DeviceSocket): boolean {
    return o === this;
  }
  frames() {
    return this.texts.map((t) => JSON.parse(t));
  }
  last(t: string) {
    return this.frames().filter((f) => f.t === t).at(-1);
  }
}

export class FakeHost {
  list: FakeSocket[] = [];
  onClose?: (s: FakeSocket) => void;
  sockets() {
    return this.list.filter((s) => !s.closed);
  }
  open() {
    const s = new FakeSocket(this);
    this.list.push(s);
    return s;
  }
}

export class FakeVoice implements VoiceProvider {
  name = "fake";
  ttsCalls: Array<{ text: string; voice?: string }> = [];
  sttCalls: Int16Array[] = [];
  ttsSamples = 16000; // 1 s
  sttText = "hello molty";
  async tts(text: string, opts: { voice?: string }) {
    this.ttsCalls.push({ text, voice: opts.voice });
    return new Int16Array(this.ttsSamples).fill(1000);
  }
  async stt(pcm: Int16Array) {
    this.sttCalls.push(pcm);
    return this.sttText;
  }
}

export function makeRoom(opts: { kv?: MemoryKV; host?: FakeHost; voice?: FakeVoice; timings?: Partial<RoomTimings> } & Partial<Pick<RoomDeps, "clock" | "scheduler" | "fetch" | "resolveHost" | "log">> = {}) {
  const kv = opts.kv ?? new MemoryKV();
  const host = opts.host ?? new FakeHost();
  const voice = opts.voice ?? new FakeVoice();
  const room = new DeviceRoom({
    deviceId: DEVICE,
    kv,
    clock: opts.clock ?? realClock,
    voice,
    host,
    timings: opts.timings,
    scheduler: opts.scheduler,
    fetch: opts.fetch,
    resolveHost: opts.resolveHost,
    log: opts.log,
  });
  host.onClose = (s) => room.socketClose(s);
  return { room, kv, host, voice };
}

export async function connect(room: DeviceRoom, host: FakeHost, key = KEY, link: string | null = LINK, hello: Record<string, unknown> = {}) {
  const s = host.open();
  room.socketOpen(s);
  await room.socketMessage(s, JSON.stringify({ t: "hello", id: DEVICE, key, fw: "test-fw", caps: ["say", "listen"], ...(link ? { link_code: link } : {}), ...hello }));
  return s;
}

/** Let queued microtasks (KV awaits) run. */
export const flush = async (n = 20) => {
  for (let i = 0; i < n; i++) await Promise.resolve();
};
