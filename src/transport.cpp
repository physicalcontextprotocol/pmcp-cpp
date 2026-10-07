#include "pmcp/transport.hpp"

namespace pmcp {

bool StdioTransport::connect() {
  connected_ = in_ != nullptr && out_ != nullptr;
  return connected_;
}

void StdioTransport::close() { connected_ = false; }

bool StdioTransport::send(const json& message) {
  if (!connected_) return false;
  // Both Python servers write compact JSON followed by a single newline, and
  // both use json.dumps defaults (ensure_ascii=True), so escape non-ASCII.
  (*out_) << message.dump(-1, ' ', /*ensure_ascii=*/true) << "\n";
  out_->flush();
  return true;
}

std::optional<json> StdioTransport::receive() {
  if (!connected_) return std::nullopt;
  std::string line;
  if (!std::getline(*in_, line)) return std::nullopt;  // EOF
  if (line.empty()) return std::nullopt;
  try {
    return json::parse(line);
  } catch (const std::exception&) {
    // Sentinel rather than nullopt, because nullopt means "clean EOF" and the
    // server has to emit a -32700 here while staying silent on real EOF.
    return json{{"__pmcp_parse_error__", true}};
  }
}

}  // namespace pmcp