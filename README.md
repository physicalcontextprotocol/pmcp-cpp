# pmcp-cpp — Physical Context Protocol C++ SDK

A C++20 SDK for building Physical Model Context Protocol (P-MCP) servers
and clients. It speaks, in one binary, every wire dialect currently
implemented by the reference SDKs, so a server written against this SDK
answers `tools/call`, `actuations/call`, and `actuations/execute` — and a
client written against it talks to servers of each kind.

## Verified status

Read [LIMITATIONS.md](LIMITATIONS.md) before relying on anything this
repository claims. The checked state, re-run at release time:

| Check | Command | Result |
|---|---|---|
| Unit tests | `./build/pmcp_tests` | **72 passed, 0 failed** |
| Interop (C++ vs real `pmcp-python/pcp` server and client) | `ctest --test-dir build -R pmcp_interop` | **17 checks passed, 0 failed** |
| Build | `cmake -S . -B build -DPMCP_WITH_ROS2=OFF && cmake --build build` | clean, no warnings (Apple Clang 17, gcc in CI) |

Interop covers both directions with the *real* SDKs: raw JSON-RPC against
the C++ server in all four dialects, the real v05 `PCPClient` class driving
the C++ server, and the C++ `Client` driving the real `pmcp-python/pcp`
server end to end (lease, shadow preview, actuation, sensor read,
e-stop engage/block/reset, unknown-actuation error, release, ping).

## Why the dialect layer exists

The reference implementations do not agree on the wire, and they disagree
on three separate axes — aliasing names alone is not enough:

1. **Method names** — `pcp/estop`, `pmcp/estop`, `safety/estop/engage`
2. **Parameter names** — `lease/request` takes `{robot_id, duration_ms}`
   on the Python dialect but `{robotId, durationMs}` on v05
3. **Result shapes** — `actuations/call` returns `{content:[{type:"actuation"}]}`
   on Python, `{content:[{type:"text"}]}` on v05, and a flat `{success,...}`
   on the conformance dialect

This SDK defines one canonical internal representation and adapts every
wire difference. Handlers are written once, against canonical ops; the
dialect layer owns the rest. A server in `Dialect::kAuto` (the default)
answers each caller in the caller's dialect, inferred from the method and
parameter spellings it used.

The four dialects:

| Dialect | Sources | Example methods |
|---|---|---|
| `kSpec` | `pmcp-spec/docs/PROTOCOL_SPEC.md` §7 | `actuations/call`, `lease/request`, `pmcp/estop` |
| `kPython` | `pmcp-python/pcp` | `actuations/call`, `metrics/get`, `pcp/estop` |
| `kV05` | `pmcp-python/v05`, `pmcp-rust`, `pmcp-typescript` | `tools/call`, `resources/read`, `pcp/estop`, camelCase params |
| `kConformance` | `pmcp-conformance` | `actuations/execute`, `leases/acquire`, `safety/estop/engage` |

## Features

- **Four dialects in one binary** — inbound aliases from every dialect are
  accepted on the server regardless of configured dialect; responses are
  emitted in the caller's dialect.
- **Safety pipeline** with a fixed order: E-stop latch → lease enforcement
  → constitution → shadow preview → execution. E-stop overrides everything,
  including lease checks.
- **Leases and fence tokens** — zone leases with expiry, required for
  guarded actuations (`requires_lease`), batched actuations auto-acquire a
  shared lease.
- **Constitution checks** — safety rules evaluated per actuation, profile
  membership enforced.
- **Shadow preview** — `shadow/preview` before motion, mandatory when an
  actuation declares `shadow_required`.
- **E-stop with audit** — pcp/estop engage/disengage and estop_reset,
  latched across every dialect, audited with normalized metadata.
- **Transports** — HTTP/JSON (multiple endpoints: `/pcp`, `/mcp`, `/`, as the
  four SDKs differ), stdio framing, and in-process binding for tests. No TLS
  yet; see LIMITATIONS.md.
- **ROS 2 bridge** — optional `Ros2Bridge` node, compiled to nothing unless
  `-DPMCP_WITH_ROS2=ON` is passed, so code can `#include` it unconditionally.
  Untested in this release; see LIMITATIONS.md.
- **Header-only JSON** — vendored `pmcp/json.hpp` (nlohmann), no external
  dependencies beyond CMake and a C++20 compiler.

## Build

Requirements: CMake >= 3.20, a C++20 compiler (Apple Clang 16+, gcc 11+).

```sh
cmake -S . -B build -DPMCP_WITH_ROS2=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Options:

- `-DPMCP_BUILD_TESTS=OFF` — skip the test executables
- `-DPMCP_BUILD_EXAMPLES=OFF` — skip the example programs
- `-DPMCP_WITH_ROS2=ON` — build the ROS 2 bridge (requires a ROS 2 install)

Install: `cmake --install build` exports the `pmcp` static library, the
`include/pmcp` headers, and a CMake package. Downstream projects can then:

```cmake
find_package(pmcp REQUIRED)
target_link_libraries(your_target PRIVATE pmcp::pmcp)
```

(or vendor the source with `FetchContent` / `add_subdirectory`).

## Quick start

### A server that answers every dialect

```cpp
#include "pmcp/server.hpp"

pmcp::ServerConfig cfg;
cfg.name = "simulated-arm";
cfg.version = "1.0.0";
cfg.robot_id = "sim-arm";
pmcp::Server server(cfg);

pmcp::ActuationSpec move;
move.name = "move_to";
move.description = "Move the tool centre point to an XYZ target";
move.est_duration_s = 2.0;
move.max_speed_m_s = 1.5;
move.parameters = {
    {"x", "number", "x target in metres"},
    {"y", "number", "y target in metres"},
    {"z", "number", "z target in metres"},
};
server.register_actuation(move, [](const pmcp::json& args) {
    return pmcp::json{{"ok", true}};
});

server.listen_http(8080);  // serves /pcp, /mcp and / on the same socket
```

Run `./build/pmcp_example_server --http --port 8080` for a complete example
with a small simulated arm.

### A client

```cpp
#include "pmcp/client.hpp"

pmcp::Client::Config cfg;
cfg.dialect = pmcp::Dialect::kPython;   // speak pmcp-python/pcp's shape
pmcp::Client client(cfg);
client.connect_http("http://127.0.0.1:8080");

auto lease = client.request_lease("lab-zone", "robot-1", 15'000);
auto out   = client.call_actuation("move_to", {{"x", 0.1}, {"y", 0.1}, {"z", 0.2}},
                                   /*lease_token=*/lease["lease"]["lease_id"], {});
```

Note: `Client::rpc` (and every method built on it) throws `pmcp::Error` when
the server answers with a JSON-RPC error. A blocked actuation — e.g. while
the E-stop is engaged — arrives as `Error(pmcp::Code::kEstopActive, ...)`,
not as an error-shaped result.

## Tests

- `pmcp_tests` — 72 unit tests: dialect aliasing/normalization, response
  shaping per dialect, lease lifecycle, E-stop latching across all four
  dialects, constitution profile enforcement, concurrency under E-stop.
- `pmcp_interop` — drives the C++ server and client against the real SDKs in
  the sibling `pmcp-python` repo. When the siblings (or their dependencies)
  are absent it **skips** (exit 77, ctest SKIP) rather than failing or
  silently passing. Cross-SDK coverage is exercised locally before release;
  see LIMITATIONS.md for what CI covers.

No test framework is required at build time: the harness in
`tests/test_main.hpp` is self-contained.

## Project layout

```
include/pmcp/server.hpp   Server, ServerConfig, ActuationSpec, SensorSpec
include/pmcp/client.hpp   Client, Config
include/pmcp/safety.hpp   Safety pipeline, constitution rules
include/pmcp/dialect.hpp  the wire-compatibility layer
include/pmcp/http.hpp     minimal HTTP server plumbing
include/pmcp/json.hpp     vendored nlohmann/json
include/pmcp/ros2_bridge.hpp  optional ROS 2 node (empty unless PMCP_WITH_ROS2)
src/                      implementation
tests/                    unit tests + interop drivers
examples/                 simulated arm server + minimal client
```

## License

Part of the Physical Model Context Protocol. See the org policy in
`pmcp-spec` for the canonical license and `SECURITY.md`.