// SPDX-License-Identifier: MIT
package molty

// Version is the product version, stamped at build time from the repo-root VERSION
// file: go build -ldflags "-X github.com/molty/cli/internal/molty.Version=$(cat ../../VERSION)"
// (apps/cli/Makefile and the release workflow do this). A plain `go build` reports "dev".
var Version = "dev"
