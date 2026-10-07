#include "pmcp/dialect.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>

namespace pmcp {
namespace {

struct Alias {
  std::string_view method;
  Op op;
  Dialect dialect;
};

// Single source of truth for the whole fragmentation. Adding a dialect is a
// change to this one table plus the serializers below.
//
// `kSpec` and `kPython` share most names, but the spec says `pmcp/estop` where
// both SDKs say `pcp/estop`; the spec also names `constitution/check` where v05
// says `pcp/constitution`. Those divergences are recorded explicitly rather
// than papered over, because they are the bug.
// The size is exact on purpose: a larger std::array would value-initialize the
// remaining slots into empty-method aliases, and `resolve_op("")` would then
// resolve to whatever Op the padding happened to carry.
constexpr std::array<Alias, 61> kAliases{{
    // ---- initialize -------------------------------------------------------
    {"initialize", Op::kInitialize, Dialect::kSpec},
    {"initialize", Op::kInitialize, Dialect::kPython},
    {"initialize", Op::kInitialize, Dialect::kV05},
    {"initialize", Op::kInitialize, Dialect::kConformance},

    // ---- ping -------------------------------------------------------------
    {"ping", Op::kPing, Dialect::kSpec},
    {"ping", Op::kPing, Dialect::kPython},
    {"ping", Op::kPing, Dialect::kV05},
    {"pcp/ping", Op::kPing, Dialect::kConformance},

    // ---- actuation discovery ---------------------------------------------
    {"actuations/list", Op::kListActuations, Dialect::kSpec},
    {"actuations/list", Op::kListActuations, Dialect::kPython},
    {"actuations/list", Op::kListActuations, Dialect::kConformance},
    {"tools/list", Op::kListActuations, Dialect::kV05},

    // ---- actuation execution --------------------------------------------
    {"actuations/call", Op::kCallActuation, Dialect::kSpec},
    {"actuations/call", Op::kCallActuation, Dialect::kPython},
    {"tools/call", Op::kCallActuation, Dialect::kV05},
    {"actuations/execute", Op::kCallActuation, Dialect::kConformance},

    // ---- batch (no v05 equivalent; falls back to tools/call per item) ----
    {"actuations/batch", Op::kBatchActuation, Dialect::kPython},
    {"actuations/batch", Op::kBatchActuation, Dialect::kConformance},

    // ---- sensors ----------------------------------------------------------
    {"sensors/list", Op::kListSensors, Dialect::kSpec},
    {"sensors/list", Op::kListSensors, Dialect::kPython},
    {"sensors/list", Op::kListSensors, Dialect::kConformance},
    {"resources/list", Op::kListSensors, Dialect::kV05},

    {"sensors/read", Op::kReadSensor, Dialect::kSpec},
    {"sensors/read", Op::kReadSensor, Dialect::kPython},
    {"sensors/read", Op::kReadSensor, Dialect::kConformance},
    {"resources/read", Op::kReadSensor, Dialect::kV05},

    // ---- prompts (all four agree) ----------------------------------------
    {"prompts/list", Op::kListPrompts, Dialect::kSpec},
    {"prompts/list", Op::kListPrompts, Dialect::kPython},
    {"prompts/list", Op::kListPrompts, Dialect::kV05},
    {"prompts/list", Op::kListPrompts, Dialect::kConformance},
    {"prompts/get", Op::kGetPrompt, Dialect::kSpec},
    {"prompts/get", Op::kGetPrompt, Dialect::kPython},
    {"prompts/get", Op::kGetPrompt, Dialect::kV05},
    {"prompts/get", Op::kGetPrompt, Dialect::kConformance},

    // ---- shadow (all four agree on the name) -----------------------------
    {"shadow/preview", Op::kShadowPreview, Dialect::kSpec},
    {"shadow/preview", Op::kShadowPreview, Dialect::kPython},
    {"shadow/preview", Op::kShadowPreview, Dialect::kV05},
    {"shadow/preview", Op::kShadowPreview, Dialect::kConformance},

    // ---- leases -----------------------------------------------------------
    {"lease/request", Op::kLeaseRequest, Dialect::kSpec},
    {"lease/request", Op::kLeaseRequest, Dialect::kPython},
    {"lease/request", Op::kLeaseRequest, Dialect::kV05},
    {"leases/acquire", Op::kLeaseRequest, Dialect::kConformance},

    {"lease/release", Op::kLeaseRelease, Dialect::kSpec},
    {"lease/release", Op::kLeaseRelease, Dialect::kPython},
    {"lease/release", Op::kLeaseRelease, Dialect::kV05},
    {"leases/release", Op::kLeaseRelease, Dialect::kConformance},

    // ---- e-stop: three names, three result shapes ------------------------
    {"pmcp/estop", Op::kEstop, Dialect::kSpec},
    {"pcp/estop", Op::kEstop, Dialect::kPython},
    {"pcp/estop", Op::kEstop, Dialect::kV05},
    {"safety/estop/engage", Op::kEstop, Dialect::kConformance},

    {"pmcp/estop_reset", Op::kEstopReset, Dialect::kSpec},
    {"pcp/estop_reset", Op::kEstopReset, Dialect::kPython},
    {"safety/estop/disengage", Op::kEstopReset, Dialect::kConformance},

    // ---- observability ----------------------------------------------------
    {"metrics/get", Op::kMetrics, Dialect::kPython},
    {"pcp/metrics", Op::kMetrics, Dialect::kConformance},
    {"audit/list", Op::kAuditList, Dialect::kPython},
    {"pcp/status", Op::kStatus, Dialect::kV05},
    {"pcp/identity", Op::kIdentity, Dialect::kV05},
    {"constitution/check", Op::kConstitution, Dialect::kSpec},
    {"pcp/constitution", Op::kConstitution, Dialect::kV05},
    {"logging/setLevel", Op::kSetLogLevel, Dialect::kV05},
}};

// Methods that arrive with no id and get no reply. Both Python SDKs treat a
// missing id as a notification: stdio writes nothing, HTTP writes "{}".
constexpr std::array<std::string_view, 2> kNotifications{
    "notifications/initialized", "notifications/cancelled"};

bool is_notification(std::string_view m) {
  return std::find(kNotifications.begin(), kNotifications.end(), m) != kNotifications.end();
}

// Pull the first present key from a list of candidate spellings. Every key it
// examines is recorded in `seen` so canonical_params can tell an unmodelled key
// from one it deliberately ignored.
const json* pick(const json& o, std::initializer_list<const char*> keys,
                 std::set<std::string>* seen = nullptr) {
  if (!o.is_object()) return nullptr;
  for (auto* k : keys) {
    if (seen) seen->insert(k);
    auto it = o.find(k);
    if (it != o.end() && !it->is_null()) return &(*it);
  }
  return nullptr;
}

void set_if(json& dst, const char* key, const json* src) {
  if (src) dst[key] = *src;
}

// v05 resources/read accepts a full pcp:// URI or a bare sensor name. Both
// Python servers resolve on the last path segment, so do the same.
std::string sensor_name_from_uri(std::string_view uri) {
  auto pos = uri.find_last_of('/');
  return std::string(pos == std::string_view::npos ? uri : uri.substr(pos + 1));
}

double now_seconds() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

// ---------------------------------------------------------------------------

std::string_view dialect_name(Dialect d) noexcept {
  switch (d) {
    case Dialect::kAuto: return "auto";
    case Dialect::kSpec: return "spec";
    case Dialect::kPython: return "python";
    case Dialect::kV05: return "v05";
    case Dialect::kConformance: return "conformance";
  }
  return "unknown";
}

std::optional<Dialect> dialect_from_string(std::string_view s) {
  if (s == "auto") return Dialect::kAuto;
  if (s == "spec") return Dialect::kSpec;
  if (s == "python") return Dialect::kPython;
  if (s == "v05") return Dialect::kV05;
  if (s == "conformance") return Dialect::kConformance;
  return std::nullopt;
}

std::string_view op_name(Op op) noexcept {
  switch (op) {
    case Op::kInitialize: return "initialize";
    case Op::kPing: return "ping";
    case Op::kListActuations: return "list_actuations";
    case Op::kCallActuation: return "call_actuation";
    case Op::kBatchActuation: return "batch_actuation";
    case Op::kListSensors: return "list_sensors";
    case Op::kReadSensor: return "read_sensor";
    case Op::kListPrompts: return "list_prompts";
    case Op::kGetPrompt: return "get_prompt";
    case Op::kShadowPreview: return "shadow_preview";
    case Op::kLeaseRequest: return "lease_request";
    case Op::kLeaseRelease: return "lease_release";
    case Op::kEstop: return "estop";
    case Op::kEstopReset: return "estop_reset";
    case Op::kMetrics: return "metrics";
    case Op::kAuditList: return "audit_list";
    case Op::kStatus: return "status";
    case Op::kIdentity: return "identity";
    case Op::kConstitution: return "constitution";
    case Op::kSetLogLevel: return "set_log_level";
  }
  return "unknown";
}

std::string_view method_name(Op op, Dialect d) noexcept {
  for (const auto& a : kAliases) {
    if (a.op == op && a.dialect == d) return a.method;
  }
  return {};
}

std::optional<Op> resolve_op(std::string_view method) {
  if (method.empty()) return std::nullopt;
  for (const auto& a : kAliases) {
    if (a.method == method) return a.op;
  }
  return std::nullopt;
}

std::optional<Dialect> dialect_of_method(std::string_view method) {
  // First match wins. Names shared by several dialects (actuations/list,
  // initialize, shadow/preview, ...) resolve to the first dialect that
  // declares them, which is the one whose result shape they were written for.
  if (method.empty()) return std::nullopt;
  for (const auto& a : kAliases) {
    if (a.method == method) return a.dialect;
  }
  return std::nullopt;
}

std::vector<std::string> aliases_for(Op op) {
  std::vector<std::string> out;
  for (const auto& a : kAliases) {
    if (a.op == op) {
      auto s = std::string(a.method);
      if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Parameter normalization
// ---------------------------------------------------------------------------

json canonical_params(Op op, std::string_view inbound_method, const json& params) {
  json p = params.is_object() ? params : json::object();
  json c = json::object();
  json extra = json::object();

  // Records every candidate spelling pick() considered, so the sweep at the end
  // can distinguish a key we deliberately ignored from one nobody modelled.
  std::set<std::string> seen;
  auto pickp = [&p, &seen](std::initializer_list<const char*> keys) { return pick(p, keys, &seen); };

  switch (op) {
    case Op::kInitialize: {
      if (const json* v = pickp({"protocolVersion", "protocol_version"})) c["protocol_version"] = *v;
      if (const json* v = pickp({"clientInfo", "client_info"})) c["client_info"] = *v;
      break;
    }

    case Op::kReadSensor: {
      // Python and conformance pass `name`; v05 passes `uri`, which may be a
      // full pcp:// URL or a bare name.
      if (const json* v = pickp({"name", "sensor", "sensor_name"})) {
        c["name"] = sensor_name_from_uri(v->is_string() ? v->get<std::string>() : std::string{});
      } else if (const json* v = pickp({"uri"})) {
        c["name"] = sensor_name_from_uri(v->is_string() ? v->get<std::string>() : std::string{});
      }
      break;
    }

    case Op::kShadowPreview: {
      // Python prefers actuation_name, then falls back to name.
      if (const json* v = pickp({"actuation_name", "actuationName", "name"})) c["name"] = *v;
      set_if(c, "arguments", pickp({"arguments", "params", "args"}));
      set_if(c, "robot_id", pickp({"robot_id", "robotId"}));
      break;
    }

    case Op::kCallActuation: {
      set_if(c, "name", pickp({"name", "actuation", "actuation_name"}));
      // The conformance dialect calls the argument bag `params`, which
      // collides with the JSON-RPC envelope member, hence the awkward trio.
      set_if(c, "arguments", pickp({"arguments", "params", "args"}));
      set_if(c, "robot_id", pickp({"robot_id", "robotId"}));
      set_if(c, "lease_token", pickp({"lease_token", "_lease_token", "leaseToken", "lease_id"}));
      set_if(c, "zone_id", pickp({"zone_id", "_zone_id", "zoneId"}));
      if (const json* v = pickp({"fence_token", "_fence_token", "fenceToken"})) {
        c["fence_token"] = *v;
      }
      break;
    }

    case Op::kBatchActuation: {
      if (const json* v = pickp({"actuations"})) c["actuations"] = *v;
      set_if(c, "atomic", pickp({"atomic"}));
      set_if(c, "zone_id", pickp({"zone_id", "zoneId"}));
      set_if(c, "robot_id", pickp({"robot_id", "robotId"}));
      // Normalize each entry's argument bag: Python uses `arguments`, the
      // conformance dialect uses `params`.
      if (c.contains("actuations") && c["actuations"].is_array()) {
        for (auto& item : c["actuations"]) {
          if (!item.is_object()) continue;
          if (!item.contains("arguments")) {
            if (auto it = item.find("params"); it != item.end()) item["arguments"] = *it;
          }
          item.erase("params");
        }
      }
      break;
    }

    case Op::kLeaseRequest: {
      set_if(c, "robot_id", pickp({"robot_id", "robotId"}));
      set_if(c, "zone_id", pickp({"zone_id", "zoneId", "zone"}));
      set_if(c, "duration_ms", pickp({"duration_ms", "durationMs", "duration"}));
      set_if(c, "bid_energy_j", pickp({"bid_energy_j", "bidEnergyJ", "bid", "bid_energy"}));
      set_if(c, "priority", pickp({"priority"}));
      break;
    }

    case Op::kLeaseRelease: {
      set_if(c, "lease_id", pickp({"lease_id", "leaseId", "lease"}));
      break;
    }

    case Op::kEstop: {
      // Three dialects, three encodings of intent:
      //   v05          -> params {"active": bool}
      //   python/spec  -> method implies engage; {"source": ...} optional
      //   conformance  -> the method name itself says engage or disengage
      if (inbound_method == "safety/estop/engage") {
        c["active"] = true;
      } else if (inbound_method == "safety/estop/disengage") {
        c["active"] = false;
      } else if (const json* v = pickp({"active", "engaged"})) {
        c["active"] = *v;
      } else {
        c["active"] = true;  // pcp/estop and pmcp/estop always engage
      }
      set_if(c, "robot_id", pickp({"robot_id", "robotId"}));
      set_if(c, "source", pickp({"source"}));
      break;
    }

    case Op::kEstopReset: {
      set_if(c, "robot_id", pickp({"robot_id", "robotId"}));
      break;
    }

    case Op::kAuditList: {
      set_if(c, "limit", pickp({"limit"}));
      set_if(c, "robot_id", pickp({"robot_id", "robotId"}));
      set_if(c, "event_type", pickp({"event_type", "eventType", "type"}));
      set_if(c, "since", pickp({"since"}));
      break;
    }

    case Op::kSetLogLevel: {
      set_if(c, "level", pickp({"level"}));
      break;
    }

    case Op::kGetPrompt: {
      set_if(c, "name", pickp({"name", "mission", "prompt"}));
      set_if(c, "arguments", pickp({"arguments", "params", "args"}));
      break;
    }

    default:
      break;
  }
  // Preserve anything unmodelled rather than dropping it: a future SDK revision
  // adding a field must not be silently truncated by this SDK. v05's
  // _lease_token / _zone_id / _fence_token extensions are picked up here too,
  // since no op claims them by name.
  for (auto it = p.begin(); it != p.end(); ++it) {
    if (seen.count(it.key()) != 0) continue;
    if (it->is_null()) continue;
    extra[it.key()] = *it;
  }
  // v05's underscore-prefixed scalars are promoted to canonical names, since
  // the server needs them under their canonical spelling.
  for (const auto& [k, v] : extra.items()) {
    if (k.size() < 2 || k[0] != '_') continue;
    if (k.rfind("_lease_token", 0) == 0) c["lease_token"] = v;
    else if (k.rfind("_zone_id", 0) == 0) c["zone_id"] = v;
    else if (k.rfind("_fence_token", 0) == 0) c["fence_token"] = v;
  }

  if (!extra.empty()) c["_extra"] = extra;
  return c;
}

// ---------------------------------------------------------------------------
// Result serialization
// ---------------------------------------------------------------------------

json wire_result(Op op, Dialect d, const json& c) {
  // Canonical field readers. A handler may populate any of the shapes the
  // dialects use; we read whichever is present.
  auto has = [&](const char* k) { return c.is_object() && c.contains(k); };
  auto get_or = [&](const char* k, json fallback) -> json {
    return (has(k) && !c[k].is_null()) ? c[k] : std::move(fallback);
  };

  switch (op) {
    case Op::kInitialize: {
      // The four dialects disagree structurally on serverInfo and
      // capabilities, so emit a superset. Every reader uses .get(), so extra
      // members are inert for all of them.
      json info = get_or("server_info", json::object());
      json out;
      // protocolVersion is the one field the dialects genuinely conflict on:
      // v05 answers "2024-11-05" (the MCP spec revision) while the spec, both
      // Python SDKs and pmcp-conformance all assert "0.5".
      out["protocolVersion"] =
          d == Dialect::kV05 ? json(std::string(kMcpSpecVersion)) : json(std::string(kPcVersion));
      out["serverInfo"] = {
          {"name", info.value("name", "pmcp-cpp")},
          {"version", info.value("version", "1.0.0")},
          // conformance asserts serverInfo.robotId; v05 asserts serverInfo
          // name/version only.
          {"robotId", info.value("robot_id", info.value("name", "pmcp-cpp"))},
          {"protocolVersion", std::string(kPcVersion)},
      };
      out["capabilities"] = get_or("capabilities", json::object());
      if (has("instructions")) out["instructions"] = c["instructions"];
      if (has("pcp")) out["pcp"] = c["pcp"];
      return out;
    }

    case Op::kPing: {
      // conformance wants a `timestamp` in ms; v05 wants `ts` in seconds;
      // Python wants `uptime_s`. Emit all three.
      const double secs = get_or("ts", now_seconds()).is_number()
                              ? get_or("ts", now_seconds()).get<double>()
                              : now_seconds();
      json out{{"pong", true},
               {"ts", secs},
               {"timestamp", static_cast<int64_t>(secs * 1000.0)},
               {"server", get_or("robot_id", json("pmcp-cpp"))},
               {"robot_id", get_or("robot_id", json("pmcp-cpp"))},
               {"uptime_s", get_or("uptime_s", json(0.0))}};
      return out;
    }

    case Op::kListActuations: {
      const json list = get_or("actuations", json::array());
      if (d == Dialect::kV05) {
        // v05 wants MCP Tool objects; the canonical entries already carry
        // `inputSchema`, so pass them through and attach annotations if the
        // handler supplied a parallel `annotations` map.
        json tools = json::array();
        for (const auto& a : list) {
          json t = a;
          if (!t.contains("inputSchema")) {
            t["inputSchema"] = {{"type", "object"},
                                {"properties", json::object()},
                                {"required", json::array()}};
          }
          tools.push_back(std::move(t));
        }
        return {{"tools", std::move(tools)}};
      }
      if (d == Dialect::kConformance) {
        // conformance only requires name + description per entry.
        json acts = json::array();
        for (const auto& a : list) {
          acts.push_back({{"name", a.value("name", "")},
                          {"description", a.value("description", "")}});
        }
        return {{"actuations", std::move(acts)}};
      }
      return {{"actuations", list}};
    }

    case Op::kListSensors: {
      const json list = get_or("sensors", json::array());
      if (d == Dialect::kV05) {
        json rs = json::array();
        for (const auto& s : list) {
          json r = s;
          if (!r.contains("uri")) {
            r["uri"] = "pcp://" + s.value("robot_id", std::string("robot")) + "/sensors/" +
                       s.value("name", std::string{});
          }
          if (!r.contains("mimeType")) r["mimeType"] = "application/json";
          rs.push_back(std::move(r));
        }
        return {{"resources", std::move(rs)}};
      }
      if (d == Dialect::kConformance) {
        json ss = json::array();
        for (const auto& s : list) ss.push_back({{"name", s.value("name", "")}});
        return {{"sensors", std::move(ss)}};
      }
      return {{"sensors", list}};
    }

    case Op::kReadSensor: {
      // Two dialects share the method name `sensors/read` with incompatible
      // shapes: Python/spec nest the reading in contents[], conformance wants
      // a flat `value` plus timestamp_ms. They use disjoint keys, so emit both
      // rather than guessing which reader is on the other end.
      const json body{{"sensor_name", get_or("name", json(""))},
                      {"value", get_or("value", json())},
                      {"unit", get_or("unit", json(""))},
                      {"timestamp", get_or("timestamp", json(now_seconds()))},
                      {"quality", get_or("quality", json(1.0))}};
      json flat{{"value", get_or("value", json())},
                {"name", get_or("name", json(""))},
                {"unit", get_or("unit", json(""))},
                {"quality", get_or("quality", json(1.0))},
                {"timestamp_ms",
                 static_cast<int64_t>(get_or("timestamp", json(now_seconds())).get<double>() *
                                      1000.0)}};
      if (d == Dialect::kV05) {
        // v05 nests a pretty-printed JSON string in contents[].
        json vbody{{"sensor", get_or("name", json(""))},
                   {"robot_id", get_or("robot_id", json(""))},
                   {"value", get_or("value", json())},
                   {"unit", get_or("unit", json(""))},
                   {"timestamp", get_or("timestamp", json(now_seconds()))},
                   {"quality", get_or("quality", json(1.0))}};
        flat["contents"] = json::array({{{"type", "text"}, {"text", dump_indent_ascii(vbody)}}});
        return flat;
      }
      if (d == Dialect::kConformance) return flat;
      flat["contents"] =
          json::array({{{"uri", get_or("uri", json(""))},
                        {"mimeType", "application/pcp-sensor"},
                        {"data", body}}});
      return flat;
    }

    case Op::kCallActuation: {
      const bool success = c.value("success", true);
      if (d == Dialect::kV05) {
        json body{{"success", success},
                  {"robot_id", get_or("robot_id", json(""))},
                  {"actuation", get_or("actuation_name", json(""))},
                  {"output", get_or("output", json::object())},
                  {"metrics",
                   {{"duration_s", get_or("duration_s", json(0.0))},
                    {"energy_consumed_j", get_or("energy_j", json(0.0))},
                    {"shadow_delta_m", get_or("shadow_delta_m", json(0.0))}}}};
        if (!success) body["error"] = get_or("error", json(""));
        if (has("final_pose") && !c["final_pose"].is_null()) body["final_pose"] = c["final_pose"];
        json out{{"content", json::array({{{"type", "text"},
                                          {"text", dump_indent_ascii(body)}}})},
                 {"isError", !success}};
        if (has("shadow") && !c["shadow"].is_null()) out["_shadow"] = c["shadow"];
        return out;
      }
      if (d == Dialect::kConformance) {
        return {{"success", success},
                {"energy_consumed_j", get_or("energy_j", json(0.0))},
                {"duration_ms", get_or("duration_ms",
                                       json(get_or("duration_s", json(0.0)).get<double>() * 1000.0))},
                {"output", get_or("output", json::object())}};
      }
      json data{{"success", success},
                {"actuation_id", get_or("actuation_id", json(""))},
                {"robot_id", get_or("robot_id", json(""))},
                {"final_pose", get_or("final_pose", json())},
                {"duration_s", get_or("duration_s", json(0.0))},
                {"energy_j", get_or("energy_j", json(0.0))},
                {"error", get_or("error", json())},
                {"metadata", get_or("metadata", json::object())}};
      return {{"content", json::array({{{"type", "actuation"}, {"data", data}}})},
              {"isError", !success}};
    }

    case Op::kBatchActuation: {
      const json results = get_or("results", json::array());
      double total_ms = 0.0;
      for (const auto& r : results) {
        total_ms += r.value("duration_ms", 0.0);
      }
      json out{{"results", results},
               {"total_duration_ms", total_ms},
               {"success", get_or("success", json(true))}};
      // Python wraps in `batch`; conformance reads the top level.
      if (d == Dialect::kPython || d == Dialect::kSpec) {
        out["batch"] = {{"batch_id", get_or("batch_id", json(""))},
                        {"success", get_or("success", json(true))},
                        {"results", results},
                        {"failed_at", get_or("failed_at", json())},
                        {"error", get_or("error", json())}};
      }
      return out;
    }

    case Op::kLeaseRequest: {
      const json lease = get_or("lease", json::object());
      const std::string state = lease.value("state", std::string("ACTIVE"));
      const bool granted = state == "ACTIVE";
      const double expires_at = lease.value("expires_at", 0.0);
      const double expires_ms = lease.value("expires_ms", expires_at * 1000.0);
      const double remaining_ms =
          lease.value("remaining_ms", (expires_at > 0.0 ? (expires_at - now_seconds()) * 1000.0 : 0.0));

      if (d == Dialect::kV05) {
        return {{"lease",
                 {{"leaseId", lease.value("lease_id", std::string{})},
                  {"robotId", lease.value("robot_id", std::string{})},
                  {"zoneId", lease.value("zone_id", std::string{})},
                  {"state", state},
                  {"expiresAt", expires_at},
                  {"remainingMs", remaining_ms},
                  {"fenceToken", lease.value("fence_token", 0)}}}};
      }
      if (d == Dialect::kConformance) {
        // conformance reads a FLAT result with `granted` and `expires_ms`.
        return {{"granted", granted},
                {"lease_id", lease.value("lease_id", std::string{})},
                {"zone_id", lease.value("zone_id", std::string{})},
                {"robot_id", lease.value("robot_id", std::string{})},
                {"expires_ms", static_cast<int64_t>(expires_ms)},
                {"state", state}};
      }
      json out = lease;
      out["valid"] = lease.value("valid", granted);
      if (!out.contains("deny_reason")) {
        out["deny_reason"] = granted ? json() : json("Auction lost or zone occupied");
      }
      return {{"lease", out}};
    }

    case Op::kLeaseRelease: {
      const bool released = c.value("released", false);
      json out{{"released", released}};
      // Python echoes lease_id back. `lease/release` is spelled identically in
      // the spec and in pmcp-python, so the dialect cannot be inferred from the
      // method name; every reader treats extra keys as ignorable, so always
      // echo it rather than dropping it for the wrong guess.
      if (has("lease_id")) out["lease_id"] = c["lease_id"];
      return out;
    }

    case Op::kEstop: {
      const bool active = c.value("active", true);
      if (d == Dialect::kV05) {
        return {{"estop", active}, {"ts", c.value("ts", now_seconds())}};
      }
      if (d == Dialect::kConformance) {
        return {{"engaged", active}};
      }
      json estop{{"robot_id", get_or("robot_id", json(""))},
                 {"triggered_at", get_or("triggered_at", json(now_seconds()))},
                 {"stop_category", 0}};
      if (has("source")) estop["source"] = c["source"];
      return {{"estop", estop}};
    }

    case Op::kEstopReset: {
      const bool was = c.value("was_estopped", false);
      if (d == Dialect::kConformance) return {{"engaged", false}, {"was_estopped", was}};
      return {{"was_estopped", was}, {"estopped", false}};
    }

    case Op::kMetrics: {
      const json m = get_or("metrics", c);
      if (d == Dialect::kConformance) {
        // conformance demands these exact camelCase names at the top level.
        return {{"actuationCount", m.value("calls_total", 0)},
                {"sensorReadCount", m.value("sensor_reads", 0)},
                {"safetyViolations", m.value("calls_blocked", 0)},
                {"avgActuationDurationMs", m.value("avg_duration_ms", 0.0)},
                {"uptimeSeconds", m.value("uptime_s", 0.0)},
                {"energyUsedJ", m.value("energy_used_j", 0.0)},
                {"connectedClients", m.value("connected_clients", 0)},
                {"lastHeartbeatMs", m.value("last_heartbeat_ms", 0)}};
      }
      return {{"metrics", m}};
    }

    case Op::kAuditList: {
      return {{"entries", get_or("entries", json::array())},
              {"total", c.value("total", 0)}};
    }

    case Op::kShadowPreview: {
      const json p = get_or("preview", c);
      // Python emits the schema v0.6.0 16-key shape with four hardcoded
      // nulls; v05 adds `verdict`. Emit the superset on every dialect: the
      // keys are disjoint, so a reader looking for one shape still finds it.
      json out = p;
      out["verdict"] = p.value("safe", false) && p.value("status", std::string("safe")) == "SAFE"
                           ? json("PASS")
                           : json("FAIL");
      for (const char* k : {"predicted_trajectory", "confidence", "monitoring", "determinism"}) {
        if (!out.contains(k)) out[k] = nullptr;  // never fabricate
      }
      if (!out.contains("actuation_name") && out.contains("actuation")) {
        out["actuation_name"] = out["actuation"];
      }
      if (!out.contains("robot_id")) out["robot_id"] = "";
      return {{"preview", out}};
    }

    case Op::kListPrompts:
      return {{"prompts", get_or("prompts", json::array())}};

    case Op::kGetPrompt:
      return {{"description", c.value("description", "")},
              {"messages", get_or("messages", json::array())}};

    default:
      // status, identity, constitution, set_log_level are bare objects with
      // one agreed shape per dialect.
      return c;
  }
}

// ---------------------------------------------------------------------------

std::string dump_ascii(const json& j) { return j.dump(-1, ' ', /*ensure_ascii=*/true); }

std::string dump_indent_ascii(const json& j, int indent) {
  return j.dump(indent, /*indent_char=*/' ', /*ensure_ascii=*/true);
}

// Silence unused-function warnings for helpers kept for symmetry.
namespace {
[[maybe_unused]] void unused_marker() {
  static std::mutex m;
  (void)m;
  std::random_device rd;
  (void)rd;
  (void)is_notification;
}
}  // namespace

}  // namespace pmcp