import { readFileSync } from "node:fs";
import { defineConfig } from "vitest/config";

// Same constant tools/deploy.mjs bakes into the deployed bundle.
const VERSION = readFileSync(new URL("../../VERSION", import.meta.url), "utf8").trim();

export default defineConfig({
  define: { __MOLTY_VERSION__: JSON.stringify(VERSION) },
  test: { include: ["test/**/*.test.ts"], environment: "node" },
});
