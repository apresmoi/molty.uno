// Cloudflare host for one DeviceRoom: a Durable Object per device_id with
// hibernatable WebSockets. All logic is in src/core; this file only adapts
// storage, sockets and RPC. Hibernation rule: the authenticated-device flag
// lives in each socket's serialized attachment (SocketMeta), never here.
import { DurableObject } from "cloudflare:workers";
import type { EventsRequest, JobView, Outcome, PairRequest, PairResult, WebhookRequest } from "./core/api.js";
import { PING_TEXT, PONG_TEXT } from "./core/protocol.js";
import { type DeviceSocket, DeviceRoom, type SocketMeta } from "./core/room.js";
import { type KV, realClock } from "./core/store.js";
import { dohResolver } from "./core/webhook.js";
import { createVoice, type VoiceEnv, type VoiceProvider } from "./voice/index.js";

export interface Env extends VoiceEnv {
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
      // Workers have no socket-level resolver: DNS-over-HTTPS for the SSRF guard (relay/README.md "Webhooks").
      resolveHost: dohResolver(fetcher),
    });
    return this.#room;
  }

  private roomFor(ws: WebSocket): DeviceRoom | undefined {
    const att = ws.deserializeAttachment() as Attachment | null;
    return att?.device ? this.room(att.device) : undefined;
  }

  async fetch(request: Request): Promise<Response> {
    const id = new URL(request.url).searchParams.get("id") ?? "";
    const pair = new WebSocketPair();
    const [client, server] = [pair[0], pair[1]];
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ device: id } satisfies Attachment);
    this.room(id).socketOpen(new WsSocket(server));
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer) {
    const room = this.roomFor(ws);
    if (!room) return ws.close(4400, "unknown socket");
    await room.socketMessage(new WsSocket(ws), typeof message === "string" ? message : new Uint8Array(message));
  }

  async webSocketClose(ws: WebSocket, code: number) {
    this.roomFor(ws)?.socketClose(new WsSocket(ws));
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

  // Webhook deliveries and retries (DeviceRoom.alarm).
  async alarm() {
    const id = await this.ctx.storage.get<string>("room_id");
    if (id) await this.room(id).alarm();
  }
}
