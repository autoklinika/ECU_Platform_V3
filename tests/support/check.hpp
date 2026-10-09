#pragma once

// Minimal assertion helper shared by V3 tests: counts failures, prints the
// location, and lets main() return a non-zero status.

#include <iostream>

namespace ecu::test {

inline int& failures() {
  static int count = 0;
  return count;
}

inline void check(const bool condition, const char* expression,
                  const char* file, const int line) {
  if (condition) return;
  ++failures();
  std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
}

inline int finish(const char* suite) {
  if (failures() == 0) {
    std::cout << suite << ": PASS\n";
    return 0;
  }
  std::cerr << suite << ": " << failures() << " failure(s)\n";
  return 1;
}

}  // namespace ecu::test

#define CHECK(expr) ::ecu::test::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
