// Minimal test harness. No gtest/catch2 dependency — pcp-cpp has none, and
// pulling one in for a conformance-focused SDK would be gratuitous.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace pmcp::test {

struct Case {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failures() {
  static int f = 0;
  return f;
}

inline std::string& current() {
  static std::string c;
  return c;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
  }
};

inline void fail(const std::string& msg, const char* file, int line) {
  ++failures();
  std::cerr << "  FAIL " << current() << "\n    " << msg << "\n    at " << file << ":" << line
            << "\n";
}

inline int run_all() {
  int failed_cases = 0;
  for (auto& c : registry()) {
    current() = c.name;
    const int before = failures();
    // Announce before running, not after: if a case deadlocks or hangs, this
    // line is the only clue about where.
    std::cout << "  ..   " << c.name << std::endl;
    try {
      c.fn();
    } catch (const std::exception& e) {
      fail(std::string("uncaught exception: ") + e.what(), __FILE__, __LINE__);
    } catch (...) {
      fail("uncaught non-std exception", __FILE__, __LINE__);
    }
    const bool ok = failures() == before;
    if (!ok) ++failed_cases;
    std::cout << (ok ? "  ok   " : "  FAIL ") << c.name << std::endl;
  }
  std::cout << "\n" << (registry().size() - failed_cases) << "/" << registry().size()
            << " passed" << std::endl;
  return failed_cases == 0 ? 0 : 1;
}

}  // namespace pmcp::test

#define PMCP_TEST(name)                                                     \
  static void name();                                                       \
  static ::pmcp::test::Registrar pmcp_reg_##name(#name, name);              \
  static void name()

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) ::pmcp::test::fail("expected: " #cond, __FILE__, __LINE__); \
  } while (0)

// Both operands are copied, not bound by reference. An expression like
// `f(x)["k"]` returns a reference into a temporary json; binding `auto&&` to it
// leaves a dangling reference once the full expression ends, which would make
// these macros report phantom failures.
#define CHECK_EQ(a, b)                                                      \
  do {                                                                      \
    auto va_ = (a);                                                         \
    auto vb_ = (b);                                                         \
    if (!(va_ == vb_)) {                                                    \
      ::pmcp::test::fail("expected " #a " == " #b, __FILE__, __LINE__);     \
    }                                                                       \
  } while (0)

#define CHECK_NE(a, b)                                                      \
  do {                                                                      \
    auto va_ = (a);                                                         \
    auto vb_ = (b);                                                         \
    if (va_ == vb_) ::pmcp::test::fail("expected " #a " != " #b, __FILE__, __LINE__); \
  } while (0)

#define PMCP_TEST_MAIN()                                                    \
  int main() { return ::pmcp::test::run_all(); }
