#!/usr/bin/env node
// Local end-to-end run. Nothing leaves this machine: `wrangler dev --local`
// (workerd + local Durable Object storage), a fake device, and scripted agents.
//
//   node tools/e2e.mjs [--go-cli /path/to/molty]
//
// Phase 1  VOICE_PROVIDER=none: fake device pairs, agent-e2e drives every MCP
//          tool and REST command (incl. say/listen jobs), a wrong-key device
//          gets 4401, optional Go CLI run (HOME redirected to a temp dir).
// Phase 2  restart on the same storage with VOICE_PROVIDER=workers-ai: the
//          device reconnects, the phase-1 token still works (DO storage
//          persisted), and say fails cleanly because Workers AI is remote-only.
import { spawn, spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import net from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

const { values: o } = parseArgs({ options: { "go-cli": { type: "string" }, keep: { type: "boolean", default: false } } });
const root = fileURLToPath(new URL("..", import.meta.url));
const tmp = mkdtempSync(join(process.env.E2E_TMP ?? tmpdir(), "molty-relay-e2e-"));
const DEVICE = "molty-k7m2q9xa4c";
const children = [];
let failed = false;

const freePort = () =>
  new Promise((resolve) => {
    const s = net.createServer().listen(0, "127.0.0.1", () => {
      const p = s.address().port;
      s.close(() => resolve(p));
    });
  });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const section = (t) => console.log(`\n=== ${t} ===`);

function run(cmd, args, opts = {}) {
  const child = spawn(cmd, args, { cwd: root, stdio: ["ignore", "pipe", "pipe"], ...opts });
  children.push(child);
  child.lines = [];
  child.stdout.setEncoding("utf8").on("data", (d) => {
    for (const line of d.split("\n").filter(Boolean)) {
      child.lines.push(line);
      if (opts.echo) console.log(`${opts.echo} ${line}`);
    }
  });
  child.stderr.setEncoding("utf8").on("data", (d) => opts.echoErr && process.stderr.write(d));
  child.done = new Promise((resolve) => child.on("exit", (code) => resolve(code)));
  return child;
}

async function waitFor(pred, ms, what) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    if (await pred()) return;
    await sleep(100);
  }
  throw new Error(`timed out waiting for ${what}`);
}

const events = (dev) => dev.lines.map((l) => JSON.parse(l));

async function startRelay(port, voice) {
  const w = run("./node_modules/.bin/wrangler", ["dev", "--local", "--ip", "127.0.0.1", "--port", String(port), "--persist-to", join(tmp, "state"), "--var", `VOICE_PROVIDER:${voice}`, "--log-level", "warn"], {
    env: { ...process.env, CI: "1", WRANGLER_SEND_METRICS: "false" },
    echoErr: true,
  });
  await waitFor(async () => (await fetch(`http://127.0.0.1:${port}/healthz`).catch(() => null))?.ok, 60000, "wrangler dev");
  return w;
}

async function stop(child) {
  if (child.exitCode !== null) return;
  child.kill("SIGTERM");
  await Promise.race([child.done, sleep(5000)]);
  if (child.exitCode === null) child.kill("SIGKILL");
}

function goCli(port, linkFile) {
  section("Go CLI (molty --relay) against the local relay");
  const home = join(tmp, "home");
  const env = { ...process.env, HOME: home, MOLTY_RELAY: "", MOLTY_DEVICE: "", MOLTY_TOKEN: "" };
  const relay = `http://127.0.0.1:${port}`;
  const steps = [
    ["pair", "--relay", relay, "--device", DEVICE, "--code", readFileSync(linkFile, "utf8").trim(), "--name", "go-cli", "--json"],
    ["--json", "status"],
    ["--json", "mood", "happy"],
    ["--json", "show", "hello from go"],
    ["--json", "volume", "6"],
    ["--json", "imu"],
    ["--json", "diag", "--timeline", "5"],
    ["--json", "log", "--since", "0"],
    ["--json", "say", "Hello from the Go CLI"],
    ["--json", "listen", "--ms", "1000"],
  ];
  for (const args of steps) {
    const r = spawnSync(o["go-cli"], args, { env, encoding: "utf8", timeout: 150000 });
    const out = (r.stdout || r.stderr).trim().replace(/"agent_token":"[^"]+"/, '"agent_token":"<redacted>"');
    console.log(`${r.status === 0 ? "ok " : "ERR"} molty ${args.join(" ").replace(relay, "<relay>")}\n    ${out.split("\n").join("\n    ")}`);
    if (r.status !== 0) failed = true;
  }
  const saved = JSON.parse(readFileSync(join(home, ".config", "molty", "link.json"), "utf8"));
  console.log(`    saved link: relay=${saved.relay} device_id=${saved.device_id} token=<${saved.agent_token.length} chars>`);
}

try {
  const port = await freePort();
  const keyFile = join(tmp, "device.key");
  const linkFile = join(tmp, "link");
  const tokenFile = join(tmp, "token");

  section(`phase 1: wrangler dev --local on :${port}, VOICE_PROVIDER=none`);
  let relay = await startRelay(port, "none");
  const dev = run("node", ["tools/fake-device.mjs", "--relay", `ws://127.0.0.1:${port}`, "--id", DEVICE, "--key-file", keyFile, "--link-file", linkFile]);
  await waitFor(() => events(dev).some((e) => e.event === "welcome"), 15000, "device welcome");
  console.log("ok  fake device connected and welcomed");

  section("scripted agent (MCP via official SDK client + REST)");
  const agent = run("node", ["tools/agent-e2e.mjs", "--relay", `http://127.0.0.1:${port}`, "--device", DEVICE, "--link-file", linkFile, "--token-out", tokenFile], { echo: "  ", echoErr: true });
  const agentCode = await agent.done;
  if (agentCode !== 0 || !agent.lines.some((l) => l.startsWith("AGENT E2E PASSED"))) throw new Error(`agent failed (exit ${agentCode})`);

  section("device-side audio checks");
  const ev = events(dev);
  const says = ev.filter((e) => e.event === "say.audio");
  const bad = ev.filter((e) => ["audio.bad_frame", "audio.seq_gap", "audio.unexpected"].includes(e.event));
  console.log(`    say streams received: ${says.length}; e.g. ${JSON.stringify(says[0])}`);
  console.log(`    listen streams sent: ${ev.filter((e) => e.event === "listen.audio").length}; pair requests shown: ${JSON.stringify(ev.filter((e) => e.event === "pair.request").map((e) => e.agent_name))}`);
  if (says.length < 3 || bad.length) throw new Error(`audio problems: ${JSON.stringify(bad)}`);
  if (!says.every((s) => s.played_ms === s.announced_ms)) throw new Error("say: streamed audio length != announced ms");
  console.log("ok  every say stream complete, in order, frames within limits, length == announced ms");

  section("wrong device key -> 4401");
  const evil = run("node", ["tools/fake-device.mjs", "--relay", `ws://127.0.0.1:${port}`, "--id", DEVICE, "--once", "--key", Buffer.alloc(32, 1).toString("base64")]);
  await evil.done;
  const closeEv = events(evil).find((e) => e.event === "close");
  if (closeEv?.code !== 4401) throw new Error(`expected 4401, got ${JSON.stringify(closeEv)}`);
  console.log(`ok  impostor closed with ${closeEv.code} (${closeEv.reason}); welcomed: ${events(evil).some((e) => e.event === "welcome")}`);
  await waitFor(async () => {
    const r = await fetch(`http://127.0.0.1:${port}/v1/devices/${DEVICE}/cmd`, { method: "POST", headers: { authorization: `Bearer ${readFileSync(tokenFile, "utf8")}` }, body: JSON.stringify({ name: "imu" }) });
    return r.ok;
  }, 5000, "real device still reachable");
  console.log("ok  real device still serving after the impostor attempt");

  if (o["go-cli"]) goCli(port, linkFile);

  section("phase 2: restart on the same storage, VOICE_PROVIDER=workers-ai");
  await stop(relay);
  relay = await startRelay(port, "workers-ai");
  const welcomes = () => events(dev).filter((e) => e.event === "welcome").length;
  const before = welcomes();
  await waitFor(() => welcomes() > before, 20000, "device reconnect");
  console.log("ok  device reconnected with its stored key (TOFU record persisted)");
  const token = readFileSync(tokenFile, "utf8");
  const call = async (name, args) =>
    (await fetch(`http://127.0.0.1:${port}/v1/devices/${DEVICE}/cmd`, { method: "POST", headers: { authorization: `Bearer ${token}` }, body: JSON.stringify({ name, args }) })).json();
  const status = await call("status", {});
  if (!status.ok || status.result.relay.voice !== "workers-ai") throw new Error(JSON.stringify(status));
  console.log(`ok  phase-1 MCP token still valid after restart; voice=${status.result.relay.voice}`);
  const say = await call("say", { text: "remote only" });
  let job;
  for (let i = 0; i < 50; i++) {
    job = await (await fetch(`http://127.0.0.1:${port}/v1/jobs/${say.result.job}`, { headers: { authorization: `Bearer ${token}` } })).json();
    if (job.status === "done" || job.status === "failed") break;
    await sleep(200);
  }
  console.log(`    say job: ${JSON.stringify(job)}`);
  if (job.status !== "failed" || !/remote/i.test(job.error)) throw new Error("expected workers-ai say to fail with the remote-binding error");
  console.log("ok  workers-ai locally fails cleanly as a failed job (needs a deployed/remote binding)");

  console.log("\nE2E PASSED");
} catch (e) {
  failed = true;
  console.error(`\nE2E FAILED: ${e.stack || e}`);
} finally {
  for (const c of children) await stop(c);
  if (!o.keep) rmSync(tmp, { recursive: true, force: true });
  process.exit(failed ? 1 : 0);
}
