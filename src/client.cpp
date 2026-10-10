#include "pmcp/client.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <random>
#include <sstream>

#include "pmcp/server.hpp"

namespace pmcp {
namespace {

std::string random_hex(int n) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) s.push_back(kHex[rng() & 0xF]);
  return s;
}

struct Url {
  std::string scheme, host;
  int port = 80;
  std::string path = "/";
};

bool parse_url(const std::string& raw, Url& out) {
  std::string s = raw;
  auto scheme_end = s.find("://");
  if (scheme_end == std::string::npos) return false;
  out.scheme = s.substr(0, scheme_end);
  s = s.substr(scheme_end + 3);
  auto slash = s.find('/');
  std::string hostport = slash == std::string::npos ? s : s.substr(0, slash);
  out.path = slash == std::string::npos ? "/" : s.substr(slash);
  out.port = out.scheme == "https" ? 443 : 80;
  auto colon = hostport.find(':');
  if (colon == std::string::npos) {
    out.host = hostport;
  } else {
    out.host = hostport.substr(0, colon);
    out.port = std::atoi(hostport.c_str() + colon + 1);
  }
  return !out.host.empty();
}

}  // namespace

Client::Client(Config cfg) : cfg_(std::move(cfg)) {}

Client::Client() : Client(Config{}) {}Client::~Client() { disconnect(); }

std::string Client::method_for(Op op) const {
  if (auto m = method_name(op, cfg_.dialect); !m.empty()) return std::string(m);
  // Fall back to any dialect that defines this op, so e.g. metrics still works
  // for a client pinned to v05.
  for (Dialect d : {Dialect::kPython, Dialect::kV05, Dialect::kConformance, Dialect::kSpec}) {
    if (auto m = method_name(op, d); !m.empty()) return std::string(m);
  }
  return {};
}

std::string Client::endpoint_path() const {
  if (!cfg_.http_path.empty()) return cfg_.http_path;
  // The spec and pcp-conformance use /mcp; pcp-python/pcp uses /pcp; v05
  // serves both. Default to the spec's, since that is the documented endpoint.
  return "/mcp";
}

void Client::connect_stdio() {
  kind_ = Kind::kStdio;
  connected_ = true;
  (void)initialize();
}

void Client::connect_http(const std::string& base_url, const std::string& path) {
  if (!base_url.empty()) cfg_.http_base = base_url;
  if (!path.empty()) cfg_.http_path = path;
  kind_ = Kind::kHttp;
  connected_ = true;
  (void)initialize();
}

void Client::connect_inprocess(Server* server) {
  inprocess_ = server;
  kind_ = Kind::kInProcess;
  connected_ = true;
  (void)initialize();
}

void Client::disconnect() {
  if (kind_ == Kind::kNone) return;
  if (kind_ == Kind::kHttp || kind_ == Kind::kStdio) {
    // Best effort; the reference clients also swallow failures here.
    try { notify("notifications/initialized", json::object()); } catch (...) {}
  }
  connected_ = false;
  kind_ = Kind::kNone;
  inprocess_ = nullptr;
}

std::string Client::next_id() {
  // 8 hex chars, matching pcp-python's str(uuid4())[:8] so CONST-08 is
  // satisfied identically on both sides.
  return random_hex(8);
}

json Client::rpc(const std::string& method, const json& params) {
  if (!connected_) throw Error(Code::kInternalError, "Client is not connected");

  const std::string id = next_id();
  json req{{"jsonrpc", kJsonRpcVersion}, {"id", id}, {"method", method}, {"params", params}};

  json resp;
  switch (kind_) {
    case Kind::kInProcess:
      if (!inprocess_) throw Error(Code::kInternalError, "No in-process server bound");
      resp = inprocess_->handle_message(req).value_or(json::object());
      break;
    case Kind::kStdio:
      throw Error(Code::kInternalError,
                  "stdio client requires an external subprocess transport; use "
                  "connect_inprocess or connect_http");
    case Kind::kHttp: {
      const std::string raw = http_post(endpoint_path(), req.dump(-1, ' ', true));
      resp = json::parse(raw);
      break;
    }
    case Kind::kNone:
      break;
  }

  if (resp.contains("error")) {
    const json e = resp["error"];
    throw Error(code_from_int(e.value("code", -1)), e.value("message", ""),
                e.contains("data") ? e["data"] : json{});
  }
  return resp.value("result", json::object());
}

void Client::notify(const std::string& method, const json& params) {
  if (!connected_ || kind_ != Kind::kHttp) return;
  json req{{"jsonrpc", kJsonRpcVersion}, {"method", method}, {"params", params}};
  try {
    (void)http_post(endpoint_path(), req.dump(-1, ' ', true));
  } catch (...) {
    // pcp-python's _notify swallows all exceptions; so do we.
  }
}

std::string Client::http_post(const std::string& path, const std::string& body) const {
  Url u;
  if (!parse_url(cfg_.http_base, u)) {
    throw Error(Code::kInternalError, "Refusing non-http(s) URL: " + cfg_.http_base);
  }
  if (u.scheme != "http") {
    throw Error(Code::kInternalError, "TLS is not implemented; use http://");
  }

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw Error(Code::kInternalError, std::string("socket: ") + std::strerror(errno));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(u.port));
  if (::inet_pton(AF_INET, u.host.c_str(), &addr.sin_addr) != 1) {
    // Fall back to DNS for hostnames like "localhost".
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(u.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
      ::close(fd);
      throw Error(Code::kInternalError, "Cannot resolve host: " + u.host);
    }
    addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    ::freeaddrinfo(res);
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    const std::string e = std::strerror(errno);
    ::close(fd);
    throw Error(Code::kInternalError, "connect failed: " + e);
  }

  timeval tv{cfg_.timeout_ms / 1000, (cfg_.timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  std::ostringstream req;
  req << "POST " << path << " HTTP/1.1\r\n"
      << "Host: " << u.host << ":" << u.port << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << body;

  const std::string wire = req.str();
  size_t sent = 0;
  while (sent < wire.size()) {
    ssize_t n = ::send(fd, wire.data() + sent, wire.size() - sent, 0);
    if (n <= 0) break;
    sent += static_cast<std::size_t>(n);
  }

  std::string raw;
  char buf[8192];
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) raw.append(buf, static_cast<std::size_t>(n));
  ::close(fd);

  auto sep = raw.find("\r\n\r\n");
  if (sep == std::string::npos) throw Error(Code::kInternalError, "Malformed HTTP response");
  return raw.substr(sep + 4);
}

// ---------------------------------------------------------------------------
// Typed operations
// ---------------------------------------------------------------------------

json Client::initialize() {
  json out = rpc(method_for(Op::kInitialize),
                 {{"protocolVersion", std::string(kPcVersion)},
                  {"clientInfo", {{"name", cfg_.name}, {"version", cfg_.version}}},
                  {"capabilities", json::object()}});
  server_info_ = out.value("serverInfo", json::object());
  capabilities_ = out.value("capabilities", json::object());
  notify("notifications/initialized", json::object());
  return out;
}

json Client::ping() { return rpc(method_for(Op::kPing)); }

json Client::list_actuations() { return rpc(method_for(Op::kListActuations)); }

json Client::call_actuation(const std::string& name, const json& arguments,
                            const std::string& lease_token,
                            std::optional<std::int64_t> fence_token) {
  json params{{"name", name}, {"arguments", arguments}};
  if (!lease_token.empty()) {
    // v05 spells the PCP extensions with a leading underscore; the Python
    // dialects do not. Send both so either server reads what it expects.
    params["lease_token"] = lease_token;
    params["_lease_token"] = lease_token;
  }
  if (fence_token) {
    params["fence_token"] = *fence_token;
    params["_fence_token"] = *fence_token;
  }
  return rpc(method_for(Op::kCallActuation), params);
}

json Client::batch_actuate(const json& actuations, bool atomic, const std::string& zone_id) {
  if (method_for(Op::kBatchActuation).empty()) {
    throw Error(Code::kMethodNotFound,
                "The configured dialect has no batch method; iterate call_actuation instead");
  }
  return rpc(method_for(Op::kBatchActuation),
             {{"actuations", actuations}, {"atomic", atomic}, {"zone_id", zone_id}});
}

json Client::list_sensors() { return rpc(method_for(Op::kListSensors)); }

json Client::read_sensor(const std::string& name_or_uri) {
  json params{{"name", name_or_uri}};
  // v05 reads `uri`; the others read `name`. Send both.
  params["uri"] = name_or_uri;
  return rpc(method_for(Op::kReadSensor), params);
}

json Client::list_prompts() { return rpc(method_for(Op::kListPrompts)); }

json Client::get_prompt(const std::string& name, const json& arguments) {
  return rpc(method_for(Op::kGetPrompt), {{"name", name}, {"arguments", arguments}});
}

json Client::shadow_preview(const std::string& name, const json& arguments) {
  json params{{"arguments", arguments}};
  params["name"] = name;
  params["actuation_name"] = name;  // pcp-python prefers this spelling
  return rpc(method_for(Op::kShadowPreview), params);
}

json Client::request_lease(const std::string& zone_id, const std::string& robot_id,
                           std::int64_t duration_ms, double bid_energy_j) {
  json params{{"zone_id", zone_id}, {"duration_ms", duration_ms}, {"bid_energy_j", bid_energy_j}};
  if (!robot_id.empty()) params["robot_id"] = robot_id;
  // v05 reads camelCase.
  params["zoneId"] = zone_id;
  params["durationMs"] = duration_ms;
  params["bidEnergyJ"] = bid_energy_j;
  if (!robot_id.empty()) params["robotId"] = robot_id;
  return rpc(method_for(Op::kLeaseRequest), params);
}

json Client::release_lease(const std::string& lease_id) {
  json params{{"lease_id", lease_id}};
  params["leaseId"] = lease_id;
  return rpc(method_for(Op::kLeaseRelease), params);
}

json Client::estop(bool active, const std::string& source) {
  json params{{"active", active}};
  if (!source.empty()) params["source"] = source;
  if (cfg_.dialect == Dialect::kConformance) {
    return rpc(active ? "safety/estop/engage" : "safety/estop/disengage", params);
  }
  return rpc(method_for(Op::kEstop), params);
}

json Client::estop_reset() {
  if (cfg_.dialect == Dialect::kConformance) return rpc("safety/estop/disengage", json::object());
  return rpc(method_for(Op::kEstopReset), json::object());
}

json Client::metrics() { return rpc(method_for(Op::kMetrics)); }

json Client::safe_actuation(const std::string& name, const json& arguments,
                             const std::string& zone_id, bool release_after) {
  auto preview_res = shadow_preview(name, arguments);
  bool safe = true;
  if (preview_res.contains("preview")) {
    const auto& pv = preview_res["preview"];
    // v05 reports `safe`; pcp-python reports a `verdict`.
    if (pv.contains("safe")) safe = pv["safe"].get<bool>();
    else if (pv.contains("verdict")) safe = pv["verdict"] == "PASS";
  }
  if (!safe) {
    throw Error(Code::kShadowBlocked, "Shadow preview rejected " + name, preview_res);
  }

  auto lease_res = request_lease(zone_id);
  std::string lease_id;
  std::int64_t fence = 0;
  bool granted = false;
  if (lease_res.contains("lease")) {
    const auto& l = lease_res["lease"];
    granted = l.value("state", std::string("ACTIVE")) == "ACTIVE";
    lease_id = l.value("lease_id", l.value("leaseId", std::string{}));
    fence = l.value("fence_token", l.value("fenceToken", static_cast<std::int64_t>(0)));
  } else {
    // Conformance dialect returns a flat grant.
    granted = lease_res.value("granted", false);
    lease_id = lease_res.value("lease_id", std::string{});
  }
  if (!granted) {
    throw Error(Code::kLeaseRequired, "Lease denied for zone " + zone_id, lease_res);
  }

  try {
    json out = call_actuation(name, arguments, lease_id, fence);
    if (release_after) (void)release_lease(lease_id);
    return out;
  } catch (...) {
    // Release on failure too, matching pcp-python's finally block.
    if (release_after) (void)release_lease(lease_id);
    throw;
  }
  // When release_after is false the lease is deliberately retained so a caller
  // can reuse it across calls — pcp-python always releases and discards it.
}

}  // namespace pmcp