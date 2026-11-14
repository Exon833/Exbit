// Are there updates waiting on GitHub? (update_check.py) No GUI; the GUI runs check() in a
// background thread (ui/update_notice.h UpdateChecker).
//
// Tiny DAW is a rolling release: there are no versions, the repository's main branch
// (ExonSounds/Tiny-DAW-Linux) is the program. A build knows the commit it was built from
// (version::COMMIT); if the main branch has moved on since, the notice under the master row says
// how many updates there are to pull and build. A build that doesn't know its commit (or whose
// commit GitHub doesn't know: commits of its own) can't tell: "Updates off here". Nothing is
// downloaded or installed here.
//
// GitHub is asked with the system's own `curl` (run as a child process; no TLS code of ours).
// (parse / isNewer / latest are from the numbered releases, kept for whoever still uses them.)
#pragma once
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/json.h"
#include "version.h"

namespace update_check {

extern const std::string API;   // "https://api.github.com/repos/" + REPOSITORY
extern const std::string PAGE;  // "https://github.com/" + REPOSITORY
constexpr int TIMEOUT_S = 8;

// "v0.2.1" -> {0, 2, 1}; nullopt if it isn't a version number
std::optional<std::vector<int>> parse(const std::string& version);
bool isNewer(const std::string& candidate, const std::string& current = version::VERSION);

// Why GitHub couldn't be asked, or answered strangely (Python's OSError / ValueError / HTTPError).
struct FetchError : std::runtime_error {
    int status;  // the HTTP status (404…), 0 when there was no answer
    FetchError(const std::string& what, int status_ = 0) : std::runtime_error(what), status(status_) {}
};
// GET url (GitHub's JSON API) -> the parsed answer; throws FetchError
using FetchFn = std::function<Json(const std::string& url)>;
Json fetch(const std::string& url);  // with curl, TIMEOUT_S at most

// (newest version, its page) published on GitHub; version "" (and PAGE) if there is none yet.
// Throws FetchError when GitHub can't be reached or answers strangely.
void latest(std::string& version, std::string& url, const FetchFn& fetchFn = fetch);

struct Result {
    std::string state;  // "waiting" | "current" | "off" (this build can't tell) | "failed"
    std::string value;  // "waiting": how many updates (also in `count`); "failed": why
    std::string url;    // the page to link to
    int count = 0;      // "waiting": how many commits the main branch is ahead of this build
};
// the commit this build came from (version::COMMIT) if it is a full commit id, else ""
std::string builtFrom();
// ("waiting", N) | ("current") | ("off"): no commit to compare | ("failed", reason). Blocks for up
// to a few seconds: not on the GUI thread. commit: the build's (tests give another).
Result check(const FetchFn& fetchFn = fetch);
Result check(const FetchFn& fetchFn, const std::string& commit);

}  // namespace update_check
