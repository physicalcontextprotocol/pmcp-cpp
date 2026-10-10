// pcp-cpp — Physical Context Protocol C++ SDK
//
// dialect.hpp — the compatibility layer.
//
// The PCP reference implementations do not agree on the wire. This repo
// currently contains four distinct vocabularies:
//
//   Dialect::kSpec        pcp-spec/docs/PROTOCOL_SPEC.md §7 (normative table)
//   Dialect::kPython      pcp-python/pcp        (actuations/call, metrics/get)
//   Dialect::kV05         pcp-python/v05, pcp-rust, pcp-typescript
//                         (tools/call, resources/read, pcp/estop)
//   Dialect::kConformance pcp-conformance       (actuations/execute, leases/acquire)
//
// The dialects disagree on three separate axes, so aliasing method names
// alone is not sufficient:
//
//   1. method name      pcp/estop vs pmcp/estop vs safety/estop/engage
//   2. parameter names  lease/request takes {robot_id,duration_ms} on the
//                       Python dialect but {robotId,durationMs} on v05
//   3. result shape     actuations/call returns {content:[{type:"actuation"}]}
//                       on Python, {content:[{type:"text"}]} on v05, and a
//                       flat {success,...} on the conformance dialect
//
// This header defines one canonical internal representation and the two
// adapters that translate it. Handlers are written once against the
// canonical form; the dialect layer owns every wire difference.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pmcp/error.hpp"
#include "pmcp/version.hpp"

namespace pmcp {

// Which wire vocabulary to speak.
enum class Dialect {
  kAuto,         // accept inbound aliases from every dialect (server default)
  kSpec,         // PROTOCOL_SPEC.md §7
  kPython,       // pcp-python/pcp
  kV05,          // v05 + rust + typescript
  kConformance,  // pcp-conformance
};

std::string_view dialect_name(Dialect d) noexcept;
std::optional<Dialect> dialect_from_string(std::string_view s);

// Canonical operations. Every handler is registered against one of these,
// never against a wire method name.
enum class Op {
  kInitialize,
  kPing,
  kListActuations,
  kCallActuation,
  kBatchActuation,
  kListSensors,
  kReadSensor,
  kListPrompts,
  kGetPrompt,
  kShadowPreview,
  kLeaseRequest,
  kLeaseRelease,
  kEstop,
  kEstopReset,
  kMetrics,
  kAuditList,
  kStatus,
  kIdentity,
  kConstitution,
  kSetLogLevel,
};

std::string_view op_name(Op op) noexcept;

// ---------------------------------------------------------------------------
// Method-name resolution
// ---------------------------------------------------------------------------

// The canonical name for an op on a dialect. Returns "" when the dialect has
// no method for the op (e.g. kV05 has no metrics/get).
std::string_view method_name(Op op, Dialect d) noexcept;

// Resolve an inbound wire method name to a canonical op. Accepts every alias
// from every dialect regardless of which dialect we are otherwise speaking,
// which is what Dialect::kAuto does for a server.
std::optional<Op> resolve_op(std::string_view method);

// Which dialect a given wire name belongs to. A server in Dialect::kAuto uses
// this to answer in the caller's dialect: a request for tools/call gets a
// v05-shaped result, a request for actuations/execute gets a conformance-shaped
// one, from the same handler.
std::optional<Dialect> dialect_of_method(std::string_view method);

// Every wire name that maps to an op, across all dialects. Used by servers to
// advertise aliases and by tests to prove the table is complete.
std::vector<std::string> aliases_for(Op op);

// ---------------------------------------------------------------------------
// Parameter normalization: wire -> canonical
// ---------------------------------------------------------------------------

// Translate inbound `params` for `op` into the canonical snake_case internal
// form. `inbound_method` is passed separately from the dialect because a few
// aliases encode information in the method name itself — notably
// safety/estop/engage vs safety/estop/disengage, which carry no params but
// differ in intent, and v05's pcp/estop, which encodes intent in a bool.
//
// Known keys after normalization, per op:
//   kCallActuation    name, arguments, robot_id, lease_token, zone_id, fence_token
//   kBatchActuation   actuations[], atomic, zone_id, robot_id
//   kLeaseRequest     robot_id, zone_id, duration_ms, bid_energy_j, priority
//   kLeaseRelease     lease_id
//   kEstop            active (bool), robot_id, source
//   kReadSensor       name
//   kShadowPreview    name, arguments, robot_id
//   kListPrompts      -
//   kGetPrompt        name, arguments
//   kAuditList        limit, robot_id, event_type, since
//   kSetLogLevel      level
//
// Unknown keys are preserved under "_extra" rather than dropped, so a dialect
// we have not modelled does not silently lose data.
json canonical_params(Op op, std::string_view inbound_method, const json& params);

// ---------------------------------------------------------------------------
// Result denormalization: canonical -> wire
// ---------------------------------------------------------------------------

// Serialize a canonical result for `dialect`. Emits a superset where the
// dialects are merely additive (initialize), and the exact shape where they
// are structurally incompatible (actuation results, lease grants, metrics).
json wire_result(Op op, Dialect d, const json& canonical);

// Python's json.dumps defaults to ensure_ascii=True, so the reference servers
// put "\u2264" on the wire where nlohmann would emit a raw UTF-8 byte. Use this
// when byte-comparing against the Python SDKs.
std::string dump_ascii(const json& j);

// nlohmann dump(2) with ensure_ascii semantics, matching Python's
// json.dumps(obj, indent=2) layout, which both Python SDKs embed as a string
// inside tools/call content blocks.
std::string dump_indent_ascii(const json& j, int indent = 2);

}  // namespace pmcp