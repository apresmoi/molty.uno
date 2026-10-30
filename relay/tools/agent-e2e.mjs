#!/usr/bin/env node
// Scripted agent for local e2e: pairs, then calls every tool over remote MCP
// (official @modelcontextprotocol/sdk client, Streamable HTTP) and every
// command over REST, including say/listen jobs via molty_wait / GET /v1/jobs.
//
//   node tools/agent-e2e.mjs --relay http://127.0.0.1:8787 --device molty-k7m2q9xa4c --link-file /tmp/link
import assert from "node:assert/strict";
import { readFileSync, writeFileSync } from "node:fs";
import { parseArgs } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const { values: o } = parseArgs({
  options: {
    relay: { type: "string", default: "http://127.0.0.1:8787" },
    device: { type: "string", default: "molty-k7m2q9xa4c" },
    "link-file": { type: "string" },
    "token-out": { type: "string" },
    "expect-stt": { type: "string", default: "[stt unavailable]" },
  },
});
const relay = o.relay.replace(/\/$/, "");
const link = () => readFileSync(o["link-file"], "utf8").trim();
const log = (step, data) => console.log(`ok  ${step}${data === undefined ? "" : "  " + JSON.stringify(data)}`);
let checks = 0;
const check = (cond, msg) => {
  assert.ok(cond, msg);
  checks++;
};

async function mcpClient(token) {
  const client = new Client({ name: "molty-e2e-agent", version: "0.0.1" });
  const transport = new StreamableHTTPClientTransport(new URL(`${relay}/mcp`), token ? { requestInit: { headers: { authorization: `Bearer ${token}` } } } : {});
  await client.connect(transport);
  return client;
}
const sc = (r) => r.structuredContent;

async function waitJob(client, job) {
  for (let i = 0; i < 10; i++) {
    const r = await client.callTool({ name: "molty_wait", arguments: { job, timeout_s: 8 } });
    if (r.isError) return r;
    if (sc(r).status !== "pending") return r;
  }
  throw new Error(`job ${job} never finished`);
}

// ---------------------------------------------------------------- MCP
const anon = await mcpClient();
check(anon.getServerVersion()?.name === "molty-relay", "serverInfo");
const tools = (await anon.listTools()).tools.map((t) => t.name).sort();
check(tools.length === 14, `14 tools, got ${tools}`);
log("mcp initialize + tools/list", tools);

const denied = await anon.callTool({ name: "molty_status", arguments: {} });
check(denied.isError && sc(denied).error === "unauthorized", "status without token is unauthorized");
log("mcp molty_status without token -> unauthorized");

const badCode = await anon.callTool({ name: "molty_pair", arguments: { device_id: o.device, link_code: "WRNG2345", agent_name: "e2e" } });
check(badCode.isError && sc(badCode).error === "bad_link_code", "bad link code rejected");
log("mcp molty_pair wrong code -> bad_link_code");

let pair = await anon.callTool({ name: "molty_pair", arguments: { device_id: o.device, link_code: link(), agent_name: "e2e-mcp", wait_s: 0 } });
check(!pair.isError && sc(pair).status === "pending" && /^\d{4}$/.test(sc(pair).code), "pending pair with code");
log("mcp molty_pair wait_s=0 -> pending", { code: sc(pair).code });
pair = await anon.callTool({ name: "molty_pair", arguments: { device_id: o.device, pair_id: sc(pair).pair_id, wait_s: 8 } });
check(!pair.isError && sc(pair).status === "approved", `approved: ${JSON.stringify(sc(pair))}`);
const token = sc(pair).agent_token;
check(/^molty-[a-z2-9]{10}\.[A-Za-z0-9_-]{43}$/.test(token), "token format");
log("mcp molty_pair resume -> approved", { device_id: sc(pair).device_id });
await anon.close();
if (o["token-out"]) writeFileSync(o["token-out"], token, { mode: 0o600 });

const mcp = await mcpClient(token);
const call = async (name, args = {}) => {
  const r = await mcp.callTool({ name, arguments: args });
  check(!r.isError, `${name} failed: ${JSON.stringify(sc(r))}`);
  log(`mcp ${name}`, sc(r));
  return sc(r);
};
const st = await call("molty_status");
check(st.fw === "fake-0.1" && st.relay.online === true && st.relay.device_id === o.device, "status merged");
check((await call("molty_mood", { mood: "happy" })).mood === "happy", "mood");
check((await call("molty_show", { text: "hello from the relay e2e", wpm: 400 })).wpm === 400, "show");
check((await call("molty_state")).creature === "fluid", "state read");
check((await call("molty_state", { state: { blink: 0.5 } })).blink === 0.5, "state patch");
check((await call("molty_volume", { volume: 9 })).volume === 9, "volume");
check(typeof (await call("molty_imu")).az === "number", "imu");
check((await call("molty_diag", { timeline: 10 })).frames.sim.fps === 30, "diag");
check((await call("molty_log", { since: 4 })).next === 5, "log");

const say = await call("molty_say", { text: "Hello, this is Molty speaking through the relay." });
const sayDone = await waitJob(mcp, say.job);
check(!sayDone.isError && sc(sayDone).status === "done" && sc(sayDone).ms > 0, `say done: ${JSON.stringify(sc(sayDone))}`);
log("mcp molty_wait(say)", sc(sayDone));

const busy1 = await call("molty_say", { text: "one two three four five six seven eight" });
const busy = await mcp.callTool({ name: "molty_listen", arguments: { ms: 500 } });
check(busy.isError && sc(busy).error === "audio_busy", "second audio job is audio_busy");
log("mcp molty_listen during say -> audio_busy");
await waitJob(mcp, busy1.job);

const listen = await call("molty_listen", { ms: 1500 });
const heard = await waitJob(mcp, listen.job);
check(!heard.isError && sc(heard).text === o["expect-stt"], `listen text: ${JSON.stringify(sc(heard))}`);
check(sc(heard).duration_ms === 1500 && sc(heard).peak > 0.25 && sc(heard).rms > 0.15, "listen levels from fake tone");
log("mcp molty_wait(listen)", sc(heard));

// events: long-poll wakes on a device evt; ride-along on the next tool result
const polled = mcp.callTool({ name: "molty_events", arguments: { wait_s: 8 } });
await call("molty_show", { text: "evt:shake" });
const got = await polled;
check(!got.isError && sc(got).events?.[0]?.type === "shake" && /^Molty: was shaken/.test(got.content[1]?.text ?? ""), `molty_events long-poll: ${JSON.stringify(got)}`);
log("mcp molty_events (long-poll) <- device evt shake", sc(got).events);
await call("molty_show", { text: "evt:tap" });
await new Promise((s) => setTimeout(s, 300));
const ride = await call("molty_imu");
check(ride.events?.[0]?.type === "tap", `ride-along tap: ${JSON.stringify(ride)}`);
check(!(await call("molty_imu")).events, "ride-along delivered once");
const hook = await call("molty_webhook", { url: "https://example.org/molty", headers: { "X-Key": "e2e-secret" } });
check(/^whsec_/.test(hook.secret) && !JSON.stringify(hook.dry_run).includes("e2e-secret"), "webhook registered, secret once, header redacted");
check((await call("molty_webhook", { remove: true })).removed === true, "webhook removed");
await mcp.close();

// ---------------------------------------------------------------- REST
const rest = async (method, path, body, tok) => {
  const res = await fetch(`${relay}${path}`, {
    method,
    headers: { "content-type": "application/json", ...(tok ? { authorization: `Bearer ${tok}` } : {}) },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  return { status: res.status, body: await res.json() };
};

const r401 = await rest("POST", `/v1/devices/${o.device}/cmd`, { name: "status" });
check(r401.status === 401 && r401.body.error === "unauthorized", "REST 401");
log("rest cmd without bearer -> 401");

const oldCode = await rest("POST", "/v1/pair", { device_id: o.device, link_code: "WRNG2345", agent_name: "e2e-rest" });
check(oldCode.status === 403 && oldCode.body.error === "bad_link_code", "REST bad code");
const p2 = await rest("POST", "/v1/pair", { device_id: o.device, link_code: link(), agent_name: "e2e-rest" });
check(p2.status === 200 && p2.body.device_id === o.device && p2.body.agent_token, `REST pair: ${JSON.stringify(p2)}`);
const t2 = p2.body.agent_token;
log("rest POST /v1/pair (long-poll) -> token", { device_id: p2.body.device_id });

for (const [name, args] of [
  ["status", {}],
  ["mood", { mood: "thinking" }],
  ["show", { text: "rest show" }],
  ["state", {}],
  ["state", { color: "#ff0000" }],
  ["volume", { volume: 4 }],
  ["imu", {}],
]) {
  const r = await rest("POST", `/v1/devices/${o.device}/cmd`, { name, args }, t2);
  check(r.status === 200 && r.body.ok === true && typeof r.body.result === "object", `${name}: ${JSON.stringify(r)}`);
  log(`rest cmd ${name}`, r.body.result);
}

const pollJob = async (job) => {
  for (let i = 0; i < 100; i++) {
    const r = await rest("GET", `/v1/jobs/${encodeURIComponent(job)}`, undefined, t2);
    check(r.status === 200, `job GET ${r.status}`);
    if (r.body.status === "done" || r.body.status === "failed") return r.body;
    check(["queued", "running"].includes(r.body.status), `status ${r.body.status}`);
    await new Promise((s) => setTimeout(s, 200));
  }
  throw new Error("job timeout");
};
const s2 = await rest("POST", `/v1/devices/${o.device}/cmd`, { name: "say", args: { text: "REST say test" } }, t2);
check(s2.status === 200 && typeof s2.body.result.job === "string", "REST say job");
const s2done = await pollJob(s2.body.result.job);
check(s2done.status === "done" && s2done.result.ms > 0, `REST say done ${JSON.stringify(s2done)}`);
log("rest say + GET /v1/jobs", s2done);
const l2 = await rest("POST", `/v1/devices/${o.device}/cmd`, { name: "listen", args: { ms: 800 } }, t2);
const l2done = await pollJob(l2.body.result.job);
check(l2done.status === "done" && l2done.result.text === o["expect-stt"], `REST listen ${JSON.stringify(l2done)}`);
log("rest listen + GET /v1/jobs", l2done);

await rest("POST", `/v1/devices/${o.device}/cmd`, { name: "show", args: { text: "evt:button" } }, t2);
const ev2 = await rest("GET", `/v1/devices/${o.device}/events?wait_s=5`, undefined, t2);
check(ev2.status === 200 && ev2.body.events.some((e) => e.type === "button"), `REST events: ${JSON.stringify(ev2)}`);
log("rest GET /v1/devices/<id>/events", ev2.body);

const rv = await rest("DELETE", "/v1/token", undefined, t2);
check(rv.status === 200, "revoke");
const after = await rest("POST", `/v1/devices/${o.device}/cmd`, { name: "status" }, t2);
check(after.status === 401, "revoked token is 401");
log("rest DELETE /v1/token -> later 401");

console.log(`AGENT E2E PASSED (${checks} checks)`);
