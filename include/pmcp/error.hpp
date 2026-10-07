// pmcp-cpp — Physical Context Protocol C++ SDK
//
// Error codes are the union of the two Python SDK enumerations. Where the
// SDKs disagree on the *meaning* of a numeric code we keep both names and
// resolve per dialect at the adapter boundary (see dialect.hpp).
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "pmcp/json.hpp"

namespace pmcp {

using json = nlohmann::json;

// Numeric codes. `pmcp-python/pcp` stops at -33010 (ZK_PROOF_INVALID);
// `v05` extends to -33015. The union is used; which subset a given dialect
// emits is a wire-compat decision, not a type-system one.
enum class Code : int {
  // JSON-RPC 2.0
  kParseError = -32700,
  kInvalidRequest = -32600,
  kMethodNotFound = -32601,
  kInvalidParams = -32602,
  kInternalError = -32603,

  // P-MCP physical safety, -33000 range
  kShadowBlocked = -33001,
  kConstitutionBlocked = -33002,
  kLeaseRequired = -33003,
  kLeaseExpired = -33004,
  kEstopActive = -33005,
  kFloorGuard = -33006,
  kSpeedLimit = -33007,
  kEnergyBudget = -33008,
  kHumanProximity = -33009,
  kZkProofInvalid = -33010,
  kJointLimit = -33011,
  kTorqueLimit = -33012,
  kWorkspaceViolation = -33013,
  kCollisionDetected = -33014,
  kRobotFault = -33015,
};

// Stable string name for a code, as used by pmcp-python/pcp::types.
std::string_view code_name(Code c) noexcept;
Code code_from_int(int v) noexcept;

// An error that is convertible to a JSON-RPC `error` member.
//
// `data` is omitted entirely from the wire when nullopt, matching both
// reference SDKs — they build `{"code", "message"}` and only add `data`
// when it is not None.
class Error {
 public:
  Error(Code code, std::string message)
      : code_(code), message_(std::move(message)) {}
  Error(Code code, std::string message, json data)
      : code_(code), message_(std::move(message)), data_(std::move(data)) {}

  [[nodiscard]] Code code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const json& data() const noexcept { return data_; }
  [[nodiscard]] bool has_data() const noexcept { return !data_.is_null(); }

  [[nodiscard]] json to_json() const {
    json d{{"code", static_cast<int>(code_)}, {"message", message_}};
    if (!data_.is_null()) d["data"] = data_;
    return d;
  }

  [[nodiscard]] std::string str() const {
    return std::string(code_name(code_)) + "(" + std::to_string(static_cast<int>(code_)) +
           "): " + message_;
  }

 private:
  Code code_;
  std::string message_;
  json data_ = nullptr;
};

}  // namespace pmcp