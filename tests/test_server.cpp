// Server-level tests: envelope handling, JSON-RPC 2.0 invariants the reference
// SDKs rely on, and the fact that ONE server answers to all four dialects.
#include "pmcp/server.hpp"
#include "test_main.hpp"

using namespace pmcp;

namespace {

// Split newline-delimited output into lines, dropping the trailing empty one.
std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      out.push_back(text.substr(start));
      break;
    }
    if (nl > start) out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

// Server owns a mutex and is deliberately neither copyable nor movable, so the
// fixture fills an instance in place rather than returning one.
void fill_server(Server& s) {

  ActuationSpec move;
  move.name = "move_to";
  move.description = "Move TCP to XYZ";
  move.est_duration_s = 2.0;
  move.parameters = {
      {"x", "number", "x target"},
      {"y", "number", "y target"},
      {"z", "number", "z target"},
  };
  move.parameters[2].required = false;
  move.parameters[2].default_value = json(0.0);
  s.register_actuation(move, [](const json& a) {
    ActuationOutcome r;
    r.output = {{"x", a.value("x", 0.0)}, {"y", a.value("y", 0.0)}, {"z", a.value("z", 0.0)}};
    r.energy_j = 12.5;
    r.final_pose = {{"x", a.value("x", 0.0)}, {"y", a.value("y", 0.0)},
                    {"z", a.value("z", 0.0)}};
    return r;
  });

  ActuationSpec grip;
  grip.name = "open_gripper";
  grip.description = "Open the gripper";
  grip.shadow_required = false;  // no workspace math to check
  s.register_actuation(grip, [](const json&) {
    ActuationOutcome r;
    r.output = {{"open", true}};
    r.energy_j = 2.5;
    return r;
  });

  SensorSpec temp;
  temp.name = "temperature";
  temp.description = "Motor temperature";
  temp.unit = "C";
  s.register_sensor(temp, []() { return json(41.5); });
}


// The fixture's identity matters: several assertions read robot_id out of
// pcp/status and build pcp://<robot>/... URIs, so give every test the same
// deterministic one instead of relying on ServerConfig's default.
ServerConfig test_cfg() {
  ServerConfig cfg;
  cfg.name = "test-arm";
  cfg.version = "2.1.0";
  cfg.robot_id = "ur5";
  return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// JSON-RPC invariants
// ---------------------------------------------------------------------------

PMCP_TEST(every_response_is_jsonrpc_2_0) {
  Server s(test_cfg());
  fill_server(s);
  for (auto* m : {"initialize", "ping", "actuations/list", "tools/list", "sensors/list",
                  "resources/list", "pcp/ping"}) {
    auto r = s.call(m);
    CHECK_EQ(r.value("jsonrpc", std::string{}), std::string("2.0"));
  }
}

PMCP_TEST(id_is_echoed_verbatim_without_type_coercion) {
  Server s(test_cfg());
  fill_server(s);
  for (json id : {json(1), json(42), json("abc"), json(3.5)}) {
    auto r = s.call("ping", json::object(), id);
    CHECK_EQ(r["id"], id);
  }
}

PMCP_TEST(result_and_error_are_mutually_exclusive) {
  Server s(test_cfg());
  fill_server(s);
  auto ok = s.call("ping");
  CHECK(ok.contains("result"));
  CHECK(!ok.contains("error"));
  auto bad = s.call("nonexistent_method");
  CHECK(bad.contains("error"));
  CHECK(!bad.contains("result"));
}

PMCP_TEST(unknown_method_is_32601) {
  Server s(test_cfg());
  fill_server(s);
  auto r = s.call("does_not_exist");
  CHECK_EQ(r["error"]["code"].get<int>(), -32601);
}

PMCP_TEST(incoming_jsonrpc_version_is_never_validated) {
  // Both Python servers ignore the request's jsonrpc member. So do we.
  Server s(test_cfg());
  fill_server(s);
  auto r = s.handle_message(
               {{"jsonrpc", "1.0"}, {"id", 1}, {"method", "ping"}, {"params", json::object()}});
  CHECK(r.has_value());
  CHECK_EQ((*r)["jsonrpc"], json("2.0"));
  CHECK(!(*r).contains("error"));
}

PMCP_TEST(notifications_get_no_reply) {
  Server s(test_cfg());
  fill_server(s);
  auto r = s.handle_message(
      {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}, {"params", json::object()}});
  CHECK(!r.has_value());
}

// ---------------------------------------------------------------------------
// One server, four dialects
// ---------------------------------------------------------------------------

PMCP_TEST(the_same_server_answers_all_four_dialects) {
  Server s(test_cfg());
  fill_server(s);

  // Discovery: three spellings, one catalog.
  auto py = s.call("actuations/list")["result"]["actuations"];
  auto v05 = s.call("tools/list")["result"]["tools"];
  auto conf = s.call("actuations/list")["result"]["actuations"];
  CHECK(py.size() == 2);
  CHECK_EQ(v05.size(), 2u);
  CHECK_EQ(conf.size(), 2u);
  CHECK_EQ(v05[0]["name"], py[0]["name"]);

  // Execution: one call per dialect spelling, same handler underneath.
  // move_to requires a lease, so acquire one first and present it in the
  // spelling each dialect uses for the token.
  auto lease = s.call("lease/request",
                      {{"robot_id", "ur5"}, {"zone_id", "cell"}, {"duration_ms", 5000}});
  CHECK(!lease.contains("error"));
  const json lease_token = lease["result"]["lease"]["lease_id"];
  const json fence_token = lease["result"]["lease"]["fence_token"];

  auto r_py = s.call("actuations/call",
                     {{"name", "move_to"},
                      {"arguments", {{"x", 0.1}, {"y", 0.2}, {"z", 0.3}}},
                      {"zone_id", "cell"},
                      {"lease_token", lease_token},
                      {"fence_token", fence_token}});
  auto r_v05 = s.call("tools/call",
                      {{"name", "move_to"},
                       {"arguments", {{"x", 0.1}, {"y", 0.2}, {"z", 0.3}}},
                       {"zone_id", "cell"},
                       {"lease_token", lease_token},
                       {"fence_token", fence_token}});
  auto r_conf = s.call("actuations/execute",
                       {{"name", "move_to"},
                        {"params", {{"x", 0.1}, {"y", 0.2}, {"z", 0.3}}},
                        {"zone_id", "cell"},
                        {"lease_token", lease_token},
                        {"fence_token", fence_token}});
  CHECK(!r_py.contains("error"));
  CHECK(!r_v05.contains("error"));
  CHECK(!r_conf.contains("error"));

  // And the three result shapes all describe the same actuation.
  CHECK_EQ(r_py["result"]["content"][0]["data"]["success"], json(true));
  CHECK_EQ(r_v05["result"]["isError"], json(false));
  CHECK_EQ(r_conf["result"]["success"], json(true));
  CHECK_EQ(r_conf["result"]["energy_consumed_j"], json(12.5));
}

PMCP_TEST(lease_lifecycle_is_identical_across_dialects) {
  Server s(test_cfg());
  fill_server(s);

  auto py = s.call("lease/request", {{"robot_id", "ur5"}, {"zone_id", "cell"},
                                     {"duration_ms", 5000}});
  auto v05 = s.call("lease/request", {{"robotId", "ur5"}, {"zoneId", "cell"},
                                      {"durationMs", 5000}, {"bidEnergyJ", 100.0}});
  auto conf = s.call("leases/acquire", {{"zone_id", "cell2"}, {"duration_ms", 5000}});

  CHECK_EQ(py["result"]["lease"]["state"], json("ACTIVE"));
  CHECK_EQ(v05["result"]["lease"]["state"], json("ACTIVE"));
  CHECK_EQ(conf["result"]["granted"], json(true));

  const std::string lid = py["result"]["lease"]["lease_id"];
  CHECK(!lid.empty());

  // Release in each dialect. lease_id is echoed in every dialect because the
  // `lease/release` method name is identical in the spec and in pmcp-python,
  // so the dialect cannot be reliably inferred from the method alone.
  auto rel_py = s.call("lease/release", {{"lease_id", lid}});
  CHECK_EQ(rel_py["result"]["released"], json(true));
  CHECK(rel_py["result"].contains("lease_id"));

  auto rel_conf = s.call("leases/release", {{"lease_id", "nonexistent"}});
  CHECK_EQ(rel_conf["result"]["released"], json(false));
  // Releasing an unknown lease is a result, never an error.
  CHECK(!rel_conf.contains("error"));
}

PMCP_TEST(actuation_without_a_lease_is_33003) {
  // requires_lease defaults to true; move_to and open_gripper both carry it.
  Server s(test_cfg());
  fill_server(s);
  auto r = s.call("actuations/call", {{"name", "open_gripper"}, {"arguments", json::object()}});
  CHECK_EQ(r["error"]["code"].get<int>(), -33003);
  CHECK(r["error"]["message"].get<std::string>().find("lease/request") != std::string::npos);
}

PMCP_TEST(estop_latches_across_all_dialects) {
  Server s(test_cfg());
  fill_server(s);

  // Engage via the conformance dialect's method name...
  auto eng = s.call("safety/estop/engage");
  CHECK_EQ(eng["result"]["engaged"], json(true));
  CHECK(s.safety().estop_active());

  // ...and every actuation spelling is now blocked.
  for (auto* m : {"actuations/call", "tools/call", "actuations/execute"}) {
    auto r = s.call(m, {{"name", "open_gripper"}, {"arguments", json::object()}});
    CHECK(r.contains("error"));
    CHECK(r["error"]["message"].get<std::string>().find("E-Stop") != std::string::npos);
  }

  // Non-motion methods still work while stopped, as in the reference servers.
  CHECK(!s.call("ping").contains("error"));
  CHECK(!s.call("shadow/preview", {{"name", "open_gripper"}}).contains("error"));
  CHECK(!s.call("lease/request", {{"zone_id", "z"}}).contains("error"));

  // Disengage via v05's bool param.
  auto dis = s.call("pcp/estop", {{"active", false}});
  CHECK_EQ(dis["result"]["estop"], json(false));
  CHECK(!s.safety().estop_active());
  // Actuation paths work again -- with a lease, since open_gripper requires
  // one (LEASE_REQUIRED now that the e-stop latch is clear).
  auto l = s.call("lease/request",
                  {{"robot_id", "ur5"}, {"zone_id", "z"}, {"duration_ms", 5000}});
  CHECK(!l.contains("error"));
  CHECK(!s.call("tools/call",
                {{"name", "open_gripper"},
                 {"arguments", json::object()},
                 {"lease_token", l["result"]["lease"]["lease_id"]},
                 {"zone_id", "z"}})
             .contains("error"));
}

PMCP_TEST(spec_spelled_estop_is_accepted_too) {
  // PROTOCOL_SPEC.md says pmcp/estop; all three SDKs say pcp/estop. Accept both.
  Server s(test_cfg());
  fill_server(s);
  CHECK(!s.call("pmcp/estop").contains("error"));
  CHECK(s.safety().estop_active());
  CHECK(!s.call("pmcp/estop_reset").contains("error"));
  CHECK(!s.safety().estop_active());
}

// ---------------------------------------------------------------------------
// Handler-level behaviour
// ---------------------------------------------------------------------------

PMCP_TEST(unknown_actuation_is_32601_and_lists_what_exists) {
  Server s(test_cfg());
  fill_server(s);
  auto r = s.call("actuations/call", {{"name", "fly_to_moon"}});
  CHECK_EQ(r["error"]["code"].get<int>(), -32601);
  CHECK(r["error"]["message"].get<std::string>().find("move_to") != std::string::npos);
}

PMCP_TEST(actuator_catalog_carries_both_physical_and_annotations) {
  Server s(test_cfg());
  fill_server(s);
  auto tools = s.call("tools/list")["result"]["tools"];
  auto move = tools[0];
  CHECK(move["annotations"].contains("max_speed_m_s"));
  CHECK(move["annotations"].contains("shadow_required"));
  CHECK_EQ(move["annotations"]["protocol"], json("pcp/0.5"));
  auto acts = s.call("actuations/list")["result"]["actuations"];
  CHECK(acts[0]["physical"].contains("max_speed"));
  CHECK(acts[0]["physical"].contains("requires_lease"));
}

PMCP_TEST(sensor_read_resolves_both_name_and_uri) {
  Server s(test_cfg());
  fill_server(s);
  auto by_name = s.call("sensors/read", {{"name", "temperature"}});
  auto by_uri = s.call("resources/read", {{"uri", "pcp://ur5/sensors/temperature"}});
  CHECK(!by_name.contains("error"));
  CHECK(!by_uri.contains("error"));
  CHECK_EQ(by_name["result"]["value"], json(41.5));
  CHECK_EQ(by_uri["result"]["contents"][0]["text"].is_string(), true);

  auto conf = s.call("sensors/read", {{"name", "temperature"}});
  CHECK_EQ(conf["result"]["value"], json(41.5));
  CHECK(conf["result"].contains("timestamp_ms"));
  CHECK(conf["result"].contains("quality"));
}

PMCP_TEST(shadow_preview_flags_out_of_workspace_without_erroring) {
  Server s(test_cfg());
  fill_server(s);
  // shadow/preview returns a result with safe:false, not an error.
  auto bad = s.call("shadow/preview", {{"name", "move_to"}, {"arguments", {{"x", 9.0}}}});
  CHECK(!bad.contains("error"));
  CHECK_EQ(bad["result"]["preview"]["safe"], json(false));
  auto good = s.call("shadow/preview", {{"name", "move_to"}, {"arguments", {{"x", 0.1}}}});
  CHECK_EQ(good["result"]["preview"]["safe"], json(true));
  // v05 gets the extra schema fields, with four deliberate nulls.
  auto pv = s.call("shadow/preview", {{"actuation_name", "move_to"}})["result"]["preview"];
  CHECK(pv.contains("verdict"));
  CHECK_EQ(pv["predicted_trajectory"], json(nullptr));
  CHECK_EQ(pv["confidence"], json(nullptr));
  CHECK_EQ(pv["monitoring"], json(nullptr));
  CHECK_EQ(pv["determinism"], json(nullptr));
}

PMCP_TEST(metrics_and_audit_are_available) {
  Server s(test_cfg());
  fill_server(s);
  s.call("tools/call", {{"name", "open_gripper"}});
  auto m_py = s.call("metrics/get")["result"]["metrics"];
  CHECK(m_py.contains("calls_total"));
  auto m_conf = s.call("pcp/metrics")["result"];
  CHECK(m_conf.contains("actuationCount"));
  CHECK(m_conf.contains("safetyViolations"));
  auto audit = s.call("audit/list", {{"limit", 10}})["result"];
  CHECK(audit.contains("entries"));
  CHECK(audit.contains("total"));
  CHECK(audit["total"].get<int>() >= 1);
}

PMCP_TEST(pcp_status_identity_and_constitution_are_v05_only) {
  Server s(test_cfg());
  fill_server(s);
  auto st = s.call("pcp/status")["result"];
  CHECK_EQ(st["robot_id"], json("ur5"));
  CHECK_EQ(st["pcp_version"], json("0.5"));
  CHECK(st.contains("safety_stats"));
  auto id = s.call("pcp/identity")["result"];
  // RobotIdentity.to_dict uses `class` and `firmware`, not the field names.
  CHECK(id.contains("class"));
  CHECK(id.contains("firmware"));
  CHECK(id["did"].get<std::string>().rfind("did:pcp:", 0) == 0);
  auto c = s.call("pcp/constitution")["result"];
  CHECK(c.contains("fingerprint"));
  CHECK(c["rule_count"].get<int>() > 0);
  // logging/setLevel returns the empty object on every dialect that has it.
  CHECK_EQ(s.call("logging/setLevel", {{"level", "debug"}})["result"], json::object());
}

PMCP_TEST(batch_execution_matches_the_conformance_shape) {
  Server s(test_cfg());
  fill_server(s);
  auto r = s.call("actuations/batch",
                  {{"actuations",
                    json::array({{{"name", "open_gripper"}, {"params", json::object()}},
                                 {{"name", "move_to"}, {"params", {{"x", 0.0}, {"y", 0.0},
                                                                     {"z", 0.2}}}}})},
                   {"atomic", true}});
  CHECK(!r.contains("error"));
  CHECK_EQ(r["result"]["results"].size(), 2u);
  CHECK(r["result"].contains("total_duration_ms"));
  CHECK_EQ(r["result"]["success"], json(true));
}

PMCP_TEST(batch_rejects_an_empty_list) {
  Server s(test_cfg());
  fill_server(s);
  auto r = s.call("actuations/batch", {{"actuations", json::array()}});
  CHECK(r.contains("error"));
  CHECK_EQ(r["error"]["code"].get<int>(), -32602);
}

PMCP_TEST(prompt_and_get_prompt) {
  Server s(test_cfg());
  fill_server(s);
  PromptSpec p;
  p.name = "pick_and_place";
  p.description = "Pick from A, place at B";
  p.arguments = {{"source", "string", "source"}, {"destination", "string", "destination"}};
  s.register_prompt(p, [](const json&) { return json("picked"); });

  auto list = s.call("prompts/list")["result"]["prompts"];
  CHECK_EQ(list.size(), 1u);
  CHECK_EQ(list[0]["arguments"][0]["required"], json(true));
  auto got = s.call("prompts/get", {{"name", "pick_and_place"}})["result"];
  CHECK(got.contains("description"));
  CHECK(got.contains("messages"));
  CHECK(s.call("prompts/get", {{"name", "nope"}}).contains("error"));
}

// ---------------------------------------------------------------------------
// Transport framing
// ---------------------------------------------------------------------------

PMCP_TEST(stdio_framing_is_newline_delimited_json) {
  Server s(test_cfg());
  fill_server(s);
  std::istringstream in(
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n"
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n");
  std::ostringstream out;
  s.serve_stdio(&in, &out);

  // Two requests, two responses: the notification produces no line.
  std::vector<std::string> lines;
  for (auto& line : split_lines(out.str())) lines.push_back(line);
  CHECK_EQ(lines.size(), 2u);
  CHECK_EQ(json::parse(lines[0])["id"], json(1));
  CHECK_EQ(json::parse(lines[1])["id"], json(2));
  // Output must be single-line JSON — no embedded newlines.
  CHECK(lines[0].find('\n') == std::string::npos);
}

PMCP_TEST(unparseable_stdio_line_yields_32700_and_does_not_stop_the_loop) {
  Server s(test_cfg());
  fill_server(s);
  std::istringstream in(
      "}}broken{{\n"
      "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}\n");
  std::ostringstream out;
  s.serve_stdio(&in, &out);

  std::vector<std::string> lines;
  for (auto& line : split_lines(out.str())) lines.push_back(line);
  CHECK_EQ(lines.size(), 2u);
  CHECK_EQ(json::parse(lines[0])["error"]["code"].get<int>(), -32700);
  CHECK_EQ(json::parse(lines[0])["id"], json(""));
  CHECK_EQ(json::parse(lines[1])["id"], json(7));
}

PMCP_TEST(http_parse_failure_is_400_while_jsonrpc_errors_stay_200) {
  Server s(test_cfg());
  fill_server(s);

  auto bad = s.handle_http_request("/mcp", "}}not json{{");
  CHECK_EQ(bad.status, 400);
  CHECK_EQ(json::parse(bad.body)["error"]["code"].get<int>(), -32700);
  CHECK_EQ(json::parse(bad.body)["id"], json(nullptr));
  // pmcp-python omits CORS on the 400 path; match it.
  CHECK_EQ(bad.allow_origin, false);

  // A JSON-RPC-level error is still HTTP 200.
  auto nf = s.handle_http_request("/mcp", R"({"jsonrpc":"2.0","id":1,"method":"nope"})");
  CHECK_EQ(nf.status, 200);
  CHECK_EQ(json::parse(nf.body)["error"]["code"].get<int>(), -32601);
}

PMCP_TEST(http_notification_writes_an_empty_object) {
  Server s(test_cfg());
  fill_server(s);
  auto r = s.handle_http_request("/mcp",
                                 R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  CHECK_EQ(r.status, 200);
  CHECK_EQ(json::parse(r.body), json::object());
}

PMCP_TEST(server_advertises_every_alias_it_answers_to) {
  Server s(test_cfg());
  fill_server(s);
  auto served = s.served_methods();
  for (auto* m : {"actuations/call", "tools/call", "actuations/execute", "pmcp/estop",
                  "pcp/estop", "safety/estop/engage", "lease/request", "leases/acquire"}) {
    bool found = false;
    for (const auto& x : served) found = found || x == m;
    CHECK(found);
  }
}
