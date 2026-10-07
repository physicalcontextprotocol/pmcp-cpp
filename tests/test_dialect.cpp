// Tests for the dialect compatibility layer. These are the tests that would
// have caught the original fragmentation: every alias must resolve, and every
// dialect must get the shape its own client reads.
#include "pmcp/dialect.hpp"

#include "test_main.hpp"

using namespace pmcp;

// ---------------------------------------------------------------------------
// Method resolution
// ---------------------------------------------------------------------------

PMCP_TEST(resolve_accepts_every_dialect_alias) {
  // The four spellings of the actuation call all resolve to one op.
  for (auto* m : {"actuations/call", "tools/call", "actuations/execute"}) {
    auto op = resolve_op(m);
    CHECK(op.has_value());
    CHECK_EQ(static_cast<int>(*op), static_cast<int>(Op::kCallActuation));
  }
  // ...and the three spellings of e-stop.
  for (auto* m : {"pmcp/estop", "pcp/estop", "safety/estop/engage"}) {
    CHECK(resolve_op(m).value_or(Op::kPing) == Op::kEstop);
  }
  for (auto* m : {"pmcp/estop_reset", "pcp/estop_reset", "safety/estop/disengage"}) {
    CHECK(resolve_op(m).value_or(Op::kPing) == Op::kEstopReset);
  }
  for (auto* m : {"lease/request", "leases/acquire"}) {
    CHECK(resolve_op(m).value_or(Op::kPing) == Op::kLeaseRequest);
  }
  for (auto* m : {"lease/release", "leases/release"}) {
    CHECK(resolve_op(m).value_or(Op::kPing) == Op::kLeaseRelease);
  }
  for (auto* m : {"sensors/read", "resources/read"}) {
    CHECK(resolve_op(m).value_or(Op::kPing) == Op::kReadSensor);
  }
  for (auto* m : {"sensors/list", "resources/list"}) {
    CHECK(resolve_op(m).value_or(Op::kPing) == Op::kListSensors);
  }
  for (auto* m : {"actuations/list", "tools/list"}) {
    CHECK(resolve_op(m).value_or(Op::kPing) == Op::kListActuations);
  }
  for (auto* m : {"ping", "pcp/ping"}) {
    CHECK(resolve_op(m).value_or(Op::kEstop) == Op::kPing);
  }
  for (auto* m : {"metrics/get", "pcp/metrics"}) {
    CHECK(resolve_op(m).value_or(Op::kPing) == Op::kMetrics);
  }
}

PMCP_TEST(unknown_method_does_not_resolve) {
  CHECK(!resolve_op("does_not_exist").has_value());
  CHECK(!resolve_op("").has_value());
  CHECK(!resolve_op("tools/calll").has_value());  // near miss, still unknown
}

PMCP_TEST(every_op_has_at_least_one_alias) {
  for (Op op : {Op::kInitialize, Op::kPing, Op::kListActuations, Op::kCallActuation,
                Op::kBatchActuation, Op::kListSensors, Op::kReadSensor, Op::kListPrompts,
                Op::kGetPrompt, Op::kShadowPreview, Op::kLeaseRequest, Op::kLeaseRelease,
                Op::kEstop, Op::kEstopReset, Op::kMetrics, Op::kAuditList, Op::kStatus,
                Op::kIdentity, Op::kConstitution, Op::kSetLogLevel}) {
    CHECK(!aliases_for(op).empty());
  }
  // v05 genuinely has no batch method; that gap is real and intentional.
  CHECK(method_name(Op::kBatchActuation, Dialect::kV05).empty());
  CHECK(!method_name(Op::kBatchActuation, Dialect::kPython).empty());
}

PMCP_TEST(method_name_is_the_canonical_spelling_per_dialect) {
  CHECK_EQ(std::string(method_name(Op::kCallActuation, Dialect::kPython)), "actuations/call");
  CHECK_EQ(std::string(method_name(Op::kCallActuation, Dialect::kV05)), "tools/call");
  CHECK_EQ(std::string(method_name(Op::kCallActuation, Dialect::kConformance)), "actuations/execute");
  // The spec is the outlier that the SDKs disagree with; that is the bug.
  CHECK_EQ(std::string(method_name(Op::kEstop, Dialect::kSpec)), "pmcp/estop");
  CHECK_EQ(std::string(method_name(Op::kEstop, Dialect::kPython)), "pcp/estop");
}

// ---------------------------------------------------------------------------
// Parameter normalization
// ---------------------------------------------------------------------------

PMCP_TEST(lease_params_normalize_across_snake_and_camel_case) {
  // pmcp-python/pcp sends snake_case.
  auto snake = canonical_params(Op::kLeaseRequest, "lease/request",
                                {{"robot_id", "r1"}, {"zone_id", "z"}, {"duration_ms", 5000},
                                 {"bid_energy_j", 42.0}});
  // v05 sends camelCase.
  auto camel = canonical_params(Op::kLeaseRequest, "lease/request",
                                {{"robotId", "r1"}, {"zoneId", "z"}, {"durationMs", 5000},
                                 {"bidEnergyJ", 42.0}});
  // The conformance dialect uses snake_case too.
  auto conf = canonical_params(Op::kLeaseRequest, "leases/acquire",
                               {{"zone_id", "z"}, {"duration_ms", 5000}});

  CHECK_EQ(snake["robot_id"], json("r1"));
  CHECK_EQ(snake["zone_id"], json("z"));
  CHECK_EQ(snake["duration_ms"], json(5000));
  CHECK_EQ(snake["bid_energy_j"], json(42.0));

  CHECK_EQ(camel["robot_id"], json("r1"));
  CHECK_EQ(camel["zone_id"], json("z"));
  CHECK_EQ(camel["duration_ms"], json(5000));
  CHECK_EQ(camel["bid_energy_j"], json(42.0));

  CHECK_EQ(conf["zone_id"], json("z"));
  CHECK_EQ(conf["duration_ms"], json(5000));

  // The whole point: the same canonical params from three dialects.
  CHECK_EQ(snake["zone_id"], camel["zone_id"]);
  CHECK_EQ(snake["duration_ms"], camel["duration_ms"]);
  CHECK_EQ(snake["duration_ms"], conf["duration_ms"]);
}

PMCP_TEST(actuation_argument_bag_is_named_three_different_ways) {
  // pmcp-python and v05: `arguments`. conformance: `params`.
  auto a = canonical_params(Op::kCallActuation, "actuations/call",
                            {{"name", "move_to"}, {"arguments", {{"x", 1.0}}}});
  auto b = canonical_params(Op::kCallActuation, "tools/call",
                            {{"name", "move_to"}, {"arguments", {{"x", 1.0}}}});
  auto c = canonical_params(Op::kCallActuation, "actuations/execute",
                            {{"name", "move_to"}, {"params", {{"x", 1.0}}}});
  CHECK_EQ(a["name"], json("move_to"));
  CHECK_EQ(a["arguments"]["x"], json(1.0));
  CHECK_EQ(b["arguments"]["x"], json(1.0));
  CHECK_EQ(c["arguments"]["x"], json(1.0));
}

PMCP_TEST(v05_underscore_extensions_normalize_to_bare_names) {
  auto p = canonical_params(Op::kCallActuation, "tools/call",
                            {{"name", "grip"},
                             {"_lease_token", "lt-1"},
                             {"_zone_id", "cell"},
                             {"_fence_token", 7}});
  CHECK_EQ(p["lease_token"], json("lt-1"));
  CHECK_EQ(p["zone_id"], json("cell"));
  CHECK_EQ(p["fence_token"], json(7));
}

PMCP_TEST(sensor_uri_resolves_to_a_bare_name) {
  // v05 sends a full URI; both servers fall back to the last path segment.
  auto p = canonical_params(Op::kReadSensor, "resources/read",
                            {{"uri", "pcp://ur5-arm/sensors/joint_angles"}});
  CHECK_EQ(p["name"], json("joint_angles"));
  // A bare name is also accepted.
  auto q = canonical_params(Op::kReadSensor, "resources/read", {{"uri", "temp"}});
  CHECK_EQ(q["name"], json("temp"));
  // And `name` wins when both are present.
  auto r = canonical_params(Op::kReadSensor, "resources/read",
                            {{"uri", "pcp://x/sensors/a"}, {"name", "b"}});
  CHECK_EQ(r["name"], json("b"));
}

PMCP_TEST(estop_intent_comes_from_three_different_places) {
  // v05 encodes intent in a param.
  CHECK_EQ(canonical_params(Op::kEstop, "pcp/estop", {{"active", false}})["active"], json(false));
  CHECK_EQ(canonical_params(Op::kEstop, "pcp/estop", {})["active"], json(true));
  // The conformance dialect encodes it in the method name.
  CHECK_EQ(canonical_params(Op::kEstop, "safety/estop/engage", {})["active"], json(true));
  CHECK_EQ(canonical_params(Op::kEstop, "safety/estop/disengage", {})["active"], json(false));
  // pmcp-python always engages.
  CHECK_EQ(canonical_params(Op::kEstop, "pcp/estop", {{"robot_id", "r"}})["active"], json(true));
}

PMCP_TEST(shadow_preview_accepts_actuation_name_or_name) {
  auto a = canonical_params(Op::kShadowPreview, "shadow/preview", {{"actuation_name", "feed"}});
  CHECK_EQ(a["name"], json("feed"));
  auto b = canonical_params(Op::kShadowPreview, "shadow/preview", {{"name", "grip"}});
  CHECK_EQ(b["name"], json("grip"));
}

PMCP_TEST(unmodelled_keys_are_preserved_not_dropped) {
  auto p = canonical_params(Op::kCallActuation, "actuations/call",
                            {{"name", "x"}, {"something_new_v2", 42}});
  CHECK(p.contains("_extra"));
  CHECK_EQ(p["_extra"]["something_new_v2"], json(42));
}

PMCP_TEST(non_object_params_do_not_crash) {
  for (auto* junk : {"[]", "5", "\"x\"", "null"}) {
    auto p = canonical_params(Op::kCallActuation, "actuations/call", json::parse(junk));
    CHECK(p.is_object());
  }
}

// ---------------------------------------------------------------------------
// Result serialization
// ---------------------------------------------------------------------------

PMCP_TEST(initialize_protocol_version_is_the_one_real_conflict) {
  json canon{{"server_info", {{"name", "s"}, {"version", "1"}, {"robot_id", "r"}}}};
  // v05 answers with the MCP spec revision...
  CHECK_EQ(wire_result(Op::kInitialize, Dialect::kV05, canon)["protocolVersion"],
           json("2024-11-05"));
  // ...everyone else, including conformance, asserts "0.5".
  for (Dialect d : {Dialect::kSpec, Dialect::kPython, Dialect::kConformance}) {
    CHECK_EQ(wire_result(Op::kInitialize, d, canon)["protocolVersion"], json("0.5"));
  }
}

PMCP_TEST(initialize_satisfies_conformance_server_info_assertion) {
  json canon{{"server_info", {{"name", "arm"}, {"version", "1.0.0"}, {"robot_id", "ur5"}}}};
  auto out = wire_result(Op::kInitialize, Dialect::kConformance, canon);
  // test_01 asserts both of these.
  CHECK(out["serverInfo"].contains("name"));
  CHECK(out["serverInfo"].contains("robotId"));
  CHECK_EQ(out["serverInfo"]["robotId"], json("ur5"));
  // and capabilities must have an `actuations` or `features` key.
  CHECK(out.contains("capabilities"));
}

PMCP_TEST(actuation_result_has_three_structurally_different_shapes) {
  json canon{{"success", true},     {"actuation_name", "grip"},
             {"robot_id", "ur5"},   {"output", {{"closed", true}}},
             {"duration_s", 0.5},    {"energy_j", 2.5},
             {"final_pose", nullptr}};

  auto py = wire_result(Op::kCallActuation, Dialect::kPython, canon);
  // pmcp-python wraps in a content array typed "actuation".
  CHECK_EQ(py["content"][0]["type"], json("actuation"));
  CHECK_EQ(py["content"][0]["data"]["success"], json(true));
  CHECK_EQ(py["isError"], json(false));

  auto v05 = wire_result(Op::kCallActuation, Dialect::kV05, canon);
  // v05 uses a "text" block whose payload is a double-encoded JSON string.
  CHECK_EQ(v05["content"][0]["type"], json("text"));
  CHECK(v05["content"][0]["text"].is_string());
  auto inner = json::parse(v05["content"][0]["text"].get<std::string>());
  CHECK_EQ(inner["success"], json(true));
  // v05 names the actuation field "actuation", not "actuation_name".
  CHECK_EQ(inner["actuation"], json("grip"));
  CHECK_EQ(inner["metrics"]["energy_consumed_j"], json(2.5));

  auto conf = wire_result(Op::kCallActuation, Dialect::kConformance, canon);
  // conformance reads a flat object.
  CHECK_EQ(conf["success"], json(true));
  CHECK(conf.contains("energy_consumed_j"));
  CHECK(conf.contains("duration_ms"));
  CHECK_EQ(conf["duration_ms"], json(500.0));
}

PMCP_TEST(failed_actuation_sets_is_error_on_both_content_dialects) {
  json canon{{"success", false}, {"error", "gripper jammed"}};
  CHECK_EQ(wire_result(Op::kCallActuation, Dialect::kV05, canon)["isError"], json(true));
  CHECK_EQ(wire_result(Op::kCallActuation, Dialect::kPython, canon)["isError"], json(true));
}

PMCP_TEST(lease_result_has_three_shapes_including_a_flat_conformance_grant) {
  json canon{{"lease",
              {{"lease_id", "l1"}, {"robot_id", "r"}, {"zone_id", "z"},
               {"state", "ACTIVE"}, {"expires_at", 1000.0}, {"fence_token", 3}}}};

  auto py = wire_result(Op::kLeaseRequest, Dialect::kPython, canon);
  CHECK_EQ(py["lease"]["lease_id"], json("l1"));
  CHECK_EQ(py["lease"]["expires_at"], json(1000.0));
  CHECK_EQ(py["lease"]["valid"], json(true));

  auto v05 = wire_result(Op::kLeaseRequest, Dialect::kV05, canon);
  CHECK_EQ(v05["lease"]["leaseId"], json("l1"));
  CHECK_EQ(v05["lease"]["zoneId"], json("z"));
  CHECK_EQ(v05["lease"]["expiresAt"], json(1000.0));
  CHECK(v05["lease"].contains("remainingMs"));
  CHECK_EQ(v05["lease"]["fenceToken"], json(3));

  auto conf = wire_result(Op::kLeaseRequest, Dialect::kConformance, canon);
  // test_04 reads result.get("granted") at the TOP level and result["lease_id"].
  CHECK_EQ(conf["granted"], json(true));
  CHECK_EQ(conf["lease_id"], json("l1"));
  CHECK(conf.contains("expires_ms"));
  CHECK_EQ(conf["expires_ms"], json(1000000));
}

PMCP_TEST(denied_lease_reports_granted_false) {
  json canon{{"lease", {{"lease_id", "l2"}, {"state", "DENIED"}, {"expires_at", 0.0},
                        {"fence_token", 0}, {"deny_reason", "Auction lost or zone occupied"}}}};
  auto conf = wire_result(Op::kLeaseRequest, Dialect::kConformance, canon);
  CHECK_EQ(conf["granted"], json(false));
  auto v05 = wire_result(Op::kLeaseRequest, Dialect::kV05, canon);
  CHECK_EQ(v05["lease"]["state"], json("DENIED"));
}

PMCP_TEST(metrics_conformance_shape_is_camel_case_and_flat) {
  json canon{{"metrics", {{"calls_total", 7}, {"calls_blocked", 2}, {"uptime_s", 12.5},
                          {"sensor_reads", 3}, {"energy_used_j", 9.0}, {"connected_clients", 1},
                          {"last_heartbeat_ms", 42}}}};
  auto conf = wire_result(Op::kMetrics, Dialect::kConformance, canon);
  // test_05 requires every one of these.
  for (auto* k : {"actuationCount", "sensorReadCount", "safetyViolations",
                  "avgActuationDurationMs", "uptimeSeconds", "energyUsedJ",
                  "connectedClients", "lastHeartbeatMs"}) {
    CHECK(conf.contains(k));
  }
  CHECK_EQ(conf["actuationCount"], json(7));
  CHECK_EQ(conf["safetyViolations"], json(2));
}

PMCP_TEST(estop_result_shape_differs_per_dialect) {
  json engage{{"active", true}, {"triggered_at", 5.0}};
  json clear{{"active", false}, {"was_estopped", true}};

  CHECK_EQ(wire_result(Op::kEstop, Dialect::kV05, engage)["estop"], json(true));
  CHECK_EQ(wire_result(Op::kEstop, Dialect::kConformance, engage)["engaged"], json(true));
  auto py = wire_result(Op::kEstop, Dialect::kPython, engage);
  CHECK(py["estop"].is_object());
  CHECK_EQ(py["estop"]["stop_category"], json(0));

  auto dis = wire_result(Op::kEstopReset, Dialect::kConformance, clear);
  CHECK_EQ(dis["engaged"], json(false));
  CHECK_EQ(dis["was_estopped"], json(true));
  auto pyreset = wire_result(Op::kEstopReset, Dialect::kPython, clear);
  CHECK_EQ(pyreset["estopped"], json(false));
}

PMCP_TEST(sensor_catalog_wraps_under_the_right_key_per_dialect) {
  json canon{{"sensors", json::array({{{"name", "temp"}, {"uri", "pcp://r/sensors/temp"},
                                        {"description", "d"}, {"robot_id", "r"}}})}};
  CHECK_EQ(wire_result(Op::kListSensors, Dialect::kPython, canon)["sensors"].size(), 1u);
  auto v05 = wire_result(Op::kListSensors, Dialect::kV05, canon);
  CHECK_EQ(v05["resources"][0]["uri"], json("pcp://r/sensors/temp"));
  CHECK_EQ(v05["resources"][0]["mimeType"], json("application/json"));
  auto conf = wire_result(Op::kListSensors, Dialect::kConformance, canon);
  CHECK_EQ(conf["sensors"][0]["name"], json("temp"));
}

PMCP_TEST(actuation_catalog_wraps_as_tools_on_v05) {
  json canon{{"actuations", json::array({{{"name", "grip"}, {"description", "close"}}})}};
  auto v05 = wire_result(Op::kListActuations, Dialect::kV05, canon);
  CHECK_EQ(v05["tools"][0]["name"], json("grip"));
  CHECK(v05["tools"][0].contains("inputSchema"));
  CHECK_EQ(v05["tools"][0]["inputSchema"]["type"], json("object"));
  auto conf = wire_result(Op::kListActuations, Dialect::kConformance, canon);
  CHECK_EQ(conf["actuations"][0]["name"], json("grip"));
}

PMCP_TEST(ping_carries_both_seconds_and_milliseconds) {
  auto conf = wire_result(Op::kPing, Dialect::kConformance, {{"ts", 1700000000.0}});
  // test_05 reads `timestamp` in ms and requires it within 5s of now.
  CHECK(conf.contains("timestamp"));
  CHECK_EQ(conf["pong"], json(true));
  auto v05 = wire_result(Op::kPing, Dialect::kV05, {{"ts", 1700000000.0}});
  CHECK(v05.contains("ts"));
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

PMCP_TEST(non_ascii_is_escaped_to_match_python_json_dumps) {
  // Python's json.dumps defaults to ensure_ascii=True, so the reference servers
  // put these on the wire escaped. Emitting raw UTF-8 would differ byte-wise.
  json j{{"rule", "Z>=0 \xc2\xa7 5.4"}};
  const std::string s = dump_ascii(j);
  CHECK(s.find("\\u") != std::string::npos);
  CHECK(s.find("\xc2\xa7") == std::string::npos);
  // ...and it still round-trips to the same value.
  CHECK_EQ(json::parse(s)["rule"], j["rule"]);
}

PMCP_TEST(indent_dump_round_trips_and_is_pretty) {
  json j{{"a", 1}, {"b", {{"c", 2}}}};
  const std::string s = dump_indent_ascii(j);
  CHECK(s.find('\n') != std::string::npos);
  CHECK_EQ(json::parse(s), j);
}

PMCP_TEST(dialect_inference_for_request_driven_response_shape) {
  CHECK(dialect_of_method("tools/call").value_or(Dialect::kSpec) == Dialect::kV05);
  CHECK(dialect_of_method("actuations/execute").value_or(Dialect::kSpec) ==
        Dialect::kConformance);
  CHECK(dialect_of_method("actuations/call").value_or(Dialect::kV05) == Dialect::kSpec);
  CHECK(!dialect_of_method("nope").has_value());
}
