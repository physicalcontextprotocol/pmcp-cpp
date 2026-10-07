#include "pmcp/error.hpp"

namespace pmcp {

std::string_view code_name(Code c) noexcept {
  switch (c) {
    case Code::kParseError: return "PARSE_ERROR";
    case Code::kInvalidRequest: return "INVALID_REQUEST";
    case Code::kMethodNotFound: return "METHOD_NOT_FOUND";
    case Code::kInvalidParams: return "INVALID_PARAMS";
    case Code::kInternalError: return "INTERNAL_ERROR";
    case Code::kShadowBlocked: return "SHADOW_BLOCKED";
    case Code::kConstitutionBlocked: return "CONSTITUTION_BLOCKED";
    case Code::kLeaseRequired: return "LEASE_REQUIRED";
    case Code::kLeaseExpired: return "LEASE_EXPIRED";
    case Code::kEstopActive: return "ESTOP_ACTIVE";
    case Code::kFloorGuard: return "FLOOR_GUARD";
    case Code::kSpeedLimit: return "SPEED_LIMIT";
    case Code::kEnergyBudget: return "ENERGY_BUDGET";
    case Code::kHumanProximity: return "HUMAN_PROXIMITY";
    case Code::kZkProofInvalid: return "ZK_PROOF_INVALID";
    case Code::kJointLimit: return "JOINT_LIMIT";
    case Code::kTorqueLimit: return "TORQUE_LIMIT";
    case Code::kWorkspaceViolation: return "WORKSPACE_VIOLATION";
    case Code::kCollisionDetected: return "COLLISION_DETECTED";
    case Code::kRobotFault: return "ROBOT_FAULT";
  }
  return "UNKNOWN";
}

Code code_from_int(int v) noexcept {
  switch (v) {
    case -32700: return Code::kParseError;
    case -32600: return Code::kInvalidRequest;
    case -32601: return Code::kMethodNotFound;
    case -32602: return Code::kInvalidParams;
    case -32603: return Code::kInternalError;
    case -33001: return Code::kShadowBlocked;
    case -33002: return Code::kConstitutionBlocked;
    case -33003: return Code::kLeaseRequired;
    case -33004: return Code::kLeaseExpired;
    case -33005: return Code::kEstopActive;
    case -33006: return Code::kFloorGuard;
    case -33007: return Code::kSpeedLimit;
    case -33008: return Code::kEnergyBudget;
    case -33009: return Code::kHumanProximity;
    case -33010: return Code::kZkProofInvalid;
    case -33011: return Code::kJointLimit;
    case -33012: return Code::kTorqueLimit;
    case -33013: return Code::kWorkspaceViolation;
    case -33014: return Code::kCollisionDetected;
    case -33015: return Code::kRobotFault;
    default: return static_cast<Code>(v);
  }
}

}  // namespace pmcp