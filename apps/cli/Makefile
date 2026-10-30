GO      ?= go
GOFMT   ?= gofmt
# Product version from the repo-root VERSION file (see tools/scripts/version.mjs).
VERSION ?= $(shell cat ../../VERSION)
LDFLAGS := -X github.com/molty/cli/internal/molty.Version=$(VERSION)

.PHONY: build test fmt

# bin/molty: the device CLI + stdio MCP server. No network needed to build.
build:
	mkdir -p bin
	$(GO) build -ldflags "$(LDFLAGS)" -o bin/molty ./cmd/molty

test:
	$(GO) build ./... && $(GO) vet ./... && $(GO) test ./...

fmt:
	$(GOFMT) -w $(shell find . -name '*.go' | sort)
