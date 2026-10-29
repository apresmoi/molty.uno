-- SPDX-License-Identifier: MIT
-- The "want one built?" waitlist (src/core/waitlist.ts). Personal data is the address only:
-- no IP, no user agent. Apply: npx wrangler d1 migrations apply molty-waitlist --remote
CREATE TABLE IF NOT EXISTS waitlist (
  email      TEXT PRIMARY KEY NOT NULL,   -- trimmed, lower-cased
  created_at TEXT NOT NULL,               -- ISO 8601, UTC
  source     TEXT NOT NULL DEFAULT 'site' -- which form: "build", ...
);

-- Per-IP rate limit, fixed one-hour windows. `key` is sha256 of the IP, and rows of past
-- windows are deleted on the next request, so nothing here outlives its hour for long.
CREATE TABLE IF NOT EXISTS waitlist_rate (
  key          TEXT PRIMARY KEY NOT NULL,
  window_start INTEGER NOT NULL,
  hits         INTEGER NOT NULL
);
