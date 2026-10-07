// A simulated arm exposed over stdio or HTTP.
//
//   ./pmcp_example_server --http --port 8080
//   ./pmcp_example_server --stdio
//
// The point of the example is that nothing here knows a wire method name: the
// handlers are registered against canonical ops and the dialect layer handles
// whatever spelling the caller used.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "pmcp/server.hpp"

namespace {

double g_x = 0.0, g_y = 0.0, g_z = 0.1;
double g_temperature = 38.0;
std::int64_t g_ticks = 0;

}  // namespace

int main(int argc, char** argv) {
  bool http = false;
  int port = 8080;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--http") == 0) http = true;
    else if (std::strncmp(argv[i], "--port=", 7) == 0) port = std::atoi(argv[i] + 7);
  }

  pmcp::ServerConfig cfg;
  cfg.name = "simulated-arm";
  cfg.version = "1.0.0";
  cfg.robot_id = "sim-arm";
  pmcp::Server server(cfg);

  // ---- motion -------------------------------------------------------------

  pmcp::ActuationSpec move;
  move.name = "move_to";
  move.description = "Move the tool centre point to an XYZ target";
  move.est_duration_s = 2.0;
  move.max_speed_m_s = 1.5;
  move.parameters = {
      {"x", "number", "x target in metres"},
      {"y", "number", "y target in metres"},
      {"z", "number", "z target in metres"},
      {"speed", "number", "speed limit in m/s"},
  };
  move.parameters[3].required = false;
  move.parameters[3].default_value = pmcp::json(0.5);
  move.parameters[3].minimum = 0.0;
  move.parameters[3].maximum = 2.0;
  server.register_actuation(move, [](const pmcp::json& a) {
    const double nx = a.value("x", 0.0);
    const double ny = a.value("y", 0.0);
    const double nz = a.value("z", 0.0);
    const double dist = std::sqrt((nx - g_x) * (nx - g_x) + (ny - g_y) * (ny - g_y) +
                                  (nz - g_z) * (nz - g_z));
    // A real actuator would refuse a move it cannot finish in time; the
    // simulated one just burns energy proportional to distance.
    g_x = nx;
    g_y = ny;
    g_z = nz;
    pmcp::ActuationOutcome r;
    r.output = {{"x", g_x}, {"y", g_y}, {"z", g_z}, {"distance_m", dist}};
    r.final_pose = {{"x", g_x}, {"y", g_y}, {"z", g_z}};
    r.energy_j = 8.0 + 4.0 * dist;
    return r;
  });

  // ---- gripper ------------------------------------------------------------
  //
  // requires_lease = false: a gripper close is not motion, and demanding a zone
  // lease for it means an E-STOP can leave a payload suspended. shadow_required
  // is likewise false — there is no trajectory to simulate.

  pmcp::ActuationSpec grip;
  grip.name = "open_gripper";
  grip.description = "Open the gripper";
  grip.category = "gripper";
  grip.requires_lease = false;
  grip.shadow_required = false;
  grip.est_duration_s = 0.4;
  server.register_actuation(grip, [](const pmcp::json&) {
    pmcp::ActuationOutcome r;
    r.output = {{"open", true}};
    r.energy_j = 1.5;
    return r;
  });

  pmcp::ActuationSpec close;
  close.name = "close_gripper";
  close.description = "Close the gripper to a target force";
  close.category = "gripper";
  close.requires_lease = false;
  close.shadow_required = false;
  close.max_force_n = 40.0;
  close.est_duration_s = 0.6;
  close.parameters = {{"force_n", "number", "grip force in newtons"}};
  close.parameters[0].required = false;
  close.parameters[0].default_value = pmcp::json(20.0);
  server.register_actuation(close, [](const pmcp::json& a) {
    const double f = a.value("force_n", 20.0);
    if (f > 40.0) {
      pmcp::ActuationOutcome r;
      r.success = false;
      r.error = "requested force exceeds the 40 N limit";
      return r;
    }
    pmcp::ActuationOutcome r;
    r.output = {{"closed", true}, {"force_n", f}};
    r.energy_j = 2.0;
    return r;
  });

  // ---- sensors ------------------------------------------------------------

  pmcp::SensorSpec pose;
  pose.name = "tcp_pose";
  pose.description = "Current tool centre point pose";
  pose.sensor_type = "pose";
  pose.unit = "m";
  server.register_sensor(pose, []() {
    return pmcp::json{{"x", g_x}, {"y", g_y}, {"z", g_z}};
  });

  pmcp::SensorSpec temp;
  temp.name = "temperature";
  temp.description = "Motor winding temperature";
  temp.unit = "C";
  server.register_sensor(temp, []() {
    g_ticks += 1;
    return g_temperature;
  });

  pmcp::SensorSpec joints;
  joints.name = "joint_angles";
  joints.description = "Joint positions";
  joints.unit = "rad";
  joints.hz = 50.0;
  joints.is_stream = true;
  server.register_sensor(joints, []() {
    return pmcp::json::array({0.1, -0.2, 0.5, 0.0, 1.1, 0.0});
  });

  // ---- prompt -------------------------------------------------------------

  pmcp::PromptSpec prompt;
  prompt.name = "pick_and_place";
  prompt.description = "Move to a source, close, move to a destination, open";
  prompt.arguments = {{"source_x", "number", "source x"},
                      {"source_y", "number", "source y"},
                      {"destination_x", "number", "destination x"},
                      {"destination_y", "number", "destination y"}};
  server.register_prompt(prompt, [](const pmcp::json& a) {
    return pmcp::json{{"steps",
                       pmcp::json::array({
                           pmcp::json{{"actuation", "move_to"},
                                      {"arguments",
                                       pmcp::json{{"x", a.value("source_x", 0.0)},
                                                  {"y", a.value("source_y", 0.0)},
                                                  {"z", g_z}}}},
                           pmcp::json{{"actuation", "close_gripper"}, {"arguments", pmcp::json::object()}},
                           pmcp::json{{"actuation", "move_to"},
                                      {"arguments",
                                       pmcp::json{{"x", a.value("destination_x", 0.0)},
                                                  {"y", a.value("destination_y", 0.0)},
                                                  {"z", g_z}}}},
                           pmcp::json{{"actuation", "open_gripper"}, {"arguments", pmcp::json::object()}},
                       })}};
  });

  if (http) {
    std::fprintf(stderr, "simulated-arm listening on http://127.0.0.1:%d/pcp\n", port);
    return server.serve_http("127.0.0.1", port);
  }
  return server.serve_stdio();
}
