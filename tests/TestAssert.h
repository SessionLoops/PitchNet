#pragma once

#include <cstdio>
#include <cstdlib>

// A check that survives NDEBUG.
//
// assert() compiles to nothing when NDEBUG is defined, which every release
// configuration does - so a test written with bare assert() reports success
// without having checked anything. Undefining NDEBUG for the test targets is
// not a fix either: it changes the compile flags for every JUCE source the
// target pulls in, forcing a full rebuild of JUCE for each test.
//
// CHECK() is an ordinary if, so it behaves identically in every configuration.
// On failure it prints the file, line and expression, then exits non-zero,
// which is what CTest reports as a failure.
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "FAILED: %s\n  at %s:%d\n", #condition, __FILE__,   \
                   __LINE__);                                                  \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)
