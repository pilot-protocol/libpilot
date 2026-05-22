# libpilot

[![ci](https://github.com/pilot-protocol/libpilot/actions/workflows/ci.yml/badge.svg)](https://github.com/pilot-protocol/libpilot/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/pilot-protocol/libpilot/branch/main/graph/badge.svg)](https://codecov.io/gh/pilot-protocol/libpilot)
[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)

Pilot Protocol C ABI. Builds a shared library (`libpilot.dylib` / `libpilot.so` / `libpilot.dll`) plus a C header (`libpilot.h`) that other language SDKs link against via FFI.

The library embeds a full pilot daemon plus all the standard plugins (handshake, policy, runtime, skillinject, trustedagents, dataexchange, eventstream, webhook). FFI clients get a single in-process pilot node.

## Consumers

- [sdk-node](https://github.com/pilot-protocol/sdk-node) — Node.js via `koffi`
- [sdk-python](https://github.com/pilot-protocol/sdk-python) — Python via `ctypes`
- [sdk-swift](https://github.com/pilot-protocol/sdk-swift) — Swift via C interop

## Build

```bash
make build           # produces libpilot.<ext> + libpilot.h
make clean
```

The output `libpilot.h` is the C header consumers include or generate bindings against. The `.dylib` / `.so` / `.dll` is the runtime library.

## Layout

| File | What it does |
|---|---|
| `bindings.go` | Exported `//export` C functions: `pilot_init`, `pilot_send`, `pilot_recv`, `pilot_close`, etc. |
| `embedded.go` | In-process daemon construction (runtime + plugins). |

## Releases

The release workflow builds binaries for `linux/{amd64,arm64}`, `darwin/{amd64,arm64}`, and `windows/amd64` and attaches them to the GitHub release. SDK repos pull the matching artifacts in their own release pipelines.

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).
