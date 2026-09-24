#pragma once

// Test assertions must fail through stderr and an exit code. The MSVC CRT
// assert dialog blocks unattended CTest runs and hides the failure from logs.
#include <cstdio>
#include <cstdlib>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                         #condition);                                          \
            std::fflush(stderr);                                               \
            std::exit(2);                                                      \
        }                                                                      \
    } while (false)
