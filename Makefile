# Molty — build and test every component. `make help` lists the targets.
# Nothing here flashes a device or deploys anything.

IDF_PATH ?= $(HOME)/esp/esp-idf
export IDF_PATH

.PHONY: help all test console console-test console-e2e firmware firmware-test \
        firmware-size relay relay-test cli cli-test creatures hygiene version

help:
	@echo "build:"
	@echo "  make console        console/dist/index.html (typecheck + unit tests + build)"
	@echo "  make firmware       idf.py build (needs 'make console' first; never flashes)"
	@echo "  make cli            cli/bin/molty"
	@echo "  make creatures      re-export creatures/molty/*.molty from the cute-kit specs"
	@echo "test:"
	@echo "  make firmware-test  firmware host tests (fluid, cute runtime, agent link)"
	@echo "  make relay-test     relay unit tests + typecheck"
	@echo "  make cli-test       go build + go test"
	@echo "  make console-test   console unit tests + typecheck"
	@echo "  make console-e2e    headless walk-through against the mock device (after 'make console')"
	@echo "  make test           all of the above except console-e2e"
	@echo "checks:"
	@echo "  make firmware-size  app image vs its OTA slot (after 'make firmware'; fails > 90%)"
	@echo "  make hygiene        no secrets/local paths committed, licences present, versions agree"
	@echo "  make version        print the product version (VERSION; bump: node scripts/version.mjs bump)"

all: console firmware cli

test: firmware-test relay-test cli-test console-test

console:
	cd console && pnpm install --frozen-lockfile && pnpm run build

console-test:
	cd console && pnpm install --frozen-lockfile && pnpm run typecheck && pnpm test

console-e2e:
	cd console && { node tools/mock-device.mjs & pid=$$!; sleep 1; \
	  node tools/e2e.mjs; status=$$?; kill $$pid; exit $$status; }

firmware:
	. "$(IDF_PATH)/export.sh" >/dev/null && cd firmware && idf.py build

# _GNU_SOURCE: the host-test SDK stubs use strdup, which glibc hides under -std=c11
# (macOS does not), so Linux/CI needs it.
firmware-test:
	LINK_TEST_CFLAGS="-D_GNU_SOURCE $(LINK_TEST_CFLAGS)" firmware/test/host/run_all.sh

firmware-size:
	scripts/firmware-size.sh

relay:
	cd relay && pnpm install --frozen-lockfile

relay-test: relay
	cd relay && pnpm test && pnpm typecheck

cli:
	$(MAKE) -C cli build

cli-test:
	$(MAKE) -C cli test

creatures:
	node creatures/tools/molty-export.mjs all

hygiene:
	scripts/hygiene.sh

version:
	@node scripts/version.mjs
