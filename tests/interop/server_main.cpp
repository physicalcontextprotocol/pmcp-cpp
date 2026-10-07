// A small C++ server used by tests/interop/test_interop.py.
//
// Deliberately identical in shape to examples/simulated_arm_server.cpp so that
// interop failures point at the protocol layer rather than at a test-only
// server. Prints `listening on 127.0.0.1:<port>` on stdout once bound, so the
// Python harness can wait on a concrete readiness signal instead of sleeping.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pmcp/server.hpp"

int main(int argc, char** argv) {
  std::string mode = "http";
  int port = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--stdio") == 0) mode = "stdio";
    else if (std::strncmp(argv[i], "--port=", 7) == 0) port = std::atoi(argv[i] + 7);
  }

  pmcp::ServerConfig cfg;
  cfg.name = "cpp-interop-arm";
  cfg.version = "1.0.0";
  cfg.robot_id = "cpp-arm";
  pmcp::Server server(cfg);

  pmcp::ActuationSpec move;
  move.name = "move_to";
  move.description = "Move the TCP to an XYZ target";
  move.est_duration_s = 1.5;
  move.parameters = {{"x", "number", "x"},
                     {"y", "number", "y"},
                     {"z", "number", "z"}};
  server.register_actuation(move, [](const pmcp::json& a) {
    pmcp::ActuationOutcome r;
    r.output = {{"x", a.value("x", 0.0)}, {"y", a.value("y", 0.0)}, {"z", a.value("z", 0.0)}};
    r.energy_j = 10.0;
    r.final_pose = r.output;
    return r;
  });

  pmcp::ActuationSpec grip;
  grip.name = "open_gripper";
  grip.description = "Open the gripper";
  grip.shadow_required = false;
  grip.requires_lease = false;
  grip.category = "gripper";
  server.register_actuation(grip, [](const pmcp::json&) {
    pmcp::ActuationOutcome r;
    r.output = {{"open", true}};
    r.energy_j = 1.0;
    return r;
  });

  pmcp::SensorSpec temp;
  temp.name = "temperature";
  temp.description = "Motor temperature";
  temp.unit = "C";
  server.register_sensor(temp, []() { return pmcp::json(40.0); });

  pmcp::PromptSpec p;
  p.name = "hello";
  p.description = "Say hello";
  server.register_prompt(p, [](const pmcp::json&) { return pmcp::json("hello"); });

  if (mode == "stdio") {
    std::fprintf(stderr, "ready (stdio)\n");
    std::fflush(stderr);
    return server.serve_stdio(&std::cin, &std::cout);
  }

  // Port 0 lets the OS pick a free port, which keeps parallel runs from
  // colliding; the chosen port is published on stdout before we block.
  server.serve_http("127.0.0.1", port);
  std::fprintf(stderr, "listening on 127.0.0.1:%d\n", server.bound_port());
  std::fflush(stderr);
  return 0;
}
