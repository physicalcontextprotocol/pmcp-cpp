// Safety pipeline tests. The ordering assertions here are the ones worth
// having in CI: an e-stop must beat a valid lease, a constitution violation
// must beat a clean shadow, and a stale fence token must be refused even while
// the lease itself is still live.
#include "pmcp/safety.hpp"

#include <algorithm>
#include <cctype>
#include <thread>

#include "test_main.hpp"

using namespace pmcp;

namespace {

const json nowhere = json::object();
json at(double x, double y, double z) {
  return {{"x", x}, {"y", y}, {"z", z}};
}

}  // namespace

// ---------------------------------------------------------------------------
// E-stop latch
// ---------------------------------------------------------------------------

PMCP_TEST(estop_latches_and_blocks_everything) {
  Safety s;
  CHECK(!s.estop_active());

  auto before = s.check("move_to", at(0.1, 0.1, 0.1));
  CHECK(before.safe);

  s.set_estop(true);
  CHECK(s.estop_active());
  auto after = s.check("move_to", at(0.1, 0.1, 0.1));
  CHECK(!after.safe);
  CHECK_EQ(after.code, Code::kEstopActive);

  // Clearing requires an explicit call — nothing un-latches implicitly.
  s.set_estop(false);
  CHECK(!s.estop_active());
  CHECK(s.check("move_to", at(0.1, 0.1, 0.1)).safe);
}

PMCP_TEST(estop_outranks_a_valid_lease) {
  // Ordering test: with a live lease in hand the pipeline still stops at
  // step 1, so the reported code is the e-stop, not a lease or shadow problem.
  Safety s;
  auto lease = s.request_lease("r1", "cell", 5000);
  CHECK_EQ(lease.state, std::string("ACTIVE"));

  s.set_estop(true);
  auto out = s.check("move_to", at(0.1, 0.1, 0.1), lease.lease_id, "cell", lease.fence_token);
  CHECK(!out.safe);
  CHECK_EQ(out.code, Code::kEstopActive);
  CHECK(!out.violations.empty());
}

// ---------------------------------------------------------------------------
// Leases and fencing
// ---------------------------------------------------------------------------

PMCP_TEST(lease_grant_sets_fence_token_and_increments_on_renewal) {
  Safety s;
  auto a = s.request_lease("r1", "cell", 5000);
  CHECK_EQ(a.state, std::string("ACTIVE"));
  CHECK(a.valid);
  CHECK(!a.lease_id.empty());
  CHECK(a.fence_token > 0);

  // A different robot asking for the same zone is denied, not queued forever.
  auto b = s.request_lease("r2", "cell", 5000);
  CHECK_EQ(b.state, std::string("DENIED"));
  CHECK(!b.valid);
  CHECK(!b.deny_reason.empty());

  CHECK(s.release_lease(a.lease_id));
  CHECK(!s.release_lease(a.lease_id));  // double release is a no-op
}

PMCP_TEST(expired_lease_is_evicted_and_readmits_the_zone) {
  Safety s;
  auto a = s.request_lease("r1", "cell", 30);  // 30ms
  CHECK_EQ(a.state, std::string("ACTIVE"));
  CHECK_EQ(s.active_lease_count(), 1u);

  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  s.evict_expired();
  CHECK_EQ(s.active_lease_count(), 0u);

  auto b = s.request_lease("r2", "cell", 5000);
  CHECK_EQ(b.state, std::string("ACTIVE"));
  CHECK_NE(b.lease_id, a.lease_id);
  // The fence token must not go backwards, or a stale holder could re-enter.
  CHECK(b.fence_token > a.fence_token);
}

PMCP_TEST(stale_fence_token_is_refused_even_with_a_live_lease) {
  // The classic stale-holder bug: robot B now owns the zone, but robot A still
  // holds the old lease id. A must be blocked.
  Safety s;
  auto a = s.request_lease("r1", "cell", 5000);
  CHECK(s.release_lease(a.lease_id));
  auto b = s.request_lease("r2", "cell", 5000);
  CHECK_EQ(b.state, std::string("ACTIVE"));
  CHECK(b.fence_token > a.fence_token);

  // A replays its old token.
  auto out = s.check("move_to", at(0.1, 0.1, 0.1), a.lease_id, "cell", a.fence_token);
  CHECK(!out.safe);
  CHECK_EQ(out.code, Code::kLeaseRequired);

  // B proceeds.
  CHECK(s.check("move_to", at(0.1, 0.1, 0.1), b.lease_id, "cell", b.fence_token).safe);
}

PMCP_TEST(missing_lease_is_lease_required_not_a_crash) {
  Safety s;
  auto out = s.check("move_to", at(0.1, 0.1, 0.1), "lease-i-never-existed", "cell");
  CHECK(!out.safe);
  CHECK_EQ(out.code, Code::kLeaseRequired);

  auto unknown = s.find_lease("nope");
  CHECK(!unknown.has_value());
}

PMCP_TEST(lease_canonical_json_carries_both_clock_spellings) {
  Safety s;
  auto l = s.request_lease("r1", "cell", 2000);
  auto c = l.to_canonical();
  CHECK(c.contains("lease_id"));
  CHECK(c.contains("expires_at"));
  CHECK(c.contains("expires_ms"));
  CHECK_EQ(c["expires_ms"].get<std::int64_t>(),
           static_cast<std::int64_t>(c["expires_at"].get<double>() * 1000.0));
  CHECK_EQ(c["state"], json("ACTIVE"));
  // An empty deny_reason serializes as null, not "".
  CHECK(c["deny_reason"].is_null());
}

// ---------------------------------------------------------------------------
// Constitution
// ---------------------------------------------------------------------------

PMCP_TEST(constitution_blocks_motion_below_the_floor) {
  Safety s;
  auto out = s.check("move_to", at(0.1, 0.1, -0.5));
  CHECK(!out.safe);
  CHECK_EQ(out.code, Code::kConstitutionBlocked);
  bool found = false;
  for (const auto& v : out.violations) found = found || v.find("CONST-02") != std::string::npos;
  CHECK(found);
}

PMCP_TEST(constitution_blocks_an_explicit_estop_flag_in_arguments) {
  // CONST-06 is "the emergency stop bit must be clear". pcp-python evaluates
  // bool(payload.get("estop", False)), so a bit that is *set* blocks and a bit
  // that is explicitly clear does not -- carrying the key is not the violation,
  // carrying a set bit is.
  Safety s;
  auto set = s.check("move_to", {{"x", 0.1}, {"y", 0.1}, {"z", 0.1}, {"estop", true}});
  CHECK(!set.safe);
  CHECK_EQ(set.code, Code::kConstitutionBlocked);
  bool found = false;
  for (const auto& v : set.violations) found = found || v.find("CONST-06") != std::string::npos;
  CHECK(found);

  auto clear = s.check("move_to", {{"x", 0.1}, {"y", 0.1}, {"z", 0.1}, {"estop", false}});
  CHECK(clear.safe);
}

PMCP_TEST(constitution_blocks_out_of_workspace_targets) {
  Safety s;
  // Within the shadow box but outside the constitution's workspace bound.
  auto out = s.check("move_to", at(5.0, 0.0, 0.5));
  CHECK(!out.safe);
  bool found = false;
  for (const auto& v : out.violations) found = found || v.find("CONST-10") != std::string::npos;
  CHECK(found);
}

PMCP_TEST(constitution_blocks_an_oversized_step) {
  Safety s;
  // The step heuristic rejects implausible single-call jumps.
  auto out = s.check("move_to", at(1.9, 1.9, 1.1));
  CHECK(!out.safe);
}

PMCP_TEST(minimal_profile_enforces_fewer_rules) {
  Safety strict(SafetyProfile::kDefault);
  Safety minimal(SafetyProfile::kMinimal);
  const json args = at(5.0, 0.0, 0.5);
  // skip_shadow so this is a statement about the constitution alone: the
  // shadow validator is a separate pipeline stage with its own envelope and
  // does not consult SafetyProfile.
  auto s_out = strict.check("move_to", args, "", "default", std::nullopt, true);
  auto m_out = minimal.check("move_to", args, "", "default", std::nullopt, true);
  CHECK(!s_out.safe);
  bool ws = false;
  for (const auto& v : s_out.violations) ws = ws || v.find("CONST-10") != std::string::npos;
  CHECK(ws);  // the workspace rule is in the default set
  CHECK(m_out.safe);  // ...and not in the minimal set (CONST-01/02/06 only)
}

PMCP_TEST(every_profile_reports_a_fingerprint_and_rules) {
  for (auto p : {SafetyProfile::kDefault, SafetyProfile::kMinimal, SafetyProfile::kV05,
                 SafetyProfile::kArm}) {
    Safety s(p);
    CHECK(!s.fingerprint().empty());
    // pcp-python computes hashlib.sha256(rule_str).hexdigest()[:16], so the
    // fingerprint is 16 hex chars, not the full digest.
    CHECK_EQ(s.fingerprint().size(), 16u);
    auto sum = s.constitution_summary("ur5");
    CHECK(sum.contains("rules"));
    CHECK(sum["rules"].is_array());
    CHECK(sum["rules"].size() > 0);
    CHECK(sum["fingerprint"] == s.fingerprint());
  }
}

PMCP_TEST(v05_profile_reports_the_r_prefixed_rule_ids) {
  Safety s(SafetyProfile::kV05);
  auto sum = s.constitution_summary("ur5");
  bool saw_r_rule = false;
  for (const auto& r : sum["rules"]) saw_r_rule = saw_r_rule || r["id"].get<std::string>().rfind("R-", 0) == 0;
  CHECK(saw_r_rule);
}

// ---------------------------------------------------------------------------
// Shadow
// ---------------------------------------------------------------------------

PMCP_TEST(shadow_flags_out_of_bounds_targets_without_erroring) {
  Safety s;
  auto p = s.preview("move_to", at(9.0, 0.0, 0.5));
  CHECK(!p.safe);
  CHECK_EQ(p.status, std::string("UNSAFE"));
  CHECK(!p.warnings.empty());
  // A preview is advisory: it reports, it does not throw or block.
  CHECK_EQ(p.actuation_name, std::string("move_to"));
  CHECK(p.timestamp > 0.0);
}

PMCP_TEST(shadow_accepts_an_in_bounds_target) {
  Safety s;
  auto p = s.preview("move_to", at(0.2, 0.2, 0.4));
  CHECK(p.safe);
  CHECK_EQ(p.status, std::string("SAFE"));
}

PMCP_TEST(shadow_flags_speed_limit_violations) {
  Safety s;
  auto p = s.preview("move_to", {{"x", 0.2}, {"y", 0.2}, {"z", 0.4}, {"speed", 9.9}});
  CHECK(!p.safe);
  // The warning reads "Speed=9.90 m/s exceeds soft limit (...)" -- match the
  // metric rather than the capitalisation.
  bool warned = false;
  for (const auto& w : p.warnings) {
    std::string lower = w;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    warned = warned || lower.find("speed") != std::string::npos;
  }
  CHECK(warned);
}

PMCP_TEST(shadow_preview_json_has_the_fields_v05_publishes) {
  Safety s;
  auto c = s.preview("move_to", at(0.2, 0.2, 0.4)).to_canonical();
  CHECK(c.contains("actuation_name"));
  CHECK(c.contains("safe"));
  CHECK(c.contains("status"));
  CHECK(c.contains("risk_score"));
  CHECK(c.contains("duration_s"));
  CHECK(c.contains("engine"));
  CHECK(c.contains("warnings"));
}

PMCP_TEST(skip_shadow_returns_a_safe_outcome_when_nothing_else_blocks) {
  Safety s;
  auto out = s.check("open_gripper", nowhere, {}, {}, std::nullopt, /*skip_shadow=*/true);
  CHECK(out.safe);
  CHECK(!out.shadow.has_value());

  auto with = s.check("open_gripper", nowhere, {}, {}, std::nullopt, /*skip_shadow=*/false);
  CHECK(with.safe);
  CHECK(with.shadow.has_value());
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

PMCP_TEST(stats_count_calls_blocked_and_energy) {
  Safety s;
  CHECK(s.check("move_to", at(0.1, 0.1, 0.1)).safe);
  CHECK(s.check("move_to", at(0.1, 0.1, 0.2)).safe);
  CHECK(!s.check("move_to", at(0.1, 0.1, -9.0)).safe);  // blocked
  auto st = s.stats();
  CHECK_EQ(st["calls"].get<std::int64_t>(), 3);
  CHECK_EQ(st["blocked"].get<std::int64_t>(), 1);
  CHECK(st.contains("warnings"));
  CHECK(st.contains("lease_denials"));
}

// ---------------------------------------------------------------------------
// Thread safety
// ---------------------------------------------------------------------------

PMCP_TEST(estop_and_leases_are_safe_under_concurrency) {
  Safety s;
  auto l = s.request_lease("r1", "cell", 60'000);

  std::atomic<bool> go{true};
  std::atomic<int> allowed{0};
  std::atomic<int> refused{0};

  std::thread hammer([&] {
    while (go.load()) {
      auto out = s.check("move_to", at(0.1, 0.1, 0.1), l.lease_id, "cell", l.fence_token);
      if (out.safe) ++allowed; else ++refused;
    }
  });

  for (int i = 0; i < 200; ++i) s.request_lease("r2", "other", 1000);
  s.set_estop(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  go = false;
  hammer.join();

  // The hammer must have seen both verdicts — proving the latch was observed
  // mid-run — and must not have crashed or deadlocked.
  CHECK(allowed.load() > 0);
  CHECK(refused.load() > 0);
  CHECK(s.estop_active());
}
