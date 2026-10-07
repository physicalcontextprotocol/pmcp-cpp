#include "pmcp/server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#include "pmcp/http.hpp"
#include "pmcp/transport.hpp"

namespace pmcp {
namespace {

double now_s() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

std::string random_hex(int n) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) s.push_back(kHex[rng() & 0xF]);
  return s;
}

// Recover a canonical actuation result from whatever shape a nested call came
// back in. Batch dispatch re-enters handle_message with a method name that may
// resolve to any dialect, so the inner result can be a flat conformance object,
// a typed content block (pmcp-python), or a text block holding a JSON string
// (v05).
json unwrap_actuation_result(const json& response) {
  auto it = response.find("result");
  if (it == response.end() || !it->is_object()) return {{"success", true}};

  // Already canonical (the conformance shape, or an in-process call).
  if (it->contains("success")) return *it;

  const auto content = it->find("content");
  if (content != it->end() && content->is_array() && !content->empty()) {
    const json& block = (*content)[0];
    const auto data = block.find("data");
    if (data != block.end() && data->is_object()) return *data;
    const auto text = block.find("text");
    if (text != block.end() && text->is_string()) {
      try {
        json parsed = json::parse(text->get<std::string>());
        if (parsed.is_object()) return parsed;
      } catch (const std::exception&) {
        // Not JSON after all; fall through to the success default below.
      }
    }
  }
  // isError is the only remaining signal.
  if (it->contains("isError")) return {{"success", !it->value("isError", false)}};
  return {{"success", true}};
}

json json_schema_of(const ActuationSpec& spec) {
  json props = json::object();
  json required = json::array();
  for (const auto& p : spec.parameters) {
    json prop{{"type", p.type}, {"description", p.description}};
    if (!p.unit.empty()) {
      // pmcp-python uses "x-unit"; v05 emits no unit at all. Emitting both is
      // inert for every reader.
      prop["x-unit"] = p.unit;
    }
    if (p.minimum) prop["minimum"] = *p.minimum;
    if (p.maximum) prop["maximum"] = *p.maximum;
    if (p.default_value) prop["default"] = *p.default_value;
    props[p.name] = std::move(prop);
    if (p.required) required.push_back(p.name);
  }
  return {{"type", "object"}, {"properties", props}, {"required", required}};
}

// The superset capability object. The v05 dialect nests each capability under
// its own key and puts the PCP extensions under experimental.pcp; pmcp-python
// uses flat names; pmcp-conformance only asserts that an `actuations` or
// `features` key exists. Emitting all three families satisfies all of them.
json default_capabilities() {
  return json{
      // v05 / MCP-native
      {"tools", {{"listChanged", true}}},
      {"resources", {{"subscribe", false}, {"listChanged", true}}},
      {"prompts", {{"listChanged", false}}},
      {"logging", json::object()},
      // pmcp-python
      {"actuations", {{"listChanged", true}}},
      {"sensors", {{"streaming", false}}},
      {"shadow", json::object()},
      {"constitution", json::object()},
      {"leases", json::object()},
      {"experimental",
       {{"pcp",
         {{"version", std::string(kPcVersion)},
          {"shadow", true},
          {"leases", true},
          {"estop", true},
          {"constitution", true}}}}}};
}

}  // namespace

// ---------------------------------------------------------------------------

Server::Server() : Server(ServerConfig{}) {}

Server::Server(ServerConfig cfg) : cfg_(std::move(cfg)), safety_(cfg_.safety_profile) {
  if (cfg_.robot_id.empty()) cfg_.robot_id = cfg_.name;
  started_at_ = now_s();
}

std::string join(const std::vector<std::string>& v) {
  std::ostringstream os;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) os << "; ";
    os << v[i];
  }
  return os.str();
}

Server::~Server() = default;

void Server::register_actuation(ActuationSpec spec, ActuationFn fn) {
  std::lock_guard<std::mutex> lock(mtx_);
  if (spec.robot_id.empty()) spec.robot_id = cfg_.robot_id;
  actuation_fns_[spec.name] = std::move(fn);
  actuations_[spec.name] = std::move(spec);
}

void Server::register_sensor(SensorSpec spec, SensorFn fn) {
  std::lock_guard<std::mutex> lock(mtx_);
  if (spec.robot_id.empty()) spec.robot_id = cfg_.robot_id;
  if (spec.uri.empty()) {
    spec.uri = "pcp://" + spec.robot_id + "/sensors/" + spec.name;
  }
  sensor_fns_[spec.name] = std::move(fn);
  sensors_[spec.name] = std::move(spec);
}

void Server::register_prompt(PromptSpec spec, PromptFn fn) {
  std::lock_guard<std::mutex> lock(mtx_);
  prompt_fns_[spec.name] = std::move(fn);
  prompts_[spec.name] = std::move(spec);
}

// ---------------------------------------------------------------------------
// Catalogs
// ---------------------------------------------------------------------------

json Server::actuator_catalog() const {
  std::lock_guard<std::mutex> lock(mtx_);
  json out = json::array();
  for (const auto& [name, spec] : actuations_) {
    json physical{
        {"max_speed", spec.max_speed_m_s ? json(*spec.max_speed_m_s) : json()},
        {"max_force", spec.max_force_n ? json(*spec.max_force_n) : json()},
        {"max_energy", spec.max_energy_j ? json(*spec.max_energy_j) : json()},
        {"requires_lease", spec.requires_lease},
        {"shadow_required", spec.shadow_required},
        {"robot_class", spec.iso_class},
        {"safety_category", spec.category}};
    // v05 reads these from `annotations`, with unit-suffixed names.
    json annotations{
        {"robot_id", spec.robot_id},
        {"category", spec.category},
        {"max_speed_m_s", spec.max_speed_m_s ? json(*spec.max_speed_m_s) : json()},
        {"max_force_n", spec.max_force_n ? json(*spec.max_force_n) : json()},
        {"max_energy_j", spec.max_energy_j ? json(*spec.max_energy_j) : json()},
        {"est_duration_s", spec.est_duration_s},
        {"requires_lease", spec.requires_lease},
        {"shadow_required", spec.shadow_required},
        {"iso_class", spec.iso_class},
        {"protocol", std::string(kProtocolTag)}};
    out.push_back({{"name", name},
                   {"description", spec.description},
                   {"inputSchema", json_schema_of(spec)},
                   {"physical", physical},
                   {"annotations", annotations}});
  }
  return out;
}

json Server::sensor_catalog() const {
  std::lock_guard<std::mutex> lock(mtx_);
  json out = json::array();
  for (const auto& [name, spec] : sensors_) {
    json annotations{{"robot_id", spec.robot_id},
                     {"sensor_type", spec.sensor_type},
                     {"unit", spec.unit},
                     {"hz", spec.hz},
                     {"is_stream", spec.is_stream},
                     {"protocol", std::string(kProtocolTag)}};
    json physical{{"sensor_type", spec.sensor_type},
                  {"unit", spec.unit},
                  {"sample_rate_hz", spec.hz},
                  {"streaming", spec.is_stream}};
    out.push_back({{"name", name},
                   {"uri", spec.uri},
                   {"description", spec.description},
                   {"mimeType", "application/json"},
                   {"annotations", annotations},
                   {"physical", physical}});
  }
  return out;
}

json Server::prompt_catalog() const {
  std::lock_guard<std::mutex> lock(mtx_);
  json out = json::array();
  for (const auto& [name, spec] : prompts_) {
    json args = json::array();
    for (const auto& a : spec.arguments) {
      args.push_back({{"name", a.name}, {"description", a.description}, {"required", a.required}});
    }
    out.push_back({{"name", name}, {"description", spec.description}, {"arguments", args}});
  }
  return out;
}

json Server::metrics() {
  std::lock_guard<std::mutex> lock(mtx_);
  safety_.evict_expired();
  return {
      {"server_name", cfg_.name},
      {"uptime_s", std::round((now_s() - started_at_) * 10.0) / 10.0},
      {"calls_total", call_count_},
      {"calls_blocked", blocked_count_},
      {"calls_executed", call_count_ - blocked_count_},
      {"shadow_blocks", blocked_count_},
      {"constitution_blocks", blocked_count_},
      {"lease_denials", lease_denials_},
      {"active_leases", static_cast<int>(safety_.active_lease_count())},
      {"actuations_registered", static_cast<int>(actuations_.size())},
      {"sensors_registered", static_cast<int>(sensors_.size())},
      {"audit_entries", static_cast<int>(audit_log_.size())},
      {"sensor_reads", sensor_reads_},
      {"connected_clients", connected_clients_},
      {"energy_used_j", energy_used_j_},
      {"avg_duration_ms", call_count_ > 0 ? (total_duration_s_ / static_cast<double>(call_count_)) *
                                                1000.0
                                          : 0.0},
      {"last_heartbeat_ms", static_cast<int64_t>(now_s() * 1000.0)},
      {"timestamp", now_s()}};
}

json Server::status() const {
  std::lock_guard<std::mutex> lock(mtx_);
  json st = safety_.stats();
  return {{"robot_id", cfg_.robot_id},
          {"pcp_version", std::string(kPcVersion)},
          {"uptime_s", started_at_ > 0.0 ? std::round((now_s() - started_at_) * 10.0) / 10.0 : 0},
          {"call_count", call_count_},
          {"blocked_count", blocked_count_},
          {"safety_stats", st},
          {"estop_active", safety_.estop_active()},
          {"actuations", json::array()},
          {"sensors", json::array()},
          {"missions", json::array()}};
}

json Server::identity() const {
  // Matches RobotIdentity.to_dict(): `class` and `firmware`, not robot_class
  // and firmware_ver.
  return {{"did", "did:pcp:arm:pmcp-cpp:local:" + random_hex(8)},
          {"class", "arm"},
          {"model", "pmcp-cpp"},
          {"serial", cfg_.robot_id},
          {"firmware", cfg_.version},
          {"location", "local"}};
}

std::vector<std::pair<std::string, SensorSpec>> Server::catalog_for_bridge() const {
  std::lock_guard<std::mutex> lock(mtx_);
  std::vector<std::pair<std::string, SensorSpec>> out;
  out.reserve(sensors_.size());
  for (const auto& kv : sensors_) out.emplace_back(kv.first, kv.second);
  return out;
}

std::vector<std::string> Server::actuation_names_for_bridge() const {
  std::lock_guard<std::mutex> lock(mtx_);
  std::vector<std::string> out;
  out.reserve(actuations_.size());
  for (const auto& kv : actuations_) out.push_back(kv.first);
  return out;
}

std::vector<std::pair<std::string, ActuationSpec>> Server::actuations_for_bridge() const {
  std::lock_guard<std::mutex> lock(mtx_);
  std::vector<std::pair<std::string, ActuationSpec>> out;
  out.reserve(actuations_.size());
  for (const auto& kv : actuations_) out.emplace_back(kv.first, kv.second);
  return out;
}

std::vector<std::string> Server::served_methods() const {
  std::vector<std::string> out;
  for (Op op : {Op::kInitialize, Op::kPing, Op::kListActuations, Op::kCallActuation,
                Op::kBatchActuation, Op::kListSensors, Op::kReadSensor, Op::kListPrompts,
                Op::kGetPrompt, Op::kShadowPreview, Op::kLeaseRequest, Op::kLeaseRelease,
                Op::kEstop, Op::kEstopReset, Op::kMetrics, Op::kAuditList, Op::kStatus,
                Op::kIdentity, Op::kConstitution, Op::kSetLogLevel}) {
    for (auto& a : aliases_for(op)) {
      if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(std::move(a));
    }
  }
  return out;
}

void Server::audit(const std::string& event_type, const std::string& outcome, const json& extra) {
  std::lock_guard<std::mutex> lock(mtx_);
  audit_locked(event_type, outcome, extra);
}

// Split out because several dispatch paths already hold mtx_ when they audit.
// std::mutex is not recursive, so routing those through audit() deadlocks;
// audit_locked() is the caller-must-hold form.
void Server::audit_locked(const std::string& event_type, const std::string& outcome,
                          const json& extra) {
  // `{}` is a null json, not an empty object — a call site writing
  // audit_locked(..., {}) passes null, and nlohmann's value() on null throws
  // type_error 306 rather than returning the default. Normalize first.
  const json e_in = extra.is_object() ? extra : json::object();
  json e{{"event_type", event_type},
         {"timestamp", now_s()},
         {"robot_id", cfg_.robot_id},
         {"actuation_name", e_in.value("actuation_name", std::string{})},
         {"call_id", e_in.value("call_id", std::string{})},
         {"outcome", outcome},
         {"violations", e_in.value("violations", json::array())},
         {"constitution_fp", safety_.fingerprint()},
         {"lease_id", e_in.value("lease_id", std::string{})},
         {"duration_s", e_in.value("duration_s", 0.0)},
         {"energy_j", e_in.value("energy_j", 0.0)}};
  audit_log_.push_back(std::move(e));
  if (audit_log_.size() > cfg_.max_audit_entries) {
    // Match the reference: truncate to the most recent half, then continue.
    audit_log_.erase(audit_log_.begin(), audit_log_.begin() + cfg_.max_audit_entries / 2);
  }
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

json Server::dispatch(Op op, const json& p, const json& id) {
  switch (op) {
    case Op::kInitialize: {
      started_at_ = now_s();
      return json{{"protocol_version", std::string(kPcVersion)},
                  {"server_info",
                   {{"name", cfg_.name}, {"version", cfg_.version}, {"robot_id", cfg_.robot_id}}},
                  {"capabilities", default_capabilities()},
                  {"instructions",
                   "This is a PCP physical robot server. Use actuations/list to discover "
                   "physical commands, sensors/list for available sensor streams. All "
                   "actuations require shadow/preview to pass before execution."},
                  {"pcp",
                   {{"version", std::string(kPcVersion)},
                    {"robotId", cfg_.robot_id},
                    {"identity", identity()},
                    {"constitution", safety_.fingerprint()}}},
                  {"_debug_id", id}};
    }

    case Op::kPing: {
      std::lock_guard<std::mutex> lock(mtx_);
      return {{"pong", true},
              {"ts", now_s()},
              {"robot_id", cfg_.robot_id},
              {"uptime_s", now_s() - started_at_}};
    }

    case Op::kListActuations:
      return {{"actuations", actuator_catalog()}};

    case Op::kCallActuation: {
      std::string name = p.value("name", std::string{});
      json args = p.value("arguments", json::object());
      ActuationFn fn;
      ActuationSpec spec;
      {
        std::lock_guard<std::mutex> lock(mtx_);
        ++call_count_;
        auto it = actuation_fns_.find(name);
        if (it == actuation_fns_.end()) {
          ++blocked_count_;
          std::vector<std::string> names;
          for (const auto& [n, _] : actuations_) names.push_back(n);
          audit_locked("actuation_failed", "not_found", {{"actuation_name", name}});
          std::ostringstream os;
          os << "Actuation '" << name << "' not found. Available: [";
          for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) os << ", ";
            os << "'" << names[i] << "'";
          }
          os << "]";
          throw Error(Code::kMethodNotFound, os.str());
        }
        fn = it->second;
        spec = actuations_.at(name);
      }

      const std::string lease_token = p.value("lease_token", std::string{});
      const std::string zone_id = p.value("zone_id", std::string{});
      std::optional<std::int64_t> fence;
      if (p.contains("fence_token") && p["fence_token"].is_number_integer()) {
        fence = p["fence_token"].get<std::int64_t>();
      }

      json shadow_json = nullptr;
      CheckOutcome outcome;
      if (cfg_.enable_safety) {
        outcome = safety_.check(name, args, lease_token, zone_id, fence,
                                /*skip_shadow=*/!spec.shadow_required,
                                /*require_lease=*/spec.requires_lease);
        if (!outcome.safe) {
          std::lock_guard<std::mutex> lock(mtx_);
          ++blocked_count_;
          audit_locked(outcome.code == Code::kEstopActive ? "estop_triggered" : "constitution_blocked",
                "blocked",
                {{"actuation_name", name}, {"violations", outcome.violations}});
          if (outcome.shadow) shadow_json = outcome.shadow->to_canonical();
          Code c = outcome.code == Code::kEstopActive ? Code::kConstitutionBlocked : outcome.code;
          throw Error(c, join(outcome.violations), {{"violations", outcome.violations},
                                                    {"shadow", shadow_json}});
        }
        if (outcome.shadow) shadow_json = outcome.shadow->to_canonical();
      }

      const double t0 = now_s();
      ActuationOutcome r;
      try {
        r = fn ? fn(args) : ActuationOutcome{};
      } catch (const Error& e) {
        std::lock_guard<std::mutex> lock(mtx_);
        audit_locked("actuation_failed", "failed", {{"actuation_name", name}});
        throw;
      } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mtx_);
        audit_locked("actuation_failed", "failed", {{"actuation_name", name}});
        throw Error(Code::kInternalError, "Actuation '" + name + "' raised: " + e.what());
      }
      const double dur = now_s() - t0;

      std::lock_guard<std::mutex> lock(mtx_);
      energy_used_j_ += r.energy_j;
      total_duration_s_ += dur;
      audit_locked("actuation_executed", r.success ? "executed" : "failed",
            {{"actuation_name", name},
             {"duration_s", dur},
             {"energy_j", r.energy_j},
             {"lease_id", lease_token}});
      return {{"success", r.success},
              {"actuation_id", random_hex(12)},
              {"robot_id", p.value("robot_id", cfg_.robot_id)},
              {"actuation_name", name},
              {"output", r.output},
              {"final_pose", r.final_pose},
              {"duration_s", std::round(dur * 10000.0) / 10000.0},
              {"duration_ms", std::round(dur * 1000.0 * 1000.0) / 1000.0},
              {"energy_j", r.energy_j},
              {"shadow_delta_m", 0.0},
              {"error", r.error.empty() ? json() : json(r.error)},
              {"metadata", json::object()},
              {"shadow", shadow_json}};
    }

    case Op::kBatchActuation: {
      const json items = p.value("actuations", json::array());
      if (items.empty()) {
        throw Error(Code::kInvalidParams, "actuations/batch requires at least one actuation");
      }
      const bool atomic = p.value("atomic", true);
      const std::string zone = p.value("zone_id", std::string("default"));
      const std::string robot = p.value("robot_id", cfg_.robot_id);

      // Python's _handle_actuations_batch acquires one shared lease for the
      // whole batch and hands each inner call its lease_token + fence_token,
      // so items that require a lease still clear the lease stage.
      json lease_resp =
          dispatch(Op::kLeaseRequest,
                   {{"robot_id", robot}, {"zone_id", zone}, {"duration_ms", 30000}}, id);
      const json lease = lease_resp.value("lease", json::object());
      if (lease.value("state", std::string{}) != "ACTIVE") {
        throw Error(Code::kLeaseRequired,
                    "Batch lease denied: " +
                        lease.value("deny_reason", std::string("zone occupied")));
      }
      const std::string lease_token = lease.value("lease_id", std::string{});

      json results = json::array();
      bool ok = true;
      int failed_at = -1;
      std::string batch_error;
      for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        // `arguments` is what pmcp-python and v05 call the bag; `params` is
        // what pmcp-conformance calls it. Take whichever the item carried.
        const json& args = it.contains("arguments") ? it["arguments"]
                                                    : it.value("params", json::object());
        json inner{{"name", it.value("name", it.value("actuation_name", std::string{}))},
                   {"arguments", args},
                   {"zone_id", zone},
                   {"robot_id", robot},
                   {"lease_token", lease_token}};
        if (lease.contains("fence_token")) inner["fence_token"] = lease["fence_token"];
        json req{{"jsonrpc", "2.0"},
                 {"id", id},
                 {"method", "actuations/call"},
                 {"params", inner}};
        auto resp = handle_message(req);
        if (resp && resp->contains("error")) {
          ok = false;
          failed_at = static_cast<int>(i);
          batch_error = resp->value("error", json::object()).value("message", "");
          json canon = canonical_params(Op::kCallActuation, "actuations/call", req["params"]);
          (void)canon;
          results.push_back({{"error", batch_error}, {"index", static_cast<int>(i)}});
          if (atomic) break;
        } else {
          // Unwrap to the canonical actuation result so the dialect layer can
          // re-wrap consistently per caller. The caller may have used any of
          // the three result shapes, so try each in turn.
          json canon = unwrap_actuation_result(*resp);
          canon["duration_ms"] = canon.value("duration_s", 0.0) * 1000.0;
          canon["actuation_name"] = it.value("name", std::string{});
          results.push_back(canon);
        }
      }
      const std::string bid = id.is_string() ? id.get<std::string>() : random_hex(8);
      return {{"results", results},
              {"success", ok},
              {"failed_at", failed_at < 0 ? json() : json(failed_at)},
              {"error", batch_error.empty() ? json() : json(batch_error)},
              {"batch_id", bid + "-batch"}};
    }

    case Op::kListSensors:
      return {{"sensors", sensor_catalog()}};

    case Op::kReadSensor: {
      const std::string name = p.value("name", std::string{});
      SensorFn fn;
      SensorSpec spec;
      {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = sensor_fns_.find(name);
        if (it == sensor_fns_.end()) {
          throw Error(Code::kMethodNotFound, "Sensor '" + name + "' not found");
        }
        fn = it->second;
        spec = sensors_.at(name);
        ++sensor_reads_;
      }
      json v = fn ? fn() : json();
      return {{"name", name},
              {"uri", spec.uri},
              {"robot_id", spec.robot_id},
              {"unit", spec.unit},
              {"value", v},
              {"timestamp", now_s()},
              {"quality", 1.0}};
    }

    case Op::kListPrompts:
      return {{"prompts", prompt_catalog()}};

    case Op::kGetPrompt: {
      const std::string name = p.value("name", std::string{});
      PromptFn fn;
      PromptSpec spec;
      {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = prompt_fns_.find(name);
        if (it == prompt_fns_.end()) {
          throw Error(Code::kMethodNotFound, "Prompt '" + name + "' not found");
        }
        fn = it->second;
        spec = prompts_.at(name);
      }
      json out = fn ? fn(p.value("arguments", json::object())) : json::object();
      if (out.is_string()) {
        return {{"description", spec.description},
                {"messages", json::array({{{"role", "user"},
                                            {"content", {{"type", "text"}, {"text", out}}}}})}};
      }
      json description = out.value("description", spec.description);
      json messages = out.value("messages", json::array());
      return {{"description", description}, {"messages", messages}};
    }

    case Op::kShadowPreview: {
      const std::string name = p.value("name", std::string{});
      {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!actuations_.contains(name)) {
          throw Error(Code::kMethodNotFound, "Actuation '" + name + "' not found");
        }
      }
      // shadow/preview bypasses the lease and constitution layers in both
      // reference SDKs; only tools/call runs the full pipeline.
      auto pv = safety_.preview(name, p.value("arguments", json::object()));
      pv.robot_id = cfg_.robot_id;
      return {{"preview", pv.to_canonical()}};
    }

    case Op::kLeaseRequest: {
      const std::string robot = p.value("robot_id", cfg_.robot_id);
      const std::string zone = p.value("zone_id", std::string("default"));
      const std::int64_t duration = p.value("duration_ms", static_cast<std::int64_t>(10'000));
      const double bid = p.value("bid_energy_j", 100.0);
      auto lease = safety_.request_lease(robot, zone, duration, bid);
      if (lease.state != "ACTIVE") {
        std::lock_guard<std::mutex> lock(mtx_);
        ++lease_denials_;
        audit_locked("lease_denied", "denied", {{"lease_id", lease.lease_id}});
      } else {
        audit("lease_granted", "granted", {{"lease_id", lease.lease_id}});
      }
      return {{"lease", lease.to_canonical()}};
    }

    case Op::kLeaseRelease: {
      const std::string id_str = p.value("lease_id", std::string{});
      const bool released = safety_.release_lease(id_str);
      audit("lease_released", released ? "released" : "not_found", {{"lease_id", id_str}});
      return {{"released", released}, {"lease_id", id_str}};
    }

    case Op::kEstop: {
      const bool active = p.value("active", true);
      safety_.set_estop(active);
      std::lock_guard<std::mutex> lock(mtx_);
      ++estop_count_;
      audit_locked("estop_triggered", active ? "estopped" : "estop_reset", {});
      json out{{"active", active}, {"engaged", active}};
      if (p.contains("source")) out["source"] = p["source"];
      if (active) out["triggered_at"] = now_s();
      return out;
    }

    case Op::kEstopReset: {
      const bool was = safety_.estop_active();
      safety_.set_estop(false);
      audit("estop_triggered", "estop_reset", {});
      return {{"active", false}, {"engaged", false}, {"was_estopped", was}};
    }

    case Op::kMetrics:
      return {{"metrics", metrics()}};

    case Op::kAuditList: {
      std::lock_guard<std::mutex> lock(mtx_);
      const int limit = p.value("limit", 100);
      json entries = json::array();
      const double since = p.value("since", 0.0);
      const std::string robot_filter = p.value("robot_id", std::string{});
      const std::string type_filter = p.value("event_type", std::string{});
      for (const auto& e : audit_log_) {
        if (e.value("timestamp", 0.0) < since) continue;
        if (!robot_filter.empty() && e.value("robot_id", std::string{}) != robot_filter) continue;
        if (!type_filter.empty() && e.value("event_type", std::string{}) != type_filter) continue;
        entries.push_back(e);
      }
      const int n = std::min<int>(limit, static_cast<int>(entries.size()));
      json tail = json::array();
      for (int i = static_cast<int>(entries.size()) - n; i < static_cast<int>(entries.size());
           ++i) {
        tail.push_back(entries[i]);
      }
      // `total` is the unfiltered log length in pmcp-python; keep that.
      return {{"entries", tail}, {"total", static_cast<int>(audit_log_.size())}};
    }

    case Op::kStatus: {
      json st = status();
      std::lock_guard<std::mutex> lock(mtx_);
      st["actuations"] = json::array();
      st["sensors"] = json::array();
      st["missions"] = json::array();
      for (const auto& [n, _] : actuations_) st["actuations"].push_back(n);
      for (const auto& [n, _] : sensors_) st["sensors"].push_back(n);
      for (const auto& [n, _] : prompts_) st["missions"].push_back(n);
      return st;
    }

    case Op::kIdentity:
      return identity();

    case Op::kConstitution:
      return safety_.constitution_summary(cfg_.robot_id);

    case Op::kSetLogLevel:
      // Every reference implementation returns the empty object here.
      return json::object();
  }
  throw Error(Code::kMethodNotFound, "Unhandled operation");
}

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

std::optional<json> Server::handle_message(const json& request) {
  if (!request.is_object()) {
    return json{{"jsonrpc", kJsonRpcVersion},
                {"id", nullptr},
                {"error", Error(Code::kInvalidRequest, "Invalid Request").to_json()}};
  }

  // Neither reference SDK validates the incoming jsonrpc member, so neither
  // do we; a request with "1.0" or none at all is accepted.
  const bool has_id = request.contains("id") && !request["id"].is_null();
  const json id = has_id ? request["id"] : json();

  const std::string method = request.value("method", std::string{});

  // A request with no id is a notification. Both servers write nothing on
  // stdio; HTTP writes {}. We return nullopt and let the transport decide.
  if (!has_id) {
    if (method == "notifications/initialized") {
      std::lock_guard<std::mutex> lock(mtx_);
      ++connected_clients_;
    }
    return std::nullopt;
  }

  const json params = request.contains("params") ? request["params"] : json::object();

  // Echo the id verbatim, with no type coercion, as both servers do.
  auto err = [&](Code c, const std::string& msg, const json& data = nullptr) -> json {
    return json{{"jsonrpc", kJsonRpcVersion},
                {"id", id},
                {"error", Error(c, msg, data).to_json()}};
  };

  auto op = resolve_op(method);
  if (!op) {
    return err(Code::kMethodNotFound, "Unknown method: " + method);
  }

  // Response dialect follows the request dialect unless the server was pinned.
  Dialect out_dialect = cfg_.dialect;
  if (out_dialect == Dialect::kAuto) {
    out_dialect = dialect_of_method(method).value_or(Dialect::kPython);
    // `pcp/estop` is declared by pmcp-python (which always engages and answers
    // with a nested estop object) and by v05 (which takes {active: bool} and
    // answers with a flat bool). The method name alone cannot tell them apart,
    // but a caller that sends `active` is speaking v05, so answer in v05.
    if (*op == Op::kEstop && params.is_object() && params.contains("active")) {
      out_dialect = Dialect::kV05;
    } else if (params.is_object()) {
      // v05 is the only dialect that writes camelCase params (leaseId,
      // robotId, zoneId, durationMs, fenceToken, ...). Every other dialect uses
      // snake_case (zone_id) or single words (name, arguments, params). When
      // the method name is shared (lease/request, shadow/preview) the presence
      // of a camelCase key tells us who is on the line: answer in v05.
      for (const auto& [k, _] : params.items()) {
        bool has_upper = false;
        for (const char c : k) {
          if (c >= 'A' && c <= 'Z') {
            has_upper = true;
            break;
          }
        }
        if (has_upper) {
          out_dialect = Dialect::kV05;
          break;
        }
      }
    }
  }

  const json cparams = canonical_params(*op, method, params);
  try {
    json canonical = dispatch(*op, cparams, id);
    // Strip the debug echo before serializing.
    canonical.erase("_debug_id");
    return json{{"jsonrpc", kJsonRpcVersion}, {"id", id}, {"result", wire_result(*op, out_dialect, canonical)}};
  } catch (const Error& e) {
    return json{{"jsonrpc", kJsonRpcVersion}, {"id", id}, {"error", e.to_json()}};
  } catch (const std::exception& e) {
    return err(Code::kInternalError, e.what());
  }
}

json Server::call(const std::string& method, const json& params, const json& id) {
  auto r = handle_message(
      json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
  return r ? *r : json::object();
}

// ---------------------------------------------------------------------------
// Transports
// ---------------------------------------------------------------------------

int Server::serve_stdio(std::istream* in, std::ostream* out) {
  StdioTransport t(in ? in : &std::cin, out ? out : &std::cout);
  if (!t.connect()) return 1;
  while (auto msg = t.receive()) {
    if (msg->is_object() && msg->contains("__pmcp_parse_error__")) {
      // pmcp-python/pcp answers an unparseable line with id "" and keeps going.
      t.send(json{{"jsonrpc", kJsonRpcVersion},
                  {"id", ""},
                  {"error", Error(Code::kParseError, "Parse error").to_json()}});
      continue;
    }
    auto resp = handle_message(*msg);
    if (resp) t.send(*resp);  // notifications write nothing
  }
  t.close();
  return 0;
}

int Server::serve_http(const std::string& host, int port) {
  HttpServer http;
  http.set_handler(
      [this](const std::string& path, const std::string& body) -> HttpResponse {
        return handle_http_request(path, body);
      });
  http.set_paths(cfg_.http_paths);
  int bound = port;
  const int rc = http.listen(host, port, &bound);
  // Publish the actual port. With port 0 the OS picks one, and a caller that
  // passed 0 needs to learn what it got — the interop harness relies on this.
  if (bound > 0) bound_port_.store(bound);
  return rc;
}

HttpResponse Server::handle_http_request(const std::string& /*path*/, const std::string& body) {
  HttpResponse r;
  json req;
  try {
    req = json::parse(body);
  } catch (const std::exception& e) {
    // Matches pmcp-python/pcp: HTTP 400, id null, and — note — no
    // Access-Control-Allow-Origin on this path.
    r.status = 400;
    r.allow_origin = false;
    r.body = json{{"jsonrpc", kJsonRpcVersion},
                  {"id", nullptr},
                  {"error", Error(Code::kParseError, std::string("Parse error: ") + e.what()).to_json()}}
                   .dump();
    return r;
  }

  auto resp = handle_message(req);
  // A notification yields nullopt; pmcp-python/pcp writes {} in that case.
  r.body = (resp ? *resp : json::object()).dump();
  r.status = 200;
  return r;
}

}  // namespace pmcp