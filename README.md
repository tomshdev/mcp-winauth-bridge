# mcp-winauth-bridge

Bridges a **stdio** MCP client to a **Streamable HTTP** MCP server using Windows
integrated authentication — Negotiate / Kerberos / NTLM as the currently
logged-in user.

MCP clients such as Claude Code and Claude Desktop launch stdio servers. A
corporate MCP server behind IIS or a reverse proxy speaks HTTP and demands
integrated auth. This sits between them: one JSON-RPC message per line on
stdin, POSTed to the HTTP endpoint with the current user's credentials, and the
replies written back one message per line on stdout.

Windows and WinHTTP only. No third-party dependencies, no package manager, no
credentials to configure or store.

```
  MCP client  ──stdio──▶  mcp-winauth-bridge  ──HTTPS + Negotiate──▶  MCP server
              ◀─stdio───                      ◀──JSON or SSE────────
```

## Get it

**Download** the signed-by-nothing but hash-published binary from the
[Releases](https://github.com/tomshdev/mcp-winauth-bridge/releases) page. It is
a single self-contained `mcp-winauth-bridge.exe` with the CRT linked
statically, so it runs on a machine with no Visual C++ redistributable. Verify
it against the `SHA256SUMS.txt` published beside it:

```powershell
certutil -hashfile mcp-winauth-bridge.exe SHA256
```

The binary is **not code-signed**, so SmartScreen will warn the first time you
run it. Getting a certificate is out of scope for now.

Every push also uploads a build artifact, so a binary is available without
waiting for a tag.

## Use it

```
mcp-winauth-bridge.exe [options] <mcp-url>
```

Register it with your MCP client as an ordinary stdio server. For Claude Code:

```json
{
  "mcpServers": {
    "corp-grafana": {
      "command": "C:\\tools\\mcp-winauth-bridge.exe",
      "args": ["https://grafana-mcp.corp.local/mcp"]
    }
  }
}
```

Or drive it by hand:

```powershell
Get-Content requests.jsonl | .\mcp-winauth-bridge.exe --log-level debug https://mcp.corp.local/mcp
```

### Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--user-agent <s>` | `mcp-winauth-bridge/1.0` | `User-Agent` header |
| `--workers <n>` | `4` | Concurrent in-flight requests |
| `--queue-depth <n>` | `256` | Pending messages before stdin blocks; `0` is unbounded |
| `--auth-retries <n>` | `3` | Attempts in the 401 auth loop |
| `--resolve-timeout <ms>` | `10000` | DNS |
| `--connect-timeout <ms>` | `10000` | TCP connect |
| `--send-timeout <ms>` | `30000` | Send |
| `--receive-timeout <ms>` | `600000` | Receive; long on purpose, tool calls can be slow |
| `--drain-timeout <ms>` | `0` | Wait for in-flight requests at exit; `0` waits forever |
| `--log-level <l>` | `info` | `error`, `warn`, `info`, `debug` |
| `--no-autologon` | off | Only send credentials to Intranet-zone hosts |
| `--no-delete-session` | off | Skip the `DELETE` that ends the server session |
| `-h`, `--help` | | |
| `--version` | | |

Logs always go to **stderr**; stdout carries nothing but JSON-RPC messages, one
per line.

### What it does and does not do

- Each stdin line is one JSON-RPC message, POSTed to the server.
- Both `application/json` and `text/event-stream` responses are handled. SSE
  events are emitted as they arrive, one message per line.
- `Mcp-Session-Id` is captured from the first response and sent on every later
  request. A `DELETE` ends the session when stdin closes.
- A request that cannot be delivered gets a synthesized `-32603` error back, so
  the client never waits forever. Notifications get nothing, correctly.
- **Not supported:** the optional server-to-client `GET` stream, and SSE resume
  via `Last-Event-ID`.

### Troubleshooting authentication

- **`server offers neither Negotiate nor NTLM`** — the endpoint is not
  configured for Windows auth, or a proxy is stripping `WWW-Authenticate`.
- **Kerberos falls back to NTLM, or fails outright** — usually a missing or
  duplicate SPN on the service account. Check with
  `setspn -L <service-account>` and `klist`.
- **401 even though you are domain-joined** — by default the bridge passes
  `WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW`, which lets WinHTTP send your
  credentials to any host rather than only Intranet-zone ones. If your policy
  forbids that, run with `--no-autologon` and add the host to the Local
  Intranet zone instead.
- Proxies are picked up from the system configuration automatically
  (`WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`).

## Build it from source

### With Visual Studio 2022 (any edition, including the free Build Tools)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`scripts\dev-build.cmd [Debug|Release]` does that plus every check CI runs —
the unit tests, the exe smoke, and both library consumption modes.

### With Docker, if you have no toolchain and will not install one

`docker\windows\Dockerfile` builds the exe hermetically. It needs Docker in
**Windows containers** mode, takes 30–60 minutes cold, and the build stage is
around 20 GB. It exists as the offline reproducer; downloading a release is the
normal path.

```powershell
docker build --isolation=hyperv -t mcpwa-build -f docker\windows\Dockerfile .
$c = docker create mcpwa-build
docker cp "${c}:C:\mcp-winauth-bridge.exe" .\dist\
docker rm $c
```

Process isolation requires the host kernel to match the base image, hence
`--isolation=hyperv` on a Windows 11 26xxx host.

## Use it as a library

The bridge is a static library with a thin exe on top. Public headers include
no `windows.h`, so dropping them into another project will not drag in
`NOMINMAX` problems or macro collisions.

### As an installed CMake package

```powershell
cmake --install build --config Release --prefix C:\opt\mcpwinauth
```

```cmake
find_package(mcpwinauth 1.0 CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE mcpwinauth::bridge)
```

`winhttp` is carried on the exported target, so consumers link nothing extra.

### As a vendored subdirectory

```cmake
set(MCPWA_BUILD_EXE   OFF CACHE BOOL "")
set(MCPWA_BUILD_TESTS OFF CACHE BOOL "")
set(MCPWA_INSTALL     OFF CACHE BOOL "")
add_subdirectory(third_party/mcp-winauth-bridge)
target_link_libraries(my_host PRIVATE mcpwinauth::bridge)
```

### As sources dropped straight in

Copy `include/mcpwinauth/*` and `src/*` into your tree, add the `.cpp` files to
your target, link `winhttp.lib`, and define `UNICODE`, `_UNICODE`, `NOMINMAX`
and `WIN32_LEAN_AND_MEAN`. That is the whole contract, and CI checks it on
every push with a single `cl` invocation.

### The API

```cpp
#include "mcpwinauth/bridge.hpp"

mcpwinauth::Config cfg;
cfg.url         = L"https://mcp.corp.local/mcp";
cfg.workerCount = 4;

// Both sinks are called from worker threads and must be thread-safe.
auto out = [](const std::string& message) { /* one JSON-RPC message */ };
auto log = [](mcpwinauth::LogLevel level, const std::string& text) { /* ... */ };

mcpwinauth::Bridge bridge(cfg, out, log);
if (mcpwinauth::Error e = bridge.Start(); !e) {
    // e.status, e.message, e.win32
}
bridge.Submit(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
bridge.Shutdown();   // drain, join, DELETE the session
```

`Submit` blocks while the queue is full rather than dropping messages, which is
the right backpressure over a pipe. `mcpwinauth::RunStdioBridge` wraps the
whole read-until-EOF loop, and `mcpwinauth::BridgeMain` is the complete CLI if
you want to expose it from your own exe.

Nothing throws across the API boundary. A host that already owns an HTTP client
can implement `mcpwinauth::ITransport` and pass it to the four-argument `Bridge`
constructor.

## How it works

`src/winhttp_transport.cpp` is the only file that touches the network, and it
does no parsing — it hands raw byte chunks up. Everything else is ordinary
testable code: `jsonrpc.cpp` scans for the top-level JSON-RPC `id` and `method`
without a JSON parser, `framing.cpp` turns arbitrary byte chunks into complete
messages, and `bridge.cpp` runs the worker pool.

Three details worth knowing:

- **A bounded worker pool**, not a thread per stdin line, so a burst cannot
  spawn unbounded threads.
- **The first request is serialized.** Until the server has issued an
  `Mcp-Session-Id`, exactly one request may be in flight; otherwise a burst of
  concurrent first requests makes the server open a session per request and
  only the last one survives. After that the pool runs fully concurrent.
- **The shutdown `DELETE` cannot race in-flight requests.** Shutdown drains the
  queue and joins every worker before ending the session. With
  `--drain-timeout` set, a stuck request is abandoned by closing the WinHTTP
  session handle — the only safe way to release a thread parked in the network
  stack — and the `DELETE` is then skipped.

## Licence

MIT. See [LICENSE](LICENSE).
