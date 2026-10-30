# molty CLI

`molty` drives a Molty from a terminal or from an agent: speak, listen, show text,
set moods, read the IMU, pull diagnostics. It is also a stdio MCP server
(`molty mcp`) with the same verbs as tools.

It reaches the device two ways:

- **direct HTTP** on the same network: `--device http://molty.local` (or
  `http://10.13.37.1` over USB-C);
- **through the relay** from anywhere: `molty pair` once, then every command uses
  the saved link (`~/.config/molty/link.json`).

```sh
make build                     # -> bin/molty
make test                      # go build ./... && go test ./...

./bin/molty --device http://molty.local status
./bin/molty pair --device molty-xxxxxxxxxx --code K7M2W9C4 --name my-agent   # tap Molty to accept
./bin/molty say "hello"
```

`molty pair` uses the default relay (`DefaultRelay` in `internal/molty/link.go`)
unless `--relay` or `MOLTY_RELAY` is set. Full verb list, speech backends and MCP
setup: [docs/CLI.md](../docs/CLI.md).

## Layout

```
cmd/molty/          CLI entry + flag parsing
internal/molty/     client, HTTP + relay transports, MCP server, moods, audio, diag
internal/device/    typed HTTP client for the device API
internal/testdevice/ in-process fake device used by the tests
scripts/stt-whisper.sh  optional local STT backend (whisper.cpp)
```
