// C++ client driver for the interop harness.
//
// Python launches this against a real SDK server, so the harness can assert on
// the C++ client's actual wire output rather than on a hand-written JSON blob.
// Every check is a hard exit code; the JSON summary on stdout is for humans.
//
//   cpp_client_main --dialect=v05|http|python|conformance --url=http://... [--path=/mcp]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "pmcp/client.hpp"
#include "pmcp/error.hpp"

using namespace pmcp;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
  std::cout << (ok ? "    ok   " : "    FAIL ") << what << "\n";
  if (!ok) ++g_failures;
}

// Unwrap the three content shapes the SDKs use, so one assertion works
// regardless of which dialect the server answered in.
bool actuation_succeeded(const json& result) {
  if (result.contains("success")) return result["success"].get<bool>();
  if (result.contains("isError")) return !result["isError"].get<bool>();
  if (result.contains("content") && result["content"].is_array() && !result["content"].empty()) {
    const auto& block = result["content"][0];
    if (block.contains("data")) return block["data"].value("success", false);
    if (block.contains("text") && block["text"].is_string()) {
      try {
        return json::parse(block["text"].get<std::string>()).value("success", false);
      } catch (const std::exception&) {
        return false;
      }
    }
  }
  return false;
}

std::string lease_id_from(const json& result) {
  if (result.contains("lease") && result["lease"].contains("lease_id"))
    return result["lease"]["lease_id"].get<std::string>();
  if (result.contains("lease") && result["lease"].contains("leaseId"))
    return result["lease"]["leaseId"].get<std::string>();
  if (result.contains("lease_id")) return result["lease_id"].get<std::string>();
  return {};
}

std::string lease_state_from(const json& result) {
  if (result.contains("lease") && result["lease"].contains("state"))
    return result["lease"]["state"].get<std::string>();
  if (result.contains("granted")) return result["granted"].get<bool>() ? "ACTIVE" : "DENIED";
  return {};
}

std::string actuation_names_from(const json& result) {
  std::string out;
  for (auto* key : {"actuations", "tools"}) {
    if (!result.contains(key) || !result[key].is_array()) continue;
    for (const auto& e : result[key]) {
      if (!out.empty()) out += ",";
      out += e.value("name", "");
    }
  }
  return out;
}

std::string sensor_names_from(const json& result) {
  std::string out;
  for (auto* key : {"sensors", "resources"}) {
    if (!result.contains(key) || !result[key].is_array()) continue;
    for (const auto& e : result[key]) {
      if (!out.empty()) out += ",";
      out += e.contains("name") ? e["name"].get<std::string>() : e.value("uri", "");
    }
  }
  return out;
}

Dialect parse_dialect(const std::string& s) {
  if (s == "python") return Dialect::kPython;
  if (s == "v05") return Dialect::kV05;
  if (s == "conformance") return Dialect::kConformance;
  if (s == "spec") return Dialect::kSpec;
  return Dialect::kAuto;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dialect = "v05";
  std::string url = "http://127.0.0.1:8080";
  std::string path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--dialect=", 0) == 0) dialect = a.substr(10);
    else if (a.rfind("--url=", 0) == 0) url = a.substr(6);
    else if (a.rfind("--path=", 0) == 0) path = a.substr(7);
  }

  Client::Config cfg;
  cfg.dialect = parse_dialect(dialect);
  cfg.name = "cpp-interop-client";
  Client client(cfg);

  try {
    client.connect_http(url, path);
    check(client.connected(), "connected");
    check(!client.server_info().empty(), "initialize returned serverInfo");

    const json acts = client.list_actuations();
    check(!actuation_names_from(acts).empty(), "actuation catalog is non-empty: " +
                                                   actuation_names_from(acts));

    // Shadow preview must succeed before we move anything.
    const json pv = client.shadow_preview("move_to", {{"x", 0.2}, {"y", 0.2}, {"z", 0.4}});
    check(!pv.contains("error"), "shadow/preview accepted");

    // Lease, actuate through the full pipeline, release.
    const json lease = client.request_lease("interop-zone", "cpp-interop-client", 15000);
    const std::string lease_id = lease_id_from(lease);
    const std::string state = lease_state_from(lease);
    check(state == "ACTIVE", "lease granted (state=" + state + ")");
    check(!lease_id.empty(), "lease id returned");

    std::int64_t fence = 0;
    if (lease.contains("lease") && lease["lease"].contains("fence_token"))
      fence = lease["lease"]["fence_token"].get<std::int64_t>();

    const json call = client.call_actuation("move_to", {{"x", 0.2}, {"y", 0.2}, {"z", 0.4}},
                                            lease_id, fence);
    check(actuation_succeeded(call), "actuation executed");

    const json sensors = client.list_sensors();
    check(!sensor_names_from(sensors).empty(), "sensor catalog is non-empty: " +
                                                 sensor_names_from(sensors));
    const json read = client.read_sensor("temperature");
    check(!read.contains("error"), "sensor read accepted");

    // E-stop must latch and block motion. Client::rpc surfaces server errors
    // as exceptions, so a blocked actuation arrives as Error(-33005), not as
    // an error-shaped result.
    const json stop = client.estop(true);
    check(!stop.contains("error"), "estop engaged");
    bool blocked_ok = false;
    try {
      (void)client.call_actuation("move_to", {{"x", 0.2}, {"y", 0.2}, {"z", 0.4}},
                                  lease_id, fence);
    } catch (const Error& e) {
      blocked_ok = (e.code() == Code::kEstopActive || e.code() == Code::kConstitutionBlocked);
    }
    check(blocked_ok, "actuation blocked while e-stopped");

    const json cleared = client.estop_reset();
    check(!cleared.contains("error"), "estop reset");

    // Unknown actuation must be a clean -32601, surfaced as an exception since
    // Client::rpc throws on any server error.
    bool missing_ok = false;
    try {
      (void)client.call_actuation("no_such_actuation", json::object(), lease_id, fence);
    } catch (const Error& e) {
      missing_ok = e.code() == Code::kMethodNotFound &&
                   e.message().find("move_to") != std::string::npos;
    }
    check(missing_ok, "unknown actuation returns -32601");

    if (!lease_id.empty()) {
      const json rel = client.release_lease(lease_id);
      check(!rel.contains("error"), "lease released");
    }

    const json ping = client.ping();
    check(!ping.contains("error"), "ping answered");
  } catch (const Error& e) {
    std::cout << "    FAIL exception: " << e.str() << "\n";
    ++g_failures;
  } catch (const std::exception& e) {
    std::cout << "    FAIL std::exception: " << e.what() << "\n";
    ++g_failures;
  }

  client.disconnect();
  std::cout << (g_failures == 0 ? "  cpp client: OK\n" : "  cpp client: FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
