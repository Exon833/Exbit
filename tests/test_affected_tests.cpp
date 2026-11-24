// tools/affected_tests.sh: which test programs a change reaches, from the compiler's .d files (a fixture tree here).
// A test is reached when it includes a changed header, or a header whose .cpp uses one (its code, linked in).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "check.h"

namespace fs = std::filesystem;

static void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

// the script's answer for these changed files, one test per line, sorted
static std::string affected(const fs::path& root, const std::string& files) {
    std::string cmd = "AFFECTED_ROOT='" + root.string() + "' sh tools/affected_tests.sh -- " + files + " 2>&1 | sort";
    std::string out;
    if (FILE* f = popen(cmd.c_str(), "r")) {
        char buf[256];
        while (fgets(buf, sizeof buf, f)) out += buf;
        pclose(f);
    }
    return out;
}

int main() {
    fs::path root = fs::temp_directory_path() / ("tinydaw-affected-" + std::to_string(::getpid()));
    fs::remove_all(root);
    // x.cpp is used by y.cpp; test_p includes y.h only; test_q includes z.h only
    put(root / "build/src/a/x.d", "build/src/a/x.o: src/a/x.cpp src/a/x.h\n");
    put(root / "build/src/b/y.d", "build/src/b/y.o: src/b/y.cpp src/b/y.h \\\n src/a/x.h\n");
    put(root / "build/src/c/z.d", "build/src/c/z.o: src/c/z.cpp src/c/z.h\n");
    put(root / "build/tests/test_p.d", "build/tests/test_p: tests/test_p.cpp tests/check.h src/b/y.h\n");
    put(root / "build/tests/test_q.d", "build/tests/test_q: tests/test_q.cpp tests/check.h src/c/z.h\n");
    const std::string P = "build/tests/test_p\n", Q = "build/tests/test_q\n";

    CHECK(affected(root, "src/a/x.cpp") == P);              // through y.cpp, which includes x.h
    CHECK(affected(root, "src/a/x.h") == P);
    CHECK(affected(root, "src/c/z.cpp") == Q);              // its own header
    CHECK(affected(root, "tests/test_q.cpp") == Q);         // the test itself
    CHECK(affected(root, "tests/check.h") == P + Q);        // a header every test includes
    CHECK(affected(root, "src/a/x.cpp src/c/z.h") == P + Q);
    CHECK(affected(root, "docs/notes.md README.md") == ""); // nothing that is built
    CHECK(affected(root, "Makefile") == P + Q);             // the build itself: every test (failsafe)
    CHECK(affected(root, "data/skins/dark.css") == P + Q);  // shipped data: every test (failsafe)
    CHECK(affected(root, "src/new_file.cpp") == "");        // in no .d yet: nothing includes it
    // no .d files (nothing built yet): the tests can't be named, so the script says so
    fs::remove_all(root / "build/tests");
    CHECK(affected(root, "src/a/x.cpp").find("no .d files") != std::string::npos);

    fs::remove_all(root);
    TEST_MAIN_END();
}
