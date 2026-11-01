// Cloudflare host for one DeviceRoom: a Durable Object per device_id with
// hibernatable WebSockets. All logic is in src/core; this file only adapts
// storage, sockets and RPC. Hibernation rule: the authenticated-device flag
// lives in each socket's serialized attachment (SocketMeta), never here.
import { DurableObject } from "cloudflare:workers";
import { type ActivityOwnerRequest, type EventsRequest, type JobView, type NotesRequest, type Outcome, type PairRequest, type PairResult, type VoiceOwnerRequest, type WebhookRequest, bearer } from "./core/api.js";
import { PING_TEXT, PONG_TEXT } from "./core/protocol.js";
import { type DeviceSocket, DeviceRoom, type SocketMeta } from "./core/room.js";
import { type KV, realClock } from "./core/store.js";
import { dohResolver } from "./core/webhook.js";
import { createVoice, type VoiceEnv, type VoiceProvider } from "./voice/index.js";
import type { LookupMode, ReleaseCache, ReleaseLookup } from "./core/releases.js";
import { type ReleaseEnv, releaseCache, releaseSource } from "./releases-host.js";

export interface Env extends VoiceEnv, ReleaseEnv {
  DEVICE: DurableObjectNamespace<MoltyDevice>;
}

interface Attachment {
  device: string;
  meta?: SocketMeta;
}

class WsSocket implements DeviceSocket {
  constructor(readonly ws: WebSocket) {}
  sendText(text: string) {
    this.ws.send(text);
  }
  sendBinary(bytes: Uint8Array) {
    this.ws.send(bytes);
  }
  close(code: number, reason: string) {
    try {
      this.ws.close(code, reason);
    } catch {
      /* already closed */
    }
  }
  getMeta() {
    return (this.ws.deserializeAttachment() as Attachment | null)?.meta;
  }
  setMeta(meta: SocketMeta) {
    const att = (this.ws.deserializeAttachment() as Attachment | null) ?? { device: "" };
    this.ws.serializeAttachment({ ...att, meta } satisfies Attachment);
  }
  same(other: DeviceSocket): boolean {
    return other instanceof WsSocket && other.ws === this.ws;
  }
}

function storageKV(storage: DurableObjectStorage): KV {
  return {
    get: (k) => storage.get(k),
    put: (k, v) => storage.put(k, v),
    delete: async (k) => {
      await storage.delete(k);
    },
    list: (prefix) => storage.list({ prefix }) as Promise<Map<string, never>>,
    // Atomic by the Durable Object input gate: while `storage.get` runs no other
    // event is delivered to this object, and between its completion and the
    // write below there is no await on external I/O (only microtasks), so no
    // other request can interleave. Never add a fetch/RPC await in here.
    update: async (key, fn) => {
      const { next, result } = fn(await storage.get(key));
      if (next === null) await storage.delete(key);
      else if (next !== undefined) await storage.put(key, next);
      return result;
    },
  };
}

function safeVoice(env: Env): VoiceProvider {
  try {
    return createVoice(env);
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e);
    return { name: "misconfigured", tts: () => Promise.reject(new Error(msg)), stt: () => Promise.reject(new Error(msg)) };
  }
}

export class MoltyDevice extends DurableObject<Env> {
  // A cache of the stateless-by-construction core object, rebuilt after eviction.
  #room: DeviceRoom | undefined;
  /** Only in the `releases` singleton (releases-host.ts). */
  #releases: ReleaseCache | undefined;

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    // Answered by the runtime without waking the object.
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair(PING_TEXT, PONG_TEXT));
  }

  private room(deviceId: string): DeviceRoom {
    if (this.#room && this.#room.deviceId === deviceId) return this.#room;
    // alarm() carries no request, so remember which device this object is.
    void this.ctx.storage.put("room_id", deviceId);
    const fetcher = (req: Request) => fetch(req);
    this.#room = new DeviceRoom({
      deviceId,
      kv: storageKV(this.ctx.storage),
      clock: realClock,
      voice: safeVoice(this.env),
      host: { sockets: () => this.ctx.getWebSockets().map((ws) => new WsSocket(ws)) },
      log: (msg, data) => console.log(JSON.stringify({ msg, ...data })),
      scheduler: { setAlarm: (at) => this.ctx.storage.setAlarm(at) },
      fetch: fetcher,
      // Workers have no socket-level resolver: DNS-over-HTTPS for the SSRF guard (services/relay/README.md "Webhooks").
      resolveHost: dohResolver(fetcher),
      releases: releaseSource(this.env.DEVICE as unknown as DurableObjectNamespace),
    });
    return this.#room;
  }

  private roomFor(ws: WebSocket): DeviceRoom | undefined {
    const att = ws.deserializeAttachment() as Attachment | null;
    return att?.device ? this.room(att.device) : undefined;
  }

  async fetch(request: Request): Promise<Response> {
    if (new URL(request.url).pathname === "/v1/voice") return this.voiceUpgrade(request);
    const id = new URL(request.url).searchParams.get("id") ?? "";
    const pair = new WebSocketPair();
    const [client, server] = [pair[0], pair[1]];
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ device: id } satisfies Attachment);
    this.room(id).socketOpen(new WsSocket(server));
    return new Response(null, { status: 101, webSocket: client });
  }

  /**
   * The voice box's socket (worker.ts checked the upgrade and the token's device). The
   * token is checked HERE, before accepting: a refused box sees the HTTP status (401
   * unknown/revoked, 403 an agent token), never an open socket. Hibernatable like the
   * device's: its role lives in its own attachment (SocketMeta stage "voice").
   */
  private async voiceUpgrade(request: Request): Promise<Response> {
    const id = new URL(request.url).searchParams.get("device_id") ?? "";
    const room = this.room(id);
    const auth = await room.voiceAuth(bearer(request.headers.get("authorization")) ?? "");
    if (!auth.ok) return Response.json({ ok: false, error: auth.error, message: auth.message }, { status: auth.error === "forbidden" ? 403 : 401 });
    const pair = new WebSocketPair();
    const [client, server] = [pair[0], pair[1]];
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ device: id } satisfies Attachment);
    room.voiceOpen(new WsSocket(server), auth.result.hash);
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer) {
    const room = this.roomFor(ws);
    if (!room) return ws.close(4400, "unknown socket");
    await room.socketMessage(new WsSocket(ws), typeof message === "string" ? message : new Uint8Array(message));
  }

  async webSocketClose(ws: WebSocket, code: number) {
    this.roomFor(ws)?.socketClose(new WsSocket(ws), code);
    try {
      ws.close(code === 1005 || code === 1006 ? 1000 : code, "closing");
    } catch {
      /* already closed */
    }
  }

  async webSocketError(ws: WebSocket) {
    this.roomFor(ws)?.socketClose(new WsSocket(ws));
  }

  // ---- RPC used by the Worker's RelayBackend ----
  pair(deviceId: string, req: PairRequest): Promise<Outcome<PairResult>> {
    return this.room(deviceId).pair(req);
  }
  command(deviceId: string, token: string, name: string, args: unknown): Promise<Outcome> {
    return this.room(deviceId).command(token, name, args);
  }
  job(deviceId: string, token: string, job: string, waitMs: number): Promise<Outcome<JobView>> {
    return this.room(deviceId).job(token, job, waitMs);
  }
  revoke(deviceId: string, token: string): Promise<Outcome> {
    return this.room(deviceId).revoke(token);
  }
  events(deviceId: string, token: string, req: EventsRequest): Promise<Outcome> {
    return this.room(deviceId).events(token, req);
  }
  webhook(deviceId: string, token: string, req: WebhookRequest): Promise<Outcome> {
    return this.room(deviceId).webhook(token, req);
  }
  talk(deviceId: string, token: string, req: unknown): Promise<Outcome> {
    return this.room(deviceId).talk(token, req);
  }
  ask(deviceId: string, token: string, req: unknown): Promise<Outcome> {
    return this.room(deviceId).ask(token, req);
  }
  updates(deviceId: string, token: string): Promise<Outcome> {
    return this.room(deviceId).updates(token);
  }
  update(deviceId: string, token: string, req: unknown): Promise<Outcome> {
    return this.room(deviceId).update(token, req);
  }
  voiceSettings(deviceId: string, linkCode: string, req: VoiceOwnerRequest): Promise<Outcome> {
    return this.room(deviceId).voiceSettings(linkCode, req);
  }
  ownerActivity(deviceId: string, linkCode: string, req: ActivityOwnerRequest): Promise<Outcome> {
    return this.room(deviceId).ownerActivity(linkCode, req);
  }
  activityLog(deviceId: string, token: string, req: unknown): Promise<Outcome> {
    return this.room(deviceId).activityLog(token, req);
  }
  ownerAudio(deviceId: string, linkCode: string, job: string): Promise<Outcome<{ wav: Uint8Array }>> {
    return this.room(deviceId).ownerAudio(linkCode, job);
  }
  draft(deviceId: string, token: string, req: unknown): Promise<Outcome> {
    return this.room(deviceId).draft(token, req);
  }
  react(deviceId: string, token: string, req: unknown): Promise<Outcome> {
    return this.room(deviceId).react(token, req);
  }
  notes(deviceId: string, token: string, req: NotesRequest): Promise<Outcome> {
    return this.room(deviceId).notes(token, req);
  }
  speechStats(deviceId: string, linkCode: string, req: { action: "get" | "reset" }): Promise<Outcome> {
    return this.room(deviceId).speechStats(linkCode, req);
  }
  phrases(deviceId: string, token: string, req: unknown): Promise<Outcome> {
    return this.room(deviceId).phrases(token, req);
  }

  // ---- the `releases` singleton (never a device room) ----
  releaseLookup(mode: LookupMode): Promise<ReleaseLookup> {
    this.#releases ??= releaseCache(this.env, this.ctx.storage);
    return this.#releases.latest(mode);
  }

  // Webhook deliveries and retries (DeviceRoom.alarm).
  async alarm() {
    const id = await this.ctx.storage.get<string>("room_id");
    if (id) await this.room(id).alarm();
  }
}
