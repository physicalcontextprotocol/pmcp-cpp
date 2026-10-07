// pmcp-cpp — transports.
//
// Two transports, matching the reference SDKs byte-for-byte where it matters:
//   - stdio: newline-delimited JSON, one request per line, one response line.
//     NOT Content-Length framed.
//   - http:  HTTP/1.1, one request per connection, Connection: close.
//     JSON-RPC errors still return 200; only a parse failure returns 400.
#pragma once

#include <istream>
#include <ostream>
#include <string>

#include "pmcp/json.hpp"

namespace pmcp {

class Transport {
 public:
  virtual ~Transport() = default;
  virtual bool connect() = 0;
  virtual void close() = 0;
  virtual bool send(const json& message) = 0;
  // Returns nullopt at EOF.
  virtual std::optional<json> receive() = 0;
};

// Reads newline-delimited JSON from an istream and writes it to an ostream.
// Usable for real stdio and for tests.
class StdioTransport : public Transport {
 public:
  StdioTransport(std::istream* in, std::ostream* out) : in_(in), out_(out) {}
  bool connect() override;
  void close() override;
  bool send(const json& message) override;
  std::optional<json> receive() override;

 private:
  std::istream* in_;
  std::ostream* out_;
  bool connected_ = false;
};

// Dispatches straight into a callback — no serialization at all. Mirrors
// pmcp-python's `connect_inprocess`.
class InProcessTransport : public Transport {
 public:
  using Handler = std::function<json(const json&)>;
  explicit InProcessTransport(Handler h) : handler_(std::move(h)) {}
  bool connect() override { return true; }
  void close() override {}
  bool send(const json&) override { return true; }
  std::optional<json> receive() override { return std::nullopt; }
  json request(const json& msg) const { return handler_(msg); }

 private:
  Handler handler_;
};

}  // namespace pmcp