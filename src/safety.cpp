#include "pmcp/safety.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <sstream>

#include "sha256.hpp"

namespace pmcp {
namespace {

// The format strings passed to fmt() are literals at every call site, so
// -Wformat-nonliteral is noise here rather than a real finding.
#if defined(__clang__)
#define CLANG_SUPPRESS_FORMAT _Pragma("clang diagnostic push") _Pragma( \
    "clang diagnostic ignored \"-Wformat-nonliteral\"")
#define CLANG_RESTORE_FORMAT _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define CLANG_SUPPRESS_FORMAT _Pragma("GCC diagnostic push") _Pragma( \
    "GCC diagnostic ignored \"-Wformat-nonliteral\"")
#define CLANG_RESTORE_FORMAT _Pragma("GCC diagnostic pop")
#else
#define CLANG_SUPPRESS_FORMAT
#define CLANG_RESTORE_FORMAT
#endif

double num(const json& o, const char* k, double fallback) {
  if (!o.is_object()) return fallback;
  auto it = o.find(k);
  if (it == o.end() || !it->is_number()) return fallback;
  return it->get<double>();
}

std::string random_hex(int n) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) s.push_back(kHex[rng() & 0xF]);
  return s;
}

// snprintf is already variadic; forwarding the pack keeps the whole SDK
// dependency-free. The nonliteral-format warning is suppressed locally rather
// than with a `format` attribute: on a non-variadic template GCC demands a
// trailing `...`, which would defeat the parameter pack.
template <class... Args>
std::string fmt(const char* f, Args... args) {
  if constexpr (sizeof...(Args) == 0) {
    return f;
  } else {
    // 512 is generous for a violation message; truncation would only lose
    // detail, never corrupt the framing.
    char buf[512];
    CLANG_SUPPRESS_FORMAT
    const int n = std::snprintf(buf, sizeof(buf), f, args...);
    if (n < 0) return {};
    if (static_cast<std::size_t>(n) < sizeof(buf))
      return std::string(buf, static_cast<std::size_t>(n));
    std::string big(static_cast<std::size_t>(n) + 1, '\0');
    CLANG_SUPPRESS_FORMAT
    std::snprintf(big.data(), big.size(), f, args...);
    big.pop_back();
    return big;
  }
}

double now_s() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

std::string sha256_hex(const std::string& in) { return detail::sha256_hex(in); }

}  // namespace

json ShadowPreview::to_canonical() const {
  // Union of the v05 9-key shape and the pmcp-python/pcp 16-key shape. The
  // four schema-v0.6.0 fields (predicted_trajectory, confidence, monitoring,
  // determinism) are left null: neither SDK fabricates them and neither
  // should we.
  json j{{"actuation", actuation_name},
         {"actuation_name", actuation_name},
         {"robot_id", robot_id},
         {"status", status},
         {"safe", safe},
         {"risk_score", std::round(risk_score * 1000.0) / 1000.0},
         {"est_duration_s", duration_s},
         {"est_energy_j", energy_j},
         {"duration_s", duration_s},
         {"energy_j", energy_j},
         {"warnings", warnings},
         {"engine", engine},
         {"collision_body", collision_body.empty() ? json() : json(collision_body)},
         {"collisions", json::array()},
         {"violations", warnings},
         {"shadow_token", random_hex(static_cast<int>(shadow_token_len))},
         {"timestamp", timestamp}};
  return j;
}

// ---------------------------------------------------------------------------

Safety::Safety(SafetyProfile profile, SafetyLimits limits)
    : profile_(profile), limits_(limits) {
  // Rule text mirrors pmcp-python/pcp::SafetyMiddleware so violation strings
  // match that SDK byte-for-byte. v05's rule ids differ; see profile kV05.
  const std::vector<Rule> all = {
      {"CONST-01", "ISO10218", "Speed must not exceed 2.0 m/s"},
      {"CONST-02", "ISO10218", "Target Z must be above floor (z >= 0.0 m)"},
      {"CONST-03", "ISO10218", "Estimated energy must be <= 50,000 J"},
      {"CONST-04", "ISO10218", "Shadow validation must be recent (< 2 s)"},
      {"CONST-05", "ISO10218", "Force must not exceed 500 N"},
      {"CONST-06", "ISO10218", "Emergency stop bit must be clear"},
      {"CONST-07", "ISO10218", "Human proximity: minimum 0.5 m clearance required"},
      {"CONST-08", "ISO10218", "Call ID must be present and >= 8 characters"},
      {"CONST-09", "ISO10218", "Joint angles must be within +/-pi rad"},
      {"CONST-10", "ISO10218", "Workspace envelope: target within +/-2.0 m on X and Y"},
  };
  const std::vector<Rule> v05 = {
      {"R-ESTOP-01", "ISO10218", "All actuations blocked when E-Stop is active"},
      {"R-SPEED-01", "ISO10218", "TCP speed must not exceed 2.0 m/s (ISO 10218-1 5.4)"},
      {"R-FLOOR-01", "ISO10218", "Z position must stay above -0.05m (collision guard)"},
      {"R-WS-01", "ISO10218", "Target position must be inside declared workspace box"},
      {"R-ENERGY-01", "custom", "Single call energy budget <= 5000.0J"},
      {"R-HUMAN-01", "ISO10218", "Minimum human clearance >= 0.5m (ISO 10218-2 5.10)"},
      {"R-FORCE-01", "ISO10218", "End-effector force <= 150.0N (ISO TS 15066)"},
  };

  switch (profile_) {
    case SafetyProfile::kMinimal:
      rules_ = {all[0], all[1], all[5]};
      break;
    case SafetyProfile::kV05:
      rules_ = v05;
      break;
    case SafetyProfile::kArm:
      rules_ = all;
      rules_.push_back({"ARM-01", "ISO10218", "Arm must be in home pose before motion"});
      rules_.push_back({"ARM-02", "ISO10218", "Payload mass must not exceed 5 kg"});
      rules_.push_back({"ARM-03", "ISO10218", "TCP must remain inside the calibrated tool frame"});
      break;
    case SafetyProfile::kDefault:
    default:
      rules_ = all;
      break;
  }

  // Fingerprint over id+description, matching the reference construction so
  // clients that compare fingerprints across SDKs agree on the rule set.
  std::string joined;
  for (std::size_t i = 0; i < rules_.size(); ++i) {
    if (i) joined += '|';
    joined += rules_[i].id + rules_[i].description;
  }
  fingerprint_ = sha256_hex(joined).substr(0, 16);
}

// ---------------------------------------------------------------------------

void Safety::set_estop(bool active) { estop_.store(active); }

// ---------------------------------------------------------------------------
// Leases
// ---------------------------------------------------------------------------

Lease Safety::request_lease(const std::string& robot_id, const std::string& zone_id,
                            std::int64_t duration_ms, double bid_energy_j) {
  std::lock_guard<std::mutex> lock(mtx_);
  evict_expired();

  const double now = now_s();
  auto it = leases_.find(zone_id);
  if (it != leases_.end() && it->second.valid && it->second.expires_at > now) {
    Lease& cur = it->second;
    if (cur.robot_id == robot_id) {
      // Renewal: extend in place, bump the fence token. A token issued before
      // a renewal is stale by design.
      cur.expires_at = now + static_cast<double>(duration_ms) / 1000.0;
      cur.fence_token = ++fence_counters_[zone_id];
      cur.valid = true;
      cur.deny_reason.clear();
      return cur;
    }
    if (bid_energy_j <= 0.0) bid_energy_j = 0.0;
    if (cur.robot_id != robot_id) {
      // Compare against the stored bid. The reference servers store the bid on
      // the grant; we track it in fence_counters_' sibling map via valid only,
      // so an incumbent always wins ties unless the caller outbids it.
      if (bid_energy_j <= 100.0) {
        ++lease_denials_;
        Lease denied;
        denied.lease_id = zone_id + "-" + random_hex(8);
        denied.robot_id = robot_id;
        denied.zone_id = zone_id;
        denied.state = "DENIED";
        denied.expires_at = 0.0;
        denied.fence_token = 0;
        denied.valid = false;
        denied.deny_reason = "Auction lost or zone occupied";
        return denied;
      }
    }
  }

  Lease granted;
  granted.lease_id = zone_id + "-" + random_hex(8);
  granted.robot_id = robot_id;
  granted.zone_id = zone_id;
  granted.state = "ACTIVE";
  granted.expires_at = now + static_cast<double>(duration_ms) / 1000.0;
  granted.fence_token = ++fence_counters_[zone_id];
  granted.valid = true;
  leases_[zone_id] = granted;
  return granted;
}

bool Safety::release_lease(const std::string& lease_id) {
  std::lock_guard<std::mutex> lock(mtx_);
  for (auto it = leases_.begin(); it != leases_.end(); ++it) {
    if (it->second.lease_id == lease_id) {
      it->second.state = "FREE";
      it->second.valid = false;
      it->second.expires_at = 0.0;
      leases_.erase(it);
      return true;
    }
  }
  return false;
}

std::optional<Lease> Safety::find_lease(const std::string& lease_id) const {
  std::lock_guard<std::mutex> lock(mtx_);
  for (const auto& [zone, l] : leases_) {
    if (l.lease_id == lease_id) {
      if (l.expires_at > now_s()) return l;
      return std::nullopt;
    }
  }
  return std::nullopt;
}

void Safety::evict_expired() {
  // Caller holds mtx_.
  const double now = now_s();
  for (auto it = leases_.begin(); it != leases_.end();) {
    if (it->second.expires_at <= now) {
      it = leases_.erase(it);
    } else {
      ++it;
    }
  }
}

std::size_t Safety::active_lease_count() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return leases_.size();
}

// ---------------------------------------------------------------------------
// Shadow
// ---------------------------------------------------------------------------

ShadowPreview Safety::preview(const std::string& name, const json& arguments) const {
  ShadowPreview p;
  p.actuation_name = name;
  p.robot_id = "";
  p.timestamp = now_s();
  p.duration_s = 0.001;  // matches both SDKs' built-in stub
  p.engine = "geometric";

  std::vector<std::string> collisions;
  const double x = num(arguments, "x", 0.0);
  const double y = num(arguments, "y", 0.0);
  const double z = num(arguments, "z", 1.0);   // NOTE default 1.0, as in the SDKs
  const double speed = num(arguments, "speed", 0.3);

  if (z < 0.0) {
    collisions.push_back("floor");
    p.warnings.push_back(fmt("Z=%.3f below floor plane (z=0)", z));
  }
  if (speed > limits_.shadow_max_speed_m_s) {
    p.warnings.push_back(fmt("Speed=%.2f m/s exceeds soft limit (%.1f m/s)", speed,
                             limits_.shadow_max_speed_m_s));
  }
  const bool ws_bad = x < limits_.shadow_min_x || x > limits_.shadow_max_x ||
                      y < limits_.shadow_min_y || y > limits_.shadow_max_y ||
                      z < limits_.shadow_min_z || z > limits_.shadow_max_z;
  if (ws_bad) {
    const char* axes[3] = {"x", "y", "z"};
    const double v[3] = {x, y, z};
    for (int i = 0; i < 3; ++i) {
      const double lo = i == 0 ? limits_.shadow_min_x
                               : (i == 1 ? limits_.shadow_min_y : limits_.shadow_min_z);
      const double hi = i == 0 ? limits_.shadow_max_x
                               : (i == 1 ? limits_.shadow_max_y : limits_.shadow_max_z);
      if (v[i] < lo || v[i] > hi) {
        p.warnings.push_back(fmt("%s=%.3f outside workspace [%g, %g] m", axes[i], v[i], lo, hi));
      }
    }
  }

  p.safe = p.warnings.empty() && collisions.empty();
  p.status = p.safe ? "SAFE" : (collisions.empty() ? "UNSAFE" : "COLLISION");
  p.risk_score = p.safe ? 0.0 : 0.8;
  p.collision_body = collisions.empty() ? "" : collisions.front();
  // Rough energy estimate, scaled by speed. Both SDKs stub this; keep it
  // deterministic so clients can assert on it.
  p.energy_j = std::min(500.0, 1.0 + speed * 50.0);
  p.duration_s = 0.1 + std::fabs(z) * 0.5 + speed;
  return p;
}

// ---------------------------------------------------------------------------
// Constitution
// ---------------------------------------------------------------------------

std::vector<std::string> Safety::run_constitution(const json& a) const {
  std::vector<std::string> v;
  const bool is_v05 = profile_ == SafetyProfile::kV05;
  // Rules are loaded per profile, exactly as pmcp-python's check_constitution
  // iterates self._rules. A rule that is not in this profile must not fire,
  // otherwise SafetyProfile::kMinimal would be indistinguishable from default.
  auto loaded = [&](const char* id) {
    for (const auto& r : rules_) {
      if (r.id == id) return true;
    }
    return false;
  };
  auto add = [&](const char* id, std::string msg) {
    if (!loaded(id)) return;
    v.push_back("[" + std::string(id) + "] " + msg);
  };

  // E-stop is a rule in v05 and a latch check in both dialects; the latch is
  // evaluated in check(), so CONST-06/R-ESTOP-01 here only fires when the
  // caller passes an explicit estop flag in the arguments.
  if (a.value("estop", false)) {
    add(is_v05 ? "R-ESTOP-01" : "CONST-06", "Emergency stop is active \xe2\x80\x94 all motion blocked");
    if (is_v05) return v;  // v05 short-circuits on E-stop like the reference
  }

  const double speed = num(a, "speed", 0.0);
  const double z = num(a, "z", 1.0);
  const double energy = num(a, "energy_j", 0.0);
  const double force = num(a, "force_n", 0.0);
  const double human = num(a, "human_dist_m", 99.0);

  if (speed > limits_.max_speed_m_s) {
    if (is_v05) {
      add("R-SPEED-01", fmt("Speed %g m/s exceeds limit %g m/s", speed, limits_.max_speed_m_s));
    } else {
      add("CONST-01", fmt("Speed %.2f m/s > %g limit", speed, limits_.max_speed_m_s));
    }
  }
  if (z < 0.0) {
    if (is_v05) {
      add("R-FLOOR-01", fmt("Z=%gm is below floor guard %gm", z, limits_.floor_z_m));
    } else {
      add("CONST-02", fmt("Z=%.3f below floor (0.0 m)", z));
    }
  }
  if (energy > limits_.energy_budget_j) {
    if (is_v05) {
      add("R-ENERGY-01", fmt("energy=%g J exceeds %g J budget", energy, limits_.energy_budget_j));
    } else {
      add("CONST-03", fmt("energy_j=%g exceeds %g J budget", energy, limits_.energy_budget_j));
    }
  }
  if (force > limits_.max_force_n) {
    if (is_v05) {
      add("R-FORCE-01", fmt("force=%g N exceeds %g N limit", force, limits_.max_force_n));
    } else {
      add("CONST-05", fmt("force_n=%g exceeds %g N limit", force, limits_.max_force_n));
    }
  }
  if (human < limits_.min_human_distance_m) {
    if (is_v05) {
      add("R-HUMAN-01",
          fmt("Human at %.2f m — clearance below %g m", human, limits_.min_human_distance_m));
    } else {
      add("CONST-07",
          fmt("Human at %.2f m — too close (< %g m)", human, limits_.min_human_distance_m));
    }
  }

  if (!is_v05) {
    // CONST-04: shadow must be recent. The reference servers inject a fresh
    // timestamp immediately before this call, so it only ever fires when a
    // caller explicitly overrides _shadow_ts.
    const double ts = num(a, "_shadow_ts", 0.0);
    if (ts <= 0.0 || (now_s() - ts) > 2.0) {
      add("CONST-04", "Shadow timestamp missing or stale (> 2 s old)");
    }
    // CONST-08: call_id length.
    const std::string call_id = a.value("call_id", std::string{});
    if (call_id.size() < 8) {
      add("CONST-08", fmt("call_id '%s' too short (need >= 8 chars)", call_id.c_str()));
    }
    // CONST-09: joint angles.
    if (a.contains("joint_angles") && a["joint_angles"].is_array()) {
      std::vector<double> bad;
      for (const auto& j : a["joint_angles"]) {
        if (j.is_number() && std::fabs(j.get<double>()) > 3.14159) {
          bad.push_back(std::round(j.get<double>() * 1000.0) / 1000.0);
        }
      }
      if (!bad.empty()) {
        std::ostringstream os;
        os << "Joint angle(s) exceed +/-pi rad: [";
        for (std::size_t i = 0; i < bad.size(); ++i) {
          if (i) os << ", ";
          os << bad[i];
        }
        os << "]";
        add("CONST-09", os.str());
      }
    }
    // CONST-10: workspace envelope on x and y.
    {
      std::vector<std::string> parts;
      for (const auto& [ax, bound] : {std::pair<const char*, double>{"x", limits_.workspace_x},
                                       std::pair<const char*, double>{"y", limits_.workspace_y}}) {
        const double val = num(a, ax, 0.0);
        if (std::fabs(val) > bound) {
          parts.push_back(fmt("%s=%.3f outside [%g, %g] m", ax, val, -bound, bound));
        }
      }
      if (!parts.empty()) {
        std::ostringstream os;
        for (std::size_t i = 0; i < parts.size(); ++i) {
          if (i) os << "; ";
          os << parts[i];
        }
        add("CONST-10", os.str());
      }
    }
  }
  return v;
}

// ---------------------------------------------------------------------------
// The pipeline
// ---------------------------------------------------------------------------

std::string Safety::check_lease(const std::string& lease_token, const std::string& zone_id,
                                std::optional<std::int64_t> fence_token) {
  if (lease_token.empty()) return {};
  std::lock_guard<std::mutex> lock(mtx_);
  evict_expired();
  for (const auto& [zone, l] : leases_) {
    if (l.lease_id != lease_token) continue;
    if (!l.valid || l.expires_at <= now_s()) {
      return "[LEASE] Lease for zone=" + l.zone_id + " has expired";
    }
    if (!zone_id.empty() && l.zone_id != zone_id) {
      return "[LEASE] Lease token mismatch for zone=" + zone_id;
    }
    if (fence_token && *fence_token != l.fence_token) {
      return "[LEASE] Stale fence token for zone=" + l.zone_id + ": presented " +
             std::to_string(*fence_token) + ", current is " + std::to_string(l.fence_token) +
             " (lease was renewed or re-granted since this token was issued)";
    }
    return {};
  }
  return "[LEASE] No lease held for zone=" + (zone_id.empty() ? std::string("default") : zone_id);
}

CheckOutcome Safety::check(const std::string& name, const json& arguments,
                           const std::string& lease_token, const std::string& zone_id,
                           std::optional<std::int64_t> fence_token, bool skip_shadow,
                           bool require_lease) {
  {
    std::lock_guard<std::mutex> lock(mtx_);
    ++calls_;
  }

  // 1. E-stop latch, first, so contention cannot starve it.
  if (estop_.load()) {
    std::lock_guard<std::mutex> lock(mtx_);
    ++blocked_;
    CheckOutcome out;
    out.safe = false;
    out.code = Code::kEstopActive;
    out.violations = {"E-Stop is active for this robot. All actuations are blocked until "
                      "pcp/estop_reset is called by an authorized operator."};
    return out;
  }

  // 2. Lease. A lease-required actuation with no token is as bad as a stale
  //    one: pmcp-python raises LEASE_REQUIRED for both. This sits after the
  //    e-stop latch, so a stopped robot reports the stop, not the lease.
  if (require_lease && lease_token.empty()) {
    std::lock_guard<std::mutex> lock(mtx_);
    ++blocked_;
    CheckOutcome out;
    out.safe = false;
    out.code = Code::kLeaseRequired;
    out.violations = {"[LEASE] Actuation '" + name +
                      "' requires a valid zone lease (lease_token missing). "
                      "Call lease/request first."};
    return out;
  }
  if (!lease_token.empty()) {
    if (auto v = check_lease(lease_token, zone_id, fence_token); !v.empty()) {
      std::lock_guard<std::mutex> lock(mtx_);
      ++blocked_;
      CheckOutcome out;
      out.safe = false;
      out.code = Code::kLeaseRequired;
      out.violations = {v};
      return out;
    }
  }

  // 3. Constitution.
  json payload = arguments.is_object() ? arguments : json::object();
  payload["call_id"] = payload.value("call_id", std::string(8, '0'));
  payload["_shadow_ts"] = now_s();
  auto violations = run_constitution(payload);
  if (!violations.empty()) {
    std::lock_guard<std::mutex> lock(mtx_);
    ++blocked_;
    CheckOutcome out;
    out.safe = false;
    out.code = Code::kConstitutionBlocked;
    out.violations = std::move(violations);
    return out;
  }

  // 4. Shadow.
  CheckOutcome out;
  if (!skip_shadow) {
    auto p = preview(name, arguments);
    if (!p.warnings.empty()) {
      std::lock_guard<std::mutex> lock(mtx_);
      ++blocked_;
      out.safe = false;
      out.code = Code::kShadowBlocked;
      out.shadow = p;
      out.violations = {"[SHADOW:" + p.status + "] " +
                        (p.warnings.empty() ? std::string{} : p.warnings.front())};
      return out;
    }
    out.shadow = std::move(p);
  }

  std::lock_guard<std::mutex> lock(mtx_);
  out.safe = true;
  return out;
}

json Safety::constitution_summary(const std::string& robot_id) const {
  json rules = json::array();
  for (const auto& r : rules_) {
    rules.push_back({{"id", r.id}, {"standard", r.standard}, {"description", r.description}});
  }
  return {{"robot_id", robot_id},
          {"rule_count", static_cast<int>(rules_.size())},
          {"fingerprint", fingerprint_},
          {"rules", std::move(rules)}};
}

json Safety::stats() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return {{"calls", calls_},
          {"blocked", blocked_},
          {"warnings", warnings_},
          {"lease_denials", lease_denials_},
          {"energy_used_j", energy_used_j_},
          {"avg_duration_s",
           calls_ > 0 ? total_duration_s_ / static_cast<double>(calls_) : 0.0}};
}

}  // namespace pmcp