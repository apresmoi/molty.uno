# Releasing

One version covers the whole product — firmware, console, relay and CLI. It lives in
the root [`VERSION`](../VERSION) file and is read at build time everywhere:

| Component | Where the version shows up | How it gets there |
|---|---|---|
| firmware | `GET /api/info` → `fw` (and the agent link's `fw`) | `PROJECT_VER` in `device/firmware/CMakeLists.txt` |
| console | status page, `console` row | Vite `define` → `__MOLTY_VERSION__` |
| CLI | `molty --version`, MCP `serverInfo` | `-ldflags -X …/internal/molty.Version` (`make cli`) |
| relay | `GET /v1/health` (`version`, `x-molty-version`), MCP `serverInfo` | `wrangler deploy --define` (`pnpm run deploy`) |

`device/console/package.json` and `services/relay/package.json` carry copies; `node tools/scripts/version.mjs
check` (run by CI's hygiene job) fails when they drift.

## Cut a release

1. Make sure `main` is green in CI.
2. Write the release notes under `## Unreleased` in [`CHANGELOG.md`](../CHANGELOG.md).
3. Bump (rewrites `VERSION`, the package.json copies, and turns `## Unreleased` into
   `## vX.Y.Z — <date>`):
   ```sh
   node tools/scripts/version.mjs bump patch      # or minor | major | 1.2.3 | 1.2.3-rc.1
   make test && make console firmware-all   # optional local check (every board)
   ```
4. Commit, tag and push:
   ```sh
   v="$(node tools/scripts/version.mjs)"
   git commit -am "chore(release): v$v"
   git tag "v$v"
   git push origin main "v$v"
   ```
5. The tag runs [`release.yml`](../.github/workflows/release.yml): it refuses a tag that
   does not equal `VERSION` or has no `CHANGELOG.md` section, builds everything from the
   tag, and publishes a GitHub Release whose notes are that section. A version with a
   `-suffix` is published as a pre-release. To rebuild an existing tag: Actions →
   Release → Run workflow → `tag`.

### Release assets

| Asset | What it is |
|---|---|
| `molty-X.Y.Z-<board>-ota.bin` | App image only, per board (e.g. `waveshare-esp32-s3-touch-lcd-1.46`). Updates a running device: `device/firmware/ota.sh` or `POST /api/ota`, which refuses another board's image. |
| `molty-X.Y.Z-<board>-factory.bin` | Bootloader + partition table + otadata + app + console (SPIFFS), merged; flash at `0x0`. |
| `molty-X.Y.Z-console.html` | The console page (also inside both firmware images). |
| `molty-X.Y.Z-{linux,darwin}-{amd64,arm64}.tar.gz`, `…-windows-{amd64,arm64}.zip` | `molty` CLI, with `LICENSE` and `THIRD_PARTY_NOTICES.md`. |
| `LICENSE`, `THIRD_PARTY_NOTICES.md`, `LICENSE-Apache-2.0.txt` | Licences that must accompany the firmware binaries (ESP-IDF is Apache-2.0). |
| `SHA256SUMS` | Checksums of everything above: `sha256sum -c SHA256SUMS`. |

The asset names and `SHA256SUMS` are a contract with the relay's update check
([AGENT-LINK.md "Firmware updates"](AGENT-LINK.md#firmware-updates)): it reads the latest
non-prerelease GitHub Release of `RELEASES_REPO`, takes `molty-X.Y.Z-<board>-ota.bin` per board
(`<board>` = the name the device reports in its hello) and that file's line in `SHA256SUMS`,
and hands exactly that URL + sha256 to a device that updates itself. Renaming an asset, or
dropping `SHA256SUMS`, makes `molty_update` refuse (`no_build` / `no_checksum`). Agents and the
console learn about a release within about an hour of publishing it. A `-suffix` pre-release
is not offered unless the relay sets `RELEASES_PRERELEASE=true`.

## Flash a release

**First flash, or after a partition-table change: the factory image.** It covers the
flash from `0x0` to the end of SPIFFS, so it also erases NVS: Wi-Fi credentials and
agent pairings are wiped. Put the board in download mode as in
[HARDWARE.md](HARDWARE.md#first-flash-and-any-partition-table-change) (hold BOOT while
plugging in USB-C), then:

```sh
sha256sum -c SHA256SUMS --ignore-missing
python -m esptool --chip esp32s3 -p PORT -b 460800 \
  --before default_reset --after hard_reset write_flash 0x0 molty-X.Y.Z-<board>-factory.bin
```

Unplug and replug to boot. `esptool` ships with ESP-IDF, or `pip install esptool`.

**A device already running Molty: the OTA image.** Keeps settings and pairings.

```sh
curl -fsS -m120 -X POST --data-binary @molty-X.Y.Z-<board>-ota.bin http://10.13.37.1/api/ota
```

or `./ota.sh` from `device/firmware/` for a local build. Check it landed:
`curl -s http://10.13.37.1/api/info` reports `"fw":"X.Y.Z"`.

## Deploy the relay

[`deploy-relay.yml`](../.github/workflows/deploy-relay.yml) runs `pnpm run deploy` in
`services/relay/` (wrangler with the version baked in). Run it from Actions → Deploy Relay with a
`ref` (tag, branch or SHA); `dry_run` bundles without deploying and needs no secrets.
It runs after a release too when the repository variable `DEPLOY_RELAY_ON_RELEASE` is
`true`.

| Name | Kind | Value |
|---|---|---|
| `CLOUDFLARE_API_TOKEN` | secret | Cloudflare API token with **Workers Scripts: Edit** on the account |
| `CLOUDFLARE_ACCOUNT_ID` | secret | the Cloudflare account that owns the worker |
| `RELAY_URL` | variable (optional) | public relay origin; the deploy then waits for `/v1/health` to report the new version |
| `DEPLOY_RELAY_ON_RELEASE` | variable (optional) | `true` to deploy the relay after every release |

The relay's own settings for the update check live in `services/relay/wrangler.jsonc`: `RELEASES_REPO`
(var, the repository whose Releases are Molty's), `RELEASES_PRERELEASE` (var, optional) and the
`RELEASES_RATE` rate limit on the public `GET /v1/releases/latest`. An optional worker secret
`GITHUB_TOKEN` (`wrangler secret put GITHUB_TOKEN`, a token with no scopes is enough for a public
repo) raises GitHub's API rate limit; without it the relay still works (it asks about once an hour).

Store the secrets as repository secrets or on the `relay` environment (where you can also
require an approval). No account id is committed: `services/relay/wrangler.jsonc` has none.

Locally: `cd services/relay && pnpm run deploy` (uses your `wrangler login`), or
`pnpm run deploy --dry-run --outdir dist` to bundle only.

## Website

[`deploy-website.yml`](../.github/workflows/deploy-website.yml) builds `apps/website/` and
publishes it to GitHub Pages on pushes to `main` that touch the site, creatures, docs or
the console catalog. One-time setup: Settings → Pages → Source **GitHub Actions**, and
DNS for `molty.uno` pointing at GitHub Pages. The domain comes from
`apps/website/public/CNAME`; the workflow fails if the built site lacks it.

## CI

[`ci.yml`](../.github/workflows/ci.yml) runs on every pull request and push to `main`:
hygiene (no secrets or local paths, licences present, versions agree), console
(typecheck, unit tests, build, Playwright walk-through), firmware (one ESP-IDF v5.5 build
per `device/firmware/boards/**/board.json` in the pinned `espressif/idf` image, each checked for
its image's board tag and app size ≤ 90% of its OTA slot; `tools/scripts/firmware-boards.py
check` first), firmware host tests (fluid goldens, cute runtime, agent link), relay
(typecheck, tests, bundle), CLI (gofmt, vet, tests, build), creatures (re-export must
match the committed `.molty` files) and website (check, build). Each has a `make`
target to run it locally (`make help`).

Actions are pinned by commit SHA and the ESP-IDF image by digest; bump them deliberately.
