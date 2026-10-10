# Changelog — pcp-cpp

Changes to the C++ SDK. Organization-wide policy and the maintained list of
what is *not* yet proven live in [`pcp-spec`](https://github.com/physicalcontextprotocol/pcp-spec)
— see its `LIMITATIONS.md`.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [1.0.1] — 2026-10-09

### Fixed
- **`cmake --install` failed outright.** The install rules referenced
  `ros2/pmcp_ros2_plugin.xml` and `ros2/launch/pmcp_bridge.launch.py`, but the
  `ros2/` packaging directory is not in this repository. The plugin.xml rule was
  not `OPTIONAL`, so `cmake --install` aborted before installing a single file.
  Both rules are now `OPTIONAL`, matching the fact that ROS 2 packaging lives
  elsewhere.
- **`find_package(pmcp)` did not work.** `EXPORT pmcpTargets` was declared on
  the library but the export set was never installed and no package config was
  generated, so the README's "with CMake package targets" claim was not true.
  The export is now installed as `pmcp::pmcp` alongside a generated
  `pmcpConfig.cmake` / `pmcpConfigVersion.cmake` under `lib/cmake/pmcp`, with a
  `find_dependency(Threads)`. Downstream projects can now
  `find_package(pmcp REQUIRED)` and link `pmcp::pmcp`. This is also what a
  vcpkg/Conan port's config-fixup step requires.

## [1.0.0] — 2026-10-07

First fully verified release.

### Added
- `pmcp::Server` and `pmcp::Client`, C++20, no dependencies beyond CMake and
  the standard library; JSON is a vendored `pmcp/json.hpp`.
- Dialect layer (`pmcp/dialect.hpp`) with four wire vocabularies — spec,
  pcp-python/pcp, v05 (rust/typescript), and pcp-conformance — plus a
  canonical internal representation so handlers are written once.
  - Inbound alias resolution accepts every method spelling from every
    dialect regardless of the configured dialect (`Dialect::kAuto`).
  - Response shaping answers each caller in the dialect it used, inferred
    from method name and from camelCase parameter spellings.
  - Superset results for ambiguous method names (e.g. `lease/release` always
    echoes `lease_id`; `sensors/read` returns both flat and `contents[]`).
- Safety pipeline (`pmcp/safety.hpp`) with a fixed order: E-stop latch →
  lease enforcement → constitution → shadow preview → execution.
  - E-stop: `pcp/estop` / `pmcp/estop` / `safety/estop/engage`, latched
    across all dialects, `estop_reset` to clear, audited.
  - Lease enforcement independently re-checks the token *after* the E-stop
    latch, so a `requires_lease` actuation without a token is rejected even
    when no guard rule covers it.
  - Constitution rules evaluated per actuation with profile membership.
  - Shadow preview required for actuations declaring `shadow_required`.
- Transports: HTTP/JSON serving `/pcp`, `/mcp` and `/` on one socket (the
  four SDKs disagree on the endpoint), stdio framing, and in-process binding.
- Batch actuation: auto-acquires a shared lease per batch, honours both
  `params` and `arguments` keys per item, forwards lease/fence tokens.
- Optional ROS 2 bridge (`pmcp/ros2_bridge.hpp`) behind
  `-DPMCP_WITH_ROS2=ON`; compiles to nothing when off.
- Test suite: 72 unit tests plus a cross-implementation interop harness that
  drives the C++ server with the real v05 `PCPClient` and the real
  `pcp-python/pcp` server with the C++ client. Interop skips (ctest SKIP)
  when the sibling SDKs are absent.

### Fixed
- Alias table padding: the v05 alias array used a fixed-size array with three
  value-initialized padding slots whose empty method string matched
  `resolve_op("")`; replaced with an exact-size table plus empty-method guards.
- E-stop crash: `audit_locked` accepted a `null` `extra` and called
  `.value(...)` on it, throwing `nlohmann::type_error` through a latched
  E-stop; `extra` is now normalized to an object before use.
- Lease enforcement gap: dispatch ran before lease validation for guarded
  actuations; enforcement now lives inside the safety check so it is ordered
  after the E-stop latch.
- `run_constitution` now gates rules on profile membership instead of
  evaluating rules from profiles that were never loaded.

### Verified
- 72/72 unit tests pass; build is warning-free (Apple Clang 17, C++20).
- Interop: 17/17 checks pass against the real `pcp-python/pcp` server and
  client, covering all four dialects, end-to-end lease + actuation, E-stop
  engage/block/reset, and error handling.
- macOS host only so far; CI runs the build and unit suite on Ubuntu. The
  ROS 2 bridge is compiled in but never exercised — see LIMITATIONS.md.