#include "pmcp/ros2_bridge.hpp"

// When built without ROS 2 this file is intentionally almost empty: pcp-cpp
// must build and pass its tests on a machine with no ROS install, which is the
// situation on macOS. The bridge is additive, never load-bearing.

#ifdef PMCP_WITH_ROS2

namespace pmcp {

Ros2Bridge::Ros2Bridge(Ros2BridgeOptions opts, rclcpp::NodeOptions node_options)
    : rclcpp::Node(std::string(opts.robot_id) + "_pcp_bridge", node_options), opts_(std::move(opts)) {
  status_pub_ = create_publisher<std_msgs::msg::String>(opts_.status_topic, 10);

  auto trig_estop = create_service<std_srvs::srv::Trigger>(
      opts_.estop_service,
      [this](const std_srvs::srv::Trigger::Request::SharedPtr req,
             std_srvs::srv::Trigger::Response::SharedPtr res) { on_estop(req, res); });

  auto trig_lease = create_service<std_srvs::srv::Trigger>(
      opts_.lease_service,
      [this](const std_srvs::srv::Trigger::Request::SharedPtr req,
             std_srvs::srv::Trigger::Response::SharedPtr res) { on_lease(req, res); });
}

void Ros2Bridge::attach(Server& server) {
  server_ = &server;

  for (const auto& [name, spec] : server.catalog_for_bridge()) {
    sensor_names_.push_back(name);
    sensor_pubs_[name] = create_publisher<std_msgs::msg::String>(
        opts_.sensor_topic_prefix + "/" + name, 10);
  }

  // One Trigger service per actuation, named after the tool. Trigger carries no
  // arguments, so this covers zero-argument actuations; use PCP directly for
  // parameterized ones.
  for (const auto& name : server.actuation_names_for_bridge()) {
    services_[name] = create_service<std_srvs::srv::Trigger>(
        opts_.actuation_service_prefix + "/" + name,
        [this, name](const std_srvs::srv::Trigger::Request::SharedPtr req,
                     std_srvs::srv::Trigger::Response::SharedPtr res) {
          on_actuation(name, req, res);
        });
  }

  if (opts_.status_period_s > 0.0) {
    status_timer_ = create_wall_timer(
        std::chrono::duration<double>(opts_.status_period_s),
        [this]() { publish_status_now(); });
  }
}

void Ros2Bridge::publish_status_now() {
  if (!server_) return;
  auto st = server_->status();
  status_pub_->publish(std_msgs::msg::String{}.set__data(dump_ascii(st)));
}

void Ros2Bridge::sensor_timer() {
  if (!server_) return;
  for (const auto& name : sensor_names_) {
    auto it = sensor_pubs_.find(name);
    if (it == sensor_pubs_.end()) continue;
    try {
      auto res = server_->call("resources/read", {{"uri", name}});
      it->second->publish(std_msgs::msg::String{}.set__data(dump_ascii(res)));
    } catch (const Error&) {
      // A sensor that fails to read publishes nothing; do not take the node
      // down over one bad reading.
    }
  }
}

void Ros2Bridge::on_actuation(const std::string& name,
                              const std_srvs::srv::Trigger::Request::SharedPtr /*req*/,
                              std_srvs::srv::Trigger::Response::SharedPtr res) {
  if (!server_) {
    res->success = false;
    res->message = "bridge not attached";
    return;
  }
  try {
    json out = opts_.enforce_safety
                   ? [&] {
                       // Route through the same pipeline a PCP client uses so
                       // the shadow preview and lease are not bypassed.
                       json res2 = server_->call("actuations/call",
                                                 {{"name", name}, {"arguments", json::object()}});
                       return res2;
                     }()
                   : server_->call("actuations/call",
                                   {{"name", name}, {"arguments", json::object()}});
    res->success = !out.contains("error");
    res->message = res->success ? name + " executed" : dump_ascii(out.value("error", json::object()));
  } catch (const Error& e) {
    res->success = false;
    res->message = e.str();
  }
}

void Ros2Bridge::on_estop(const std_srvs::srv::Trigger::Request::SharedPtr /*req*/,
                          std_srvs::srv::Trigger::Response::SharedPtr res) {
  if (!server_) {
    res->success = false;
    res->message = "bridge not attached";
    return;
  }
  // A Trigger call means "stop". Clearing it needs an explicit PCP
  // pcp/estop_reset, because an accidental second Trigger must not resume a
  // robot that a human stopped.
  auto out = server_->call("pcp/estop", {{"active", true}});
  res->success = !out.contains("error");
  res->message = "emergency stop engaged";
}

void Ros2Bridge::on_lease(const std_srvs::srv::Trigger::Request::SharedPtr req,
                          std_srvs::srv::Trigger::Response::SharedPtr res) {
  if (!server_) {
    res->success = false;
    res->message = "bridge not attached";
    return;
  }
  const std::string zone = req->request.empty() ? "default" : req->request;
  auto out = server_->call("lease/request", {{"zone_id", zone}});
  if (out.contains("error")) {
    res->success = false;
    res->message = dump_ascii(out["error"]);
    return;
  }
  const auto& lease = out.value("lease", json::object());
  const bool granted = lease.value("state", std::string("DENIED")) == "ACTIVE";
  res->success = granted;
  res->message = lease.value("lease_id", std::string{});
}

}  // namespace pmcp

#endif  // PMCP_WITH_ROS2