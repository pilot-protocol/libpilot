# libpilot

Pilot Protocol C ABI. Builds a shared library
(`libpilot.dylib` / `libpilot.so` / `libpilot.dll`) that other
language SDKs link against via FFI:

- **pilot-protocol/sdk-node** — Node.js via `koffi`
- **pilot-protocol/sdk-python** — Python via `ctypes`
- (future) Swift, Rust, etc.

The library embeds a full pilot daemon plus all the standard plugins
(handshake, policy, runtime, skillinject, trustedagents, dataexchange,
eventstream, webhook). FFI clients get a single in-process pilot node.

## Layout

| File | What it does |
|---|---|
| `bindings.go` | The exported `//export` C functions: `pilot_init`, `pilot_send`, `pilot_recv`, `pilot_close`, etc. |
| `embedded.go` | In-process daemon construction (runtime + plugins). |

## Build

```bash
make build           # produces libpilot.<ext> + libpilot.h
make clean
```

The output `libpilot.h` is the C header consumers include or generate
bindings against. The `.dylib`/`.so`/`.dll` is the runtime library.

## Releasing

Release workflow builds binaries for `linux/{amd64,arm64}`,
`darwin/{amd64,arm64}`, `windows/amd64` and attaches them to the
GitHub release. SDK repos pull the matching artifacts in their own
release pipelines.

## Why a separate repo

CGo binaries don't compose well with pure-Go modules — putting the
`cgo` blob in the protocol repo's main module would force every
consumer onto a CGo-capable toolchain. Splitting it out keeps the
protocol modules pure-Go.
