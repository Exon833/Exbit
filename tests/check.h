// A tiny test harness: each tests/*.cpp is a program; CHECK failures are counted and printed,
// and main returns non-zero if any failed.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/program_environment.h"

namespace check {
inline int& failures() { static int n = 0; return n; }
inline int& passes() { static int n = 0; return n; }
inline const char* testDir() {  // the test plugins' folder (make test sets it)
    const char* d = environment::readProgramVariable("TEST_PLUGINS");
    return d ? d : "build/test-plugins";
}
// Tests leave GitHub alone: each main window would ask it for updates, and its 60 questions an hour per network
// address are shared with the real program (spent, the program says "Couldn't check")
inline const bool noUpdateCheck = setenv("EXBIT_NO_UPDATE_CHECK", "1", 0) == 0;
}  // namespace check

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) {                                                                 \
            check::passes()++;                                                      \
        } else {                                                                    \
            check::failures()++;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        double _a = (a), _b = (b);                                                                     \
        if (std::fabs(_a - _b) <= (tol)) {                                                             \
            check::passes()++;                                                                         \
        } else {                                                                                       \
            check::failures()++;                                                                       \
            fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %g, %s = %g\n", __FILE__, __LINE__, #a, _a, #b, _b); \
        }                                                                                              \
    } while (0)

#define TEST_MAIN_END()                                                                   \
    do {                                                                                  \
        printf("%d passed, %d failed\n", check::passes(), check::failures());              \
        return check::failures() ? 1 : 0;                                                 \
    } while (0)
