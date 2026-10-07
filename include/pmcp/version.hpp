// pmcp-cpp — Physical Context Protocol C++ SDK
#pragma once

#include <string_view>

namespace pmcp {

inline constexpr std::string_view kPcVersion = "0.5";
inline constexpr std::string_view kJsonRpcVersion = "2.0";
// The Anthropic MCP spec revision the v05 dialect aligns with.
inline constexpr std::string_view kMcpSpecVersion = "2024-11-05";
inline constexpr std::string_view kProtocolTag = "pcp/0.5";

}  // namespace pmcp