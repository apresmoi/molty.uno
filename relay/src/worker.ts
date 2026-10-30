// Worker entry: routing + the Durable Object-backed RelayBackend. The MCP and
// REST surfaces run here (stateless); per-device state lives in MoltyDevice.
import type { AgentApi, RelayBackend } from "./core/api.js";
import { DEVICE_ID_RE } from "./core/protocol.js";
import { healthResponse } from "./core/version.js";
import { type Env, MoltyDevice } from "./device-object.js";
import { handleMcp } from "./surfaces/mcp.js";
import { handleRest } from "./surfaces/rest.js";
import { SKILL_PATHS, handleSkill } from "./surfaces/skill.js";
import { WAITLIST_PATH, handleWaitlist } from "./surfaces/waitlist.js";
import { D1WaitlistStore, turnstileCheck } from "./waitlist-host.js";

export { MoltyDevice };

interface WorkerEnv extends Env {
  /** D1 database molty-waitlist (migrations/); absent = the waitlist answers 503 */
  WAITLIST?: D1Database;
  /** optional secret: when set, signups must pass Cloudflare Turnstile */
  TURNSTILE_SECRET?: string;
}

interface DeviceRpc {
  pair(id: string, req: Parameters<AgentApi["pair"]>[0]): ReturnType<AgentApi["pair"]>;
  command(id: string, token: string, name: string, args: unknown): ReturnType<AgentApi["command"]>;
  job(id: string, token: string, job: string, waitMs: number): ReturnType<AgentApi["job"]>;
  revoke(id: string, token: string): ReturnType<AgentApi["revoke"]>;
  events(id: string, token: string, req: Parameters<AgentApi["events"]>[1]): ReturnType<AgentApi["events"]>;
  webhook(id: string, token: string, req: Parameters<AgentApi["webhook"]>[1]): ReturnType<AgentApi["webhook"]>;
  talk(id: string, token: string, req: unknown): ReturnType<AgentApi["talk"]>;
  ask(id: string, token: string, req: unknown): ReturnType<AgentApi["ask"]>;
}

function backendFor(env: Env): RelayBackend {
  return {
    device(id: string): AgentApi {
      // Typed as the plain RPC shape: the generated DO stub type recurses too deep for tsc.
      const stub = env.DEVICE.get(env.DEVICE.idFromName(id)) as unknown as DeviceRpc;
      return {
        pair: (req) => stub.pair(id, req),
        command: (token, name, args) => stub.command(id, token, name, args),
        job: (token, job, waitMs) => stub.job(id, token, job, waitMs),
        revoke: (token) => stub.revoke(id, token),
        // An AbortSignal does not cross DO RPC: strip it (the room frees the wait at its deadline).
        events: (token, req) => stub.events(id, token, { since: req.since, wait_ms: req.wait_ms }),
        webhook: (token, req) => stub.webhook(id, token, req),
        talk: (token, req) => stub.talk(id, token, req),
        ask: (token, req) => stub.ask(id, token, req),
      };
    },
  };
}

export default {
  async fetch(request: Request, env: WorkerEnv): Promise<Response> {
    const url = new URL(request.url);
    if (url.pathname === WAITLIST_PATH) {
      if (!env.WAITLIST) return Response.json({ ok: false, error: "unavailable" }, { status: 503 });
      return handleWaitlist(request, {
        store: new D1WaitlistStore(env.WAITLIST),
        ip: request.headers.get("cf-connecting-ip") ?? undefined,
        humanCheck: env.TURNSTILE_SECRET ? turnstileCheck(env.TURNSTILE_SECRET) : undefined,
      });
    }
    if (url.pathname === "/v1/device") {
      const id = url.searchParams.get("id") ?? "";
      if (request.headers.get("upgrade")?.toLowerCase() !== "websocket") return new Response("expected websocket upgrade", { status: 426 });
      if (!DEVICE_ID_RE.test(id)) return new Response("bad device id", { status: 400 });
      return env.DEVICE.get(env.DEVICE.idFromName(id)).fetch(request);
    }
    if (SKILL_PATHS.includes(url.pathname)) return handleSkill(request);
    if (url.pathname === "/mcp") return handleMcp(request, backendFor(env));
    if (url.pathname === "/v1/health") return healthResponse();
    if (url.pathname.startsWith("/v1/")) return handleRest(request, backendFor(env));
    if (url.pathname === "/" || url.pathname === "/healthz") {
      return new Response("molty-relay ok\n", { headers: { "content-type": "text/plain" } });
    }
    return new Response("not found", { status: 404 });
  },
} satisfies ExportedHandler<WorkerEnv>;
