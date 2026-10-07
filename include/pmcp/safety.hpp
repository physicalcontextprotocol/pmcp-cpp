// pmcp-cpp — safety pipeline.
//
// Ordering is fixed and matches the reference SDKs:
//
//   1. E-stop latch      (checked first so it cannot be starved by contention)
//   2. Lease fencing
//   3. Constitution      (CONST-* rules)
//   4. Shadow simulation
//   5. Execution         (caller's code, outside this class)
//
// The two Python SDKs disagree on their rule sets — pmcp-python/pcp has
// CONST-01..10, v05 has seven named rules (R-ESTOP-01, R-SPEED-01,
// R-FLOOR-01, R-WS-01, R-ENERGY-01, R-HUMAN-01, R-FORCE-01), and Rust has
// six. This class implements the union, selected by SafetyProfile, and each
// rule carries the id its own dialect expects so violation strings match.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pmcp/error.hpp"
#include "pmcp/json.hpp"

namespace pmcp {

// Which rule set to enforce.
enum class SafetyProfile {
  kDefault,    // CONST-01..10 (pmcp-python/pcp `SafetyMiddleware.default()`)
  kMinimal,    // CONST-01, CONST-02, CONST-06
  kV05,        // the seven R-* rules from pcp_safety_v5.py
  kArm,        // kDefault + ARM-01..03
};

struct SafetyLimits {
  double max_speed_m_s = 2.0;
  double floor_z_m = -0.05;
  double energy_budget_j = 50'000.0;
  double max_force_n = 500.0;
  double min_human_distance_m = 0.5;
  double workspace_x = 2.0;  // |x|, |y| bound for CONST-10
  double workspace_y = 2.0;
  // Shadow simulator bounds — deliberately tighter than the constitution, as
  // in both Python SDKs.
  double shadow_min_x = -1.0;
  double shadow_max_x = 1.0;
  double shadow_min_y = -1.0;
  double shadow_max_y = 1.0;
  double shadow_min_z = 0.0;
  double shadow_max_z = 1.2;
  double shadow_max_speed_m_s = 1.5;
};

// A zone lease with fencing. `fence_token` increments on every grant or
// renewal so a stale holder can be detected after the zone changes hands.
struct Lease {
  std::string lease_id;
  std::string robot_id;
  std::string zone_id;
  std::string state;  // FREE | PENDING | ACTIVE | EXPIRED | DENIED
  double expires_at = 0.0;   // unix seconds
  std::int64_t fence_token = 0;
  bool valid = false;
  std::string deny_reason;

  [[nodiscard]] json to_canonical() const {
    return {{"lease_id", lease_id},   {"robot_id", robot_id},
            {"zone_id", zone_id},     {"state", state},
            {"expires_at", expires_at},
            {"expires_ms", static_cast<std::int64_t>(expires_at * 1000.0)},
            {"fence_token", fence_token},
            {"valid", valid},
            {"deny_reason", deny_reason.empty() ? json() : json(deny_reason)}};
  }
};

struct ShadowPreview {
  std::string actuation_name;
  std::string status;  // SAFE | COLLISION | UNSAFE
  bool safe = true;
  double risk_score = 0.0;
  double duration_s = 0.0;
  double energy_j = 0.0;
  std::vector<std::string> warnings;
  std::string engine = "geometric";
  std::string collision_body;
  std::string robot_id;
  std::int64_t shadow_token_len = 16;
  double timestamp = 0.0;

  [[nodiscard]] json to_canonical() const;
};

struct CheckOutcome {
  bool safe = false;
  std::optional<ShadowPreview> shadow;
  std::vector<std::string> violations;
  Code code = Code::kConstitutionBlocked;
};

class Safety {
 public:
  explicit Safety(SafetyProfile profile = SafetyProfile::kDefault,
                  SafetyLimits limits = {});

  // ---- e-stop latch -----------------------------------------------------
  void set_estop(bool active);
  [[nodiscard]] bool estop_active() const noexcept { return estop_.load(); }

  // ---- leases -----------------------------------------------------------
  Lease request_lease(const std::string& robot_id, const std::string& zone_id,
                      std::int64_t duration_ms, double bid_energy_j = 100.0);
  bool release_lease(const std::string& lease_id);
  [[nodiscard]] std::optional<Lease> find_lease(const std::string& lease_id) const;
  // Drops expired grants. The reference servers call this before every lease
  // lookup and inside metrics, which is why an expired lease surfaces as
  // LEASE_REQUIRED rather than LEASE_EXPIRED.
  void evict_expired();
  [[nodiscard]] std::size_t active_lease_count() const;

  // ---- shadow -----------------------------------------------------------
  [[nodiscard]] ShadowPreview preview(const std::string& name,
                                      const json& arguments = json::object()) const;

  // ---- the pipeline -----------------------------------------------------
  //
  // `lease_token` empty skips step 2. `require_lease` comes from the
  // actuation spec. `skip_shadow` is set for actuations whose spec says
  // shadow_required = false.
  [[nodiscard]] CheckOutcome check(const std::string& name, const json& arguments,
                                   const std::string& lease_token = {},
                                   const std::string& zone_id = {},
                                   std::optional<std::int64_t> fence_token = std::nullopt,
                                   bool skip_shadow = false,
                                   bool require_lease = false);

  // Human-readable summary of the active rule set, for pcp/constitution.
  [[nodiscard]] json constitution_summary(const std::string& robot_id) const;
  [[nodiscard]] const std::string& fingerprint() const noexcept { return fingerprint_; }

  // ---- counters ---------------------------------------------------------
  [[nodiscard]] json stats() const;

  const SafetyLimits& limits() const noexcept { return limits_; }
  void set_limits(SafetyLimits l) { limits_ = l; }

 private:
  struct Rule {
    std::string id;
    std::string standard;
    std::string description;
  };

  [[nodiscard]] std::vector<std::string> run_constitution(const json& args) const;
  [[nodiscard]] std::string check_lease(const std::string& lease_token,
                                        const std::string& zone_id,
                                        std::optional<std::int64_t> fence_token);

  SafetyProfile profile_;
  SafetyLimits limits_;
  std::vector<Rule> rules_;
  std::string fingerprint_;

  std::atomic<bool> estop_{false};

  mutable std::mutex mtx_;
  std::map<std::string, Lease> leases_;
  std::map<std::string, std::int64_t> fence_counters_;
  std::int64_t calls_ = 0;
  std::int64_t blocked_ = 0;
  std::int64_t warnings_ = 0;
  std::int64_t lease_denials_ = 0;
  double energy_used_j_ = 0.0;
  double total_duration_s_ = 0.0;
};

}  // namespace pmcp