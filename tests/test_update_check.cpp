// The update check's own parts, without asking GitHub (its rate limit is shared with the real program):
// curl's output read back, the rate limit said plainly, tests opted out, and curl found where the program runs.
#include <cstdlib>
#include <string>

#include "check.h"
#include "core/os.h"
#include "update_check.h"

int main() {
    using update_check::FetchError;
    // the body, then the status on a line of its own
    Json j = update_check::answer("{\"sha\": \"abc\"}\n200");
    CHECK(j.isObject() && j["sha"].toString() == "abc");
    // GitHub's 60 questions an hour, spent: said as such, the status kept
    try {
        update_check::answer("{\"message\":\"API rate limit exceeded for 0.0.0.0. (But here's the good news: ...)\"}\n403");
        CHECK(false);
    } catch (const FetchError& e) {
        CHECK(e.status == 403 && std::string(e.what()).find("limit of 60") != std::string::npos);
    }
    // another refusal: the HTTP error
    try {
        update_check::answer("{\"message\":\"Forbidden\"}\n403");
        CHECK(false);
    } catch (const FetchError& e) {
        CHECK(e.status == 403 && std::string(e.what()) == "HTTP Error 403: Forbidden");
    }
    // nothing at all (curl wrote no status): an error, not a crash
    try {
        update_check::answer("");
        CHECK(false);
    } catch (const FetchError& e) {
        CHECK(e.status == 0);
    }
    // every test (check.h) leaves GitHub alone
    CHECK(check::noUpdateCheck && getenv("EXBIT_NO_UPDATE_CHECK"));
    // the system's curl starts from the program (Windows 10 and later: System32\curl.exe)
    os::Process p;
    int err = 0;
    bool started = os::spawn({"curl", "--version"}, -1, p, &err);
    if (!started) fprintf(stderr, "curl did not start (errno %d): the update check can't run here\n", err);
    CHECK(started && os::waitProcess(p) == 0);
    TEST_MAIN_END();
}
