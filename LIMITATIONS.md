# pcp-cpp: Verified State & Known Limitations

This document exists so that every claim this repository makes about itself
is checkable. If something is not listed under "Verified today", treat it as
not yet proven — that is deliberate, not an oversight.

Everything below was **re-run while assembling the v1.0.0 release**, not
carried forward from an earlier draft. Where a number did not reproduce, it
has been corrected here rather than quietly overwritten.

## Verified today

Each line below is backed by an actual run on the machine this release was
assembled on (macOS, Apple Clang 17, CMake 4.3.2), or by a CI job.

| Check | Command | Result |
|---|---|---|
| Unit tests | `./build/pmcp_tests` | **72 passed, 0 failed** |
| Cross-SDK interop | `ctest --test-dir build -R pmcp_interop` | **17 checks passed, 0 failed, 0 skipped** |
| Build hygiene | `cmake -S . -B build -DPMCP_WITH_ROS2=OFF && cmake --build build` | clean (no warnings) |

Interop is two-directional and uses the *real* reference SDKs, not mocks:

- raw JSON-RPC against the C++ server in all four dialects (initialize,
  tools/list, lease grant + v05 camelCase echo, actuations/call,
  actuations/execute flat result, e-stop engage/block/disengage,
  pcp/metrics conformance shape, resources/read, HTTP 400 on malformed JSON);
- the real v05 `PCPClient` (`pcp-python/v05`) driving the C++ server end to
  end (initialize, tools/list, tools/call, lease, resources/read, e-stop
  engage → blocked actuation → clear);
- the C++ `Client` driving the real `pcp-python/pcp` server end to end
  (initialize, catalog, shadow/preview, lease, actuation, sensor read,
  e-stop engage → blocked actuation → reset, unknown-actuation error,
  release, ping).

## Not verified / not present — do not read success into these

- **Cross-platform.** Everything was built and tested on macOS. CI (Ubuntu,
  gcc 13) covers the build and the unit suite; the interop leg cannot run in
  CI without the sibling SDKs, so it skips there. MSVC/Windows, and gcc on
  non-Ubuntu distros, are untested.
- **ROS 2 bridge.** Compiled only. It is behind `-DPMCP_WITH_ROS2=ON`, was
  never exercised against a running ROS 2 graph (`rclcpp` is unavailable on
  this host), and the ament resource files it references are not yet in the
  tree. Treat `Ros2Bridge` as unchecked.
- **TLS.** `Client::http_post` and the HTTP server are plaintext HTTP only.
  An `https://` base URL is refused with an explicit error, which is the safe
  failure mode — but nothing here supplies transport security.
- **stdio client.** `Client::connect_stdio()` is a placeholder for an
  external subprocess transport and throws if used; only the in-process and
  HTTP transports are real on the client side.
- **C binary framing.** The canonical C bindings / binary wire framing from
  the spec are not implemented in this repository. The spec dialect here is
  JSON-RPC only.
- **Concurrency stress.** Unit tests cover E-stop/lease concurrency on one
  host. No multi-process, multi-host, or adversarial network testing has been
  done; the HTTP server is a prototype-grade single-threaded loop.
- **Shadow preview semantics.** `shadow/preview` is checked as a gate and
  its result is parsed, but shadowed execution (driving a simulator in
  parallel) is not connected to any real device, because no physical device or
  simulator runtime is present on this host. The physics-backed behaviour is
  per the spec's open research problems: it requires hardware to close.
- **Persistence.** The audit log is bounded in memory (`max_audit_entries`)
  and is lost on restart. There is no durable store and no registry seeding.
- **Sibling-repo drift.** Interop encodes today's `pcp-python/pcp` and
  `pcp-python/v05` behaviour. If those SDKs change their wire shapes, this
  repository's tests must be re-run against them; this README's numbers are
  only true for the exact revisions tested at release time.

## How to verify again

```sh
cmake -S . -B build -DPMCP_WITH_ROS2=OFF
cmake --build build -j
./build/pmcp_tests                     # unit
python3 ../pcp-python -m pytest -q    # sibling SDK, if present
python3 tests/interop/test_interop.py  # needs ../pcp-python importable
  --cpp-server build/pmcp_interop_server \
  --cpp-client build/pmcp_interop_client \
  --pmcp-root ..
```

The interop script exits 77 (ctest SKIP) when the sibling SDKs or their
dependencies are absent. That is *skipped*, not *passed* — a skip proves
nothing about cross-implementation behaviour.