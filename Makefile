# Molty — build and test every component. `make help` lists the targets.
# Nothing here flashes a device or deploys anything.

IDF_PATH ?= $(HOME)/esp/esp-idf
export IDF_PATH

.PHONY: help all test console console-test console-e2e firmware firmware-all firmware-test \
        firmware-size relay relay-test cli cli-test creatures hygiene version \
        waitlist-export waitlist-delete sim sim-test sim-env voice-test

help:
	@echo "build:"
	@echo "  make console        device/console/dist/index.html (typecheck + unit tests + build)"
	@echo "  make firmware       idf.py build: the 1.46 in device/firmware/build (needs 'make console' first; never flashes)"
	@echo "  make firmware BOARD=waveshare/esp32-s3-touch-lcd-1.28"
	@echo "                      one board, in device/firmware/build/<name>/ (tools/scripts/firmware-build.sh)"
	@echo "  make firmware-all   every board under device/firmware/boards/ (each in device/firmware/build/<name>/)"
	@echo "  make cli            apps/cli/bin/molty"
	@echo "  make creatures      re-export device/creatures/molty/*.molty from the cute-kit specs"
	@echo "test:"
	@echo "  make firmware-test  firmware host tests (fluid, director, gestures, cute runtime, agent link)"
	@echo "  make relay-test     relay unit tests + typecheck"
	@echo "  make sim-test       simulated-Molty harness unit tests (tools/sim)"
	@echo "  make voice-test     voice-box bridge unit tests (services/molty-voice; no GPU, no network)"
	@echo "sim (tools/sim/README.md; needs ssh to the voice box, never touches a real device):"
	@echo "  make sim            the default conversation suite end to end; SIM_ARGS='--turn-end-hold 1.5' etc."
	@echo "  make cli-test       go build + go test"
	@echo "  make console-test   console unit tests + typecheck"
	@echo "  make console-e2e    headless walk-through against the mock device (after 'make console')"
	@echo "  make test           all of the above except console-e2e"
	@echo "checks:"
	@echo "  make firmware-size  app image vs its OTA slot (after 'make firmware'; fails > 90%)"
	@echo "  make hygiene        no secrets/local paths committed, licences present, versions agree"
	@echo "  make version        print the product version (VERSION; bump: node tools/scripts/version.mjs bump)"
	@echo "waitlist (the relay's D1 database on YOUR Cloudflare account; services/relay/README.md):"
	@echo "  make waitlist-export             every signup -> waitlist.csv (gitignored)"
	@echo "  make waitlist-delete EMAIL=a@b   delete one address (GDPR request)"

all: console firmware cli

test: firmware-test relay-test cli-test console-test

console:
	cd device/console && pnpm install --frozen-lockfile && pnpm run build

console-test:
	cd device/console && pnpm install --frozen-lockfile && pnpm run typecheck && pnpm test

console-e2e:
	cd device/console && { node tools/mock-device.mjs & pid=$$!; sleep 1; \
	  node tools/e2e.mjs; status=$$?; kill $$pid; exit $$status; }

# No BOARD: the plain idf.py build (the Waveshare 1.46) in device/firmware/build, as ever.
firmware:
ifeq ($(BOARD),)
	. "$(IDF_PATH)/export.sh" >/dev/null && cd device/firmware && idf.py build
else
	. "$(IDF_PATH)/export.sh" >/dev/null && tools/scripts/firmware-build.sh "$(BOARD)"
endif

firmware-all:
	. "$(IDF_PATH)/export.sh" >/dev/null && tools/scripts/firmware-build.sh --all

# _GNU_SOURCE: the host-test SDK stubs use strdup, which glibc hides under -std=c11
# (macOS does not), so Linux/CI needs it.
firmware-test:
	LINK_TEST_CFLAGS="-D_GNU_SOURCE $(LINK_TEST_CFLAGS)" device/firmware/test/host/run_all.sh

firmware-size:
	tools/scripts/firmware-size.sh

relay:
	cd services/relay && pnpm install --frozen-lockfile

relay-test: relay
	cd services/relay && pnpm test && pnpm typecheck

# Simulated Molty (tools/sim): its own venv (uv, Python 3.12), molty-voice installed from services/molty-voice/.
sim-env:
	cd tools/sim && { test -x .venv/bin/python || uv venv -q -p 3.12 .venv; } && uv pip install -q -p .venv/bin/python -e '.[test]'

sim-test: sim-env
	cd tools/sim && .venv/bin/python -m pytest -q

# molty-voice's own tests, in the sim venv (it installs molty-voice editable from services/).
voice-test: sim-env
	cd services/molty-voice && ../../tools/sim/.venv/bin/python -m pytest -q

sim: relay sim-env
	cd tools/sim && { test ! -f local.env || { set -a; . ./local.env; set +a; }; } && .venv/bin/molty-sim run $(SIM_ARGS)

cli:
	$(MAKE) -C apps/cli build

cli-test:
	$(MAKE) -C apps/cli test

creatures:
	node device/creatures/tools/molty-export.mjs all

hygiene:
	tools/scripts/hygiene.sh

version:
	@node tools/scripts/version.mjs

# The website's waitlist lives in the relay's D1 database (services/relay/README.md "Waitlist").
# Both talk to the REMOTE database on the account wrangler is logged into: check
# `npx wrangler whoami` first. waitlist.csv holds personal data and is gitignored.
WAITLIST_DB ?= molty-waitlist

waitlist-export: relay
	@test -n "$(CLOUDFLARE_ACCOUNT_ID)" || { echo "set CLOUDFLARE_ACCOUNT_ID to the account that holds molty-waitlist (and its wrangler login)"; exit 2; }
	cd services/relay && pnpm exec wrangler d1 execute $(WAITLIST_DB) --remote --json \
	  --command "SELECT email, created_at, source FROM waitlist ORDER BY created_at" \
	| node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{const rows=JSON.parse(s)[0].results;const q=v=>`"$${String(v).replace(/"/g,"\"\"")}"`;process.stdout.write(["email,created_at,source",...rows.map(r=>[r.email,r.created_at,r.source].map(q).join(","))].join("\n")+"\n");console.error(`$${rows.length} signups -> waitlist.csv`)})' \
	> ../../waitlist.csv

waitlist-delete: relay
	@test -n "$(CLOUDFLARE_ACCOUNT_ID)" || { echo "set CLOUDFLARE_ACCOUNT_ID to the account that holds molty-waitlist (and its wrangler login)"; exit 2; }
	@case "$(EMAIL)" in ""|*"'"*|*" "*) echo "usage: make waitlist-delete EMAIL=someone@example.org"; exit 2;; esac
	cd services/relay && pnpm exec wrangler d1 execute $(WAITLIST_DB) --remote \
	  --command "DELETE FROM waitlist WHERE email = lower(trim('$(EMAIL)'))"
