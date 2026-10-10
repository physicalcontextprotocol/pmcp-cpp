// pcp-cpp — server.
//
// One implementation, every dialect. Handlers are registered against the
// canonical Op enum and never against a wire method name; Server::handle_message
// resolves whatever alias arrived, normalizes the params, runs the canonical
// handler, and serializes the result back in the caller's dialect.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pmcp/dialect.hpp"
#include "pmcp/error.hpp"
#include "pmcp/http.hpp"
#include "pmcp/safety.hpp"
#include "pmcp/transport.hpp"

namespace pmcp {

// ---------------------------------------------------------------------------
// Registration specs
// ---------------------------------------------------------------------------

struct ActuationParameter {
  std::string name;
  std::string type = "string";
  std::string description;
  std::string unit;
  bool required = true;
  std::optional<json> default_value;
  std::optional<double> minimum;
  std::optional<double> maximum;
};

struct ActuationSpec {
  std::string name;
  std::string description;
  std::vector<ActuationParameter> parameters;
  std::string robot_id;
  std::string category = "motion";
  std::optional<double> max_speed_m_s;
  std::optional<double> max_force_n;
  std::optional<double> max_energy_j;
  double est_duration_s = 0.1;
  bool requires_lease = true;
  bool shadow_required = true;
  std::string iso_class = "ISO10218";
};

struct ActuationOutcome {
  bool success = true;
  json output = json::object();
  json final_pose = nullptr;
  std::string error;
  double energy_j = 0.0;
};

using ActuationFn = std::function<ActuationOutcome(const json& args)>;

struct SensorSpec {
  std::string name;
  std::string description;
  std::string uri;  // derived from robot_id if left empty
  std::string robot_id;
  std::string sensor_type = "joint_states";
  std::string unit;
  double hz = 10.0;
  bool is_stream = false;
};

using SensorFn = std::function<json()>;

struct PromptSpec {
  std::string name;
  std::string description;
  std::vector<ActuationParameter> arguments;
};

using PromptFn = std::function<json(const json& args)>;

// ---------------------------------------------------------------------------

struct ServerConfig {
  std::string name = "pcp-cpp";
  std::string version = "1.0.0";
  std::string robot_id;  // defaults to name
  Dialect dialect = Dialect::kAuto;
  SafetyProfile safety_profile = SafetyProfile::kDefault;
  bool enable_safety = true;
  // HTTP paths to accept. The four dialects disagree here too: the spec says
  // /mcp, pcp-python/pcp says /pcp, v05 serves both, and pcp-conformance's
  // own helper posts to the bare root in five of its six test files.
  std::vector<std::string> http_paths = {"/pcp", "/mcp", "/"};
  std::size_t max_audit_entries = 10'000;
};

// ---------------------------------------------------------------------------

class Server {
 public:
  // Same reason as Client: ServerConfig's initializers are incomplete inside
  // this class body, so the defaults come from the no-argument overload.
  explicit Server(ServerConfig cfg);
  Server();
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  void register_actuation(ActuationSpec spec, ActuationFn fn);
  void register_sensor(SensorSpec spec, SensorFn fn);
  void register_prompt(PromptSpec spec, PromptFn fn);

  const ServerConfig& config() const noexcept { return cfg_; }
  Safety& safety() noexcept { return safety_; }
  [[nodiscard]] const Safety& safety() const noexcept { return safety_; }

  // ---- JSON-RPC core ----------------------------------------------------

  // Handle a complete JSON-RPC request object. Returns the response object, or
  // nullopt for a notification (no id), which both reference servers treat as
  // "write nothing" on stdio.
  std::optional<json> handle_message(const json& request);

  // Handle a bare (method, params, id) triple — the unit-test entry point and
  // the path the in-process transport uses.
  json call(const std::string& method, const json& params = json::object(),
            const json& id = json(1));

  // ---- transports -------------------------------------------------------

  // Newline-delimited JSON on stdin/stdout, as pcp-python/pcp and v05 do.
  int serve_stdio(std::istream* in = nullptr, std::ostream* out = nullptr);

  // Minimal HTTP/1.1 server: one request per connection, Connection: close,
  // exactly as pcp-python/pcp::_run_http. JSON-RPC errors still return 200;
  // only a parse failure returns 400.
  int serve_http(const std::string& host = "127.0.0.1", int port = 8080);

  [[nodiscard]] int bound_port() const noexcept { return bound_port_.load(); }

  // Full HTTP request -> response, exposed so tests can exercise routing and
  // the 400-on-parse-error path without binding a socket. Non-const because it
  // runs the JSON-RPC dispatch, which updates counters and the audit log.
  HttpResponse handle_http_request(const std::string& path, const std::string& body);

  // ---- introspection ----------------------------------------------------

  [[nodiscard]] json actuator_catalog() const;
  [[nodiscard]] json sensor_catalog() const;
  [[nodiscard]] json prompt_catalog() const;
  // Non-const: metrics() prunes expired leases before counting them, matching
  // the reference servers, which call the same eviction inside their metrics.
  json metrics();
  [[nodiscard]] json status() const;
  [[nodiscard]] json identity() const;

  // Every alias this server answers to, for `tools/list`-style advertising and
  // for the interop tests.
  [[nodiscard]] std::vector<std::string> served_methods() const;

  // Registry views for the ROS 2 bridge, which needs names rather than the
  // JSON-RPC-shaped catalogs.
  [[nodiscard]] std::vector<std::pair<std::string, SensorSpec>> catalog_for_bridge() const;
  [[nodiscard]] std::vector<std::string> actuation_names_for_bridge() const;
  [[nodiscard]] std::vector<std::pair<std::string, ActuationSpec>> actuations_for_bridge() const;

 private:
  using Handler = std::function<json(const json& cparams, const json& id)>;

  json dispatch(Op op, const json& cparams, const json& id);

  void audit(const std::string& event_type, const std::string& outcome,
             const json& extra = json::object());
  // Same, for callers that already hold mtx_. std::mutex is not recursive, so
  // calling audit() while holding the lock would self-deadlock.
  void audit_locked(const std::string& event_type, const std::string& outcome,
                    const json& extra = json::object());

  ServerConfig cfg_;
  Safety safety_;

  mutable std::mutex mtx_;
  std::map<std::string, ActuationSpec> actuations_;
  std::map<std::string, ActuationFn> actuation_fns_;
  std::map<std::string, SensorSpec> sensors_;
  std::map<std::string, SensorFn> sensor_fns_;
  std::map<std::string, PromptSpec> prompts_;
  std::map<std::string, PromptFn> prompt_fns_;

  std::vector<json> audit_log_;
  double started_at_ = 0.0;
  std::int64_t call_count_ = 0;
  std::int64_t blocked_count_ = 0;
  std::int64_t sensor_reads_ = 0;
  std::int64_t lease_denials_ = 0;
  std::int64_t estop_count_ = 0;
  double energy_used_j_ = 0.0;
  double total_duration_s_ = 0.0;
  std::int64_t connected_clients_ = 0;

  std::atomic<int> bound_port_{0};
};

// Join violation strings the way both Python SDKs do: "; ".
std::string join(const std::vector<std::string>& v);

}  // namespace pmcp