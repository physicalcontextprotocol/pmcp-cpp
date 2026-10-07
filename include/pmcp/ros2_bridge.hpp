// pmcp-cpp — ROS 2 bridge.
//
// Exposes a P-MCP server to the ROS 2 graph and lets ROS 2 callers drive P-MCP
// actuators. Compiled only when PMCP_WITH_ROS2=ON; the rest of pmcp-cpp has no
// ROS 2 dependency.
//
// Wire it up:
//   auto node = std::make_shared<pmcp::Ros2Bridge>(options);
//   node->attach(server);              // ROS topics -> P-MCP tools
//   Ros2Bridge::publish_status(server); // P-MCP telemetry -> ROS topics
#pragma once

#include <memory>
#include <string>

#include "pmcp/server.hpp"

#ifdef PMCP_WITH_ROS2

#include <rclcpp/rclcpp.hpp>

#include <geometry_msgs/msg/pose_stamped.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_srvs/srv/trigger.hpp>

namespace pmcp {

// Namespacing for the topics and services the bridge owns. Every name is
// relative to the node's namespace, so two bridges can coexist in different
// cells.
struct Ros2BridgeOptions {
  std::string robot_id = "pmcp-cpp";
  std::string sensor_topic_prefix = "pcp/sensors";
  std::string actuation_service_prefix = "pcp/actuate";
  std::string status_topic = "pcp/status";
  std::string estop_service = "pcp/estop";
  std::string lease_service = "pcp/lease";
  // When true, every actuation goes through the safety pipeline exactly as a
  // P-MCP client would, including the shadow preview and lease.
  bool enforce_safety = true;
  double status_period_s = 1.0;
};

class Ros2Bridge : public rclcpp::Node {
 public:
  explicit Ros2Bridge(Ros2BridgeOptions opts = {},
                      rclcpp::NodeOptions node_options = rclcpp::NodeOptions());
  ~Ros2Bridge() override = default;

  // Attach to a P-MCP server. After this, every registered sensor is published
  // on <prefix>/<sensor_name> and every actuation is reachable at
  // <actuation_service_prefix>/<actuation_name>. Must be called after the
  // server has its catalog populated.
  void attach(Server& server);

  // True once attach() has run.
  [[nodiscard]] bool attached() const noexcept { return static_cast<bool>(server_); }

  // Publish one status sample. Called by attach() on a timer when
  // status_period_s > 0; exposed so tests can drive it deterministically.
  void publish_status_now();

  [[nodiscard]] Server* server() const noexcept { return server_; }

 private:
  void on_actuation(const std::string& name, const std_srvs::srv::Trigger::Request::SharedPtr req,
                    std_srvs::srv::Trigger::Response::SharedPtr res);
  void on_estop(const std_srvs::srv::Trigger::Request::SharedPtr req,
                std_srvs::srv::Trigger::Response::SharedPtr res);
  void on_lease(const std_srvs::srv::Trigger::Request::SharedPtr req,
                std_srvs::srv::Trigger::Response::SharedPtr res);
  void sensor_timer();

  Ros2BridgeOptions opts_;
  Server* server_ = nullptr;

  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr status_pub_;
  rclcpp::TimerBase::SharedPtr status_timer_;
  rclcpp::TimerBase::SharedPtr sensor_timer_;

  std::map<std::string, rclcpp::Publisher<std_msgs::msg::String>::SharedPtr> sensor_pubs_;
  std::vector<std::string> sensor_names_;
  std::map<std::string, rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr> services_;
};

}  // namespace pmcp

#endif  // PMCP_WITH_ROS2