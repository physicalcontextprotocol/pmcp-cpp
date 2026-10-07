// pmcp-cpp — client.
//
// Speaks any dialect. Method names are chosen by ClientConfig::dialect and
// every request goes through the same normalization path a server would use,
// so the C++ client can drive pmcp-python/pcp, v05, the Rust SDK, or the
// TypeScript SDK without change.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pmcp/dialect.hpp"
#include "pmcp/error.hpp"

namespace pmcp {

class Server;

class Client {
 public:
  struct Config {
    std::string name = "pmcp-cpp-client";
    std::string version = "1.0.0";
    Dialect dialect = Dialect::kV05;  // the dialect with the most test evidence
    std::string http_base = "http://127.0.0.1:8080";
    std::string http_path;  // defaults to the dialect's canonical endpoint
    int timeout_ms = 30'000;
  };

  // Config's member initializers are not complete inside this class body, so a
  // `Config cfg = {}` default argument is ill-formed here. The defaults are
  // applied by the no-argument overload, defined out of line.
  explicit Client(Config cfg);
  Client();
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Newline-delimited JSON over this process's stdin/stdout.
  void connect_stdio();
  // POST to http_base + path.
  void connect_http(const std::string& base_url = {}, const std::string& path = {});
  // Talk to a Server object with no serialization.
  void connect_inprocess(Server* server);
  void disconnect();

  [[nodiscard]] bool connected() const noexcept { return connected_; }
  [[nodiscard]] const json& server_info() const noexcept { return server_info_; }
  [[nodiscard]] const json& capabilities() const noexcept { return capabilities_; }

  // Raw escape hatch. `method` is any dialect's spelling.
  json rpc(const std::string& method, const json& params = json::object());
  void notify(const std::string& method, const json& params = json::object());

  // ---- typed operations (canonical; the dialect layer handles the wire) ---
  json initialize();
  json ping();
  json list_actuations();
  json call_actuation(const std::string& name, const json& arguments = json::object(),
                      const std::string& lease_token = {},
                      std::optional<std::int64_t> fence_token = std::nullopt);
  json batch_actuate(const json& actuations, bool atomic = true,
                     const std::string& zone_id = "default");
  json list_sensors();
  json read_sensor(const std::string& name_or_uri);
  json list_prompts();
  json get_prompt(const std::string& name, const json& arguments = json::object());
  json shadow_preview(const std::string& name, const json& arguments = json::object());
  json request_lease(const std::string& zone_id, const std::string& robot_id = {},
                     std::int64_t duration_ms = 10'000, double bid_energy_j = 100.0);
  json release_lease(const std::string& lease_id);
  json estop(bool active = true, const std::string& source = {});
  json estop_reset();
  json metrics();

  // shadow -> lease -> call -> release, with the safety outcome checked between
  // each step. Mirrors pmcp-python's `safe_actuation`, except the lease is held
  // only if you ask (the reference always releases, which silently discards a
  // lease you may have wanted to keep).
  json safe_actuation(const std::string& name, const json& arguments,
                      const std::string& zone_id = "default", bool release_after = true);

  // The wire method name this client uses for an op on its configured dialect.
  [[nodiscard]] std::string method_for(Op op) const;

 private:
  std::string endpoint_path() const;

  Config cfg_;
  bool connected_ = false;

  enum class Kind { kNone, kStdio, kHttp, kInProcess };
  Kind kind_ = Kind::kNone;
  Server* inprocess_ = nullptr;

  std::string http_post(const std::string& path, const std::string& body) const;
  std::string next_id();

  json server_info_ = json::object();
  json capabilities_ = json::object();
  std::int64_t id_counter_ = 0;
};

}  // namespace pmcp