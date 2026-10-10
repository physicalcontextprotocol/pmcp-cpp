// pcp-cpp — minimal HTTP/1.1 server (internal, POSIX sockets).
//
// Deliberately matches pcp-python/pcp::_run_http rather than a general
// framework: one request per connection, Connection: close, JSON-RPC errors
// still return HTTP 200, and only a parse failure returns 400. No chunked
// transfer-encoding support, no keep-alive — the reference server has neither,
// and clients in this repo do not use either.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace pmcp {

struct HttpResponse {
  int status = 200;
  std::string body;
  std::string content_type = "application/json";
  bool allow_origin = true;
};

class HttpServer {
 public:
  using Handler = std::function<HttpResponse(const std::string& path, const std::string& body)>;

  void set_handler(Handler h) { handler_ = std::move(h); }
  // Paths to route. Anything not listed gets 404. The reference server ignores
  // the path entirely; we route but default to "/" being accepted too, because
  // pcp-conformance's own rpc_call posts to the bare base URL.
  void set_paths(std::vector<std::string> p) { paths_ = std::move(p); }

  int listen(const std::string& host, int port, int* bound_port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      std::cerr << "[pmcp] socket() failed: " << std::strerror(errno) << "\n";
      return 1;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
      std::cerr << "[pmcp] bad host: " << host << "\n";
      ::close(fd);
      return 1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      std::cerr << "[pmcp] bind " << host << ":" << port << " failed: " << std::strerror(errno)
                << "\n";
      ::close(fd);
      return 1;
    }
    if (::listen(fd, 16) < 0) {
      std::cerr << "[pmcp] listen failed: " << std::strerror(errno) << "\n";
      ::close(fd);
      return 1;
    }
    if (bound_port) {
      sockaddr_in actual{};
      socklen_t len = sizeof(actual);
      if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) == 0) {
        *bound_port = ntohs(actual.sin_port);
      }
    }
    std::cerr << "[pmcp] listening on http://" << host << ":" << (bound_port ? *bound_port : port)
              << "\n";

    for (;;) {
      int c = ::accept(fd, nullptr, nullptr);
      if (c < 0) {
        if (errno == EINTR) continue;
        break;
      }
      serve_one(c);
      ::close(c);
    }
    ::close(fd);
    return 0;
  }

 private:
  void serve_one(int cfd) {
    std::string data;
    char buf[4096];
    ssize_t n;
    size_t header_end = std::string::npos;
    size_t content_length = 0;

    // Read headers.
    while ((n = ::recv(cfd, buf, sizeof(buf), 0)) > 0) {
      data.append(buf, static_cast<std::size_t>(n));
      header_end = data.find("\r\n\r\n");
      if (header_end != std::string::npos) break;
      if (data.size() > 64 * 1024) break;
    }
    if (header_end == std::string::npos) return;

    const std::string head = data.substr(0, header_end);
    std::string body = data.substr(header_end + 4);

    // Method and path. The reference server skips the request line when
    // scanning headers; we parse it properly.
    size_t sp1 = head.find(' ');
    if (sp1 == std::string::npos) return;
    size_t sp2 = head.find(' ', sp1 + 1);
    const std::string method = head.substr(0, sp1);
    const std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string path = target.substr(0, target.find('?'));

    // Case-insensitive Content-Length, per RFC.
    std::string lower = head;
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    auto cl = lower.find("content-length:");
    if (cl != std::string::npos) {
      content_length = static_cast<size_t>(std::strtoul(head.c_str() + cl + 15, nullptr, 10));
    }

    while (body.size() < content_length && (n = ::recv(cfd, buf, sizeof(buf), 0)) > 0) {
      body.append(buf, static_cast<std::size_t>(n));
    }

    if (method == "OPTIONS") {
      send_response(cfd, 204, "", "text/plain", false,
                    "POST, OPTIONS\r\nContent-Type");
      return;
    }
    if (method != "POST") {
      send_response(cfd, 405, "{\"error\":\"method not allowed\"}", "application/json", true,
                    nullptr);
      return;
    }

    bool routed = false;
    for (const auto& p : paths_) {
      if (p == path) { routed = true; break; }
    }
    if (!routed) {
      send_response(cfd, 404, "{\"error\":\"not found\"}", "application/json", true, nullptr);
      return;
    }
    if (!handler_) return;

    HttpResponse r = handler_(path, body);
    send_response(cfd, r.status, r.body, r.content_type, r.allow_origin, nullptr);
  }

  static void send_response(int cfd, int status, const std::string& body,
                            const std::string& content_type, bool allow_origin,
                            const char* extra_headers) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
    out += "Content-Type: " + content_type + "\r\n";
    if (allow_origin) out += "Access-Control-Allow-Origin: *\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Connection: close\r\n";
    if (extra_headers) out += std::string(extra_headers) + "\r\n";
    out += "\r\n";
    out += body;

    size_t sent = 0;
    while (sent < out.size()) {
      ssize_t n = ::send(cfd, out.data() + sent, out.size() - sent, 0);
      if (n <= 0) break;
      sent += static_cast<size_t>(n);
    }
  }

  static const char* reason(int s) {
    switch (s) {
      case 200: return "OK";
      case 204: return "No Content";
      case 400: return "Bad Request";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      default: return "OK";
    }
  }

  Handler handler_;
  std::vector<std::string> paths_{"/pcp", "/mcp", "/"};
};

}  // namespace pmcp