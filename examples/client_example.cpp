// End-to-end client example.
//
//   ./pmcp_example_client http://127.0.0.1:8080 /pcp v05
//   ./pmcp_example_client                                  # in-process demo
//
// The dialect argument picks the wire spelling; nothing else changes. Try
// running the same script against the simulated arm with each of v05, python
// and conformance and watch the method names change while the calls stay put.
#include <cstdio>
#include <iostream>
#include <string>

#include "pmcp/client.hpp"
#include "pmcp/dialect.hpp"
#include "pmcp/server.hpp"

namespace {

void show(const std::string& label, const pmcp::json& value) {
  std::cout << label << ": " << pmcp::dump_indent_ascii(value) << "\n\n";
}

pmcp::Dialect parse(const std::string& s) {
  if (s == "python") return pmcp::Dialect::kPython;
  if (s == "v05") return pmcp::Dialect::kV05;
  if (s == "conformance") return pmcp::Dialect::kConformance;
  if (s == "spec") return pmcp::Dialect::kSpec;
  return pmcp::Dialect::kV05;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cout << "usage: " << argv[0] << " [base-url [path [dialect]]]\n"
              << "  with no base-url, runs an in-process demo against a local Server\n\n"
              << "  dialects: v05 | python | conformance | spec\n";
  }

  pmcp::Dialect dialect = parse(argc > 3 ? argv[3] : "v05");
  pmcp::Client::Config cfg;
  cfg.dialect = dialect;
  cfg.name = "example-client";

  pmcp::Client client(cfg);

  // ---- in-process demo -----------------------------------------------------
  if (argc < 2) {
    pmcp::ServerConfig scfg;
    scfg.name = "demo-arm";
    scfg.robot_id = "demo";
    pmcp::Server server(scfg);

    pmcp::ActuationSpec move;
    move.name = "move_to";
    move.parameters = {{"x", "number", "x"}, {"y", "number", "y"}, {"z", "number", "z"}};
    server.register_actuation(move, [](const pmcp::json& a) {
      pmcp::ActuationOutcome r;
      r.output = a;
      r.energy_j = 9.0;
      return r;
    });
    client.connect_inprocess(&server);
  } else {
    const std::string url = argv[1];
    const std::string path = argc > 2 ? argv[2] : "";
    try {
      client.connect_http(url, path);
    } catch (const pmcp::Error& e) {
      std::cerr << "connect failed: " << e.str() << "\n";
      return 1;
    }
  }

  try {
    show("initialize", client.initialize());
    show("tools/list", client.list_actuations());
    show("shadow/preview", client.shadow_preview("move_to", {{"x", 0.2}, {"y", 0.1}, {"z", 0.3}}));

    // safe_actuation is the full pipeline: preview -> lease -> call -> release.
    show("safe_actuation", client.safe_actuation("move_to", {{"x", 0.2}, {"y", 0.1}, {"z", 0.3}},
                                                  "example-zone"));

    show("metrics", client.metrics());

    // Every actuation while stopped is refused. Worth demonstrating once.
    client.estop(true);
    try {
      client.call_actuation("move_to", {{"x", 0.2}, {"y", 0.1}, {"z", 0.3}});
      std::cout << "actuation unexpectedly succeeded while e-stopped\n";
    } catch (const pmcp::Error& e) {
      std::cout << "actuation correctly refused while e-stopped: " << e.str() << "\n";
    }
    client.estop_reset();
  } catch (const pmcp::Error& e) {
    std::cerr << "call failed: " << e.str() << "\n";
    client.disconnect();
    return 1;
  }

  client.disconnect();
  return 0;
}
