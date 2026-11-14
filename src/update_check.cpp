// Are there updates waiting on GitHub? (update_check.py)
#include "update_check.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <regex>

extern char** environ;

namespace update_check {

const std::string API = std::string("https://api.github.com/repos/") + version::REPOSITORY;
const std::string PAGE = std::string("https://github.com/") + version::REPOSITORY;

std::optional<std::vector<int>> parse(const std::string& version) {
    static const std::regex shape(R"(^\s*v?\d+(\.\d+)*\s*$)");
    if (!std::regex_match(version, shape)) return std::nullopt;
    std::vector<int> numbers;
    static const std::regex digits(R"(\d+)");
    for (auto it = std::sregex_iterator(version.begin(), version.end(), digits); it != std::sregex_iterator(); ++it) {
        try {
            numbers.push_back(std::stoi(it->str()));
        } catch (...) {
            return std::nullopt;  // (a number too big for an int)
        }
    }
    return numbers;
}

bool isNewer(const std::string& candidate, const std::string& current) {
    auto a = parse(candidate), b = parse(current);
    if (!a || !b) return false;
    size_t width = std::max(a->size(), b->size());
    a->resize(width, 0);
    b->resize(width, 0);
    return *a > *b;
}

namespace {

std::string reasonPhrase(int status) {
    switch (status) {
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
    }
}

std::string curlProblem(int code) {
    switch (code) {
    case 5: return "could not resolve the proxy";
    case 6: return "could not resolve api.github.com (no network?)";
    case 7: return "could not connect to api.github.com";
    case 28: return "GitHub did not answer in time";
    case 35: return "the secure connection to GitHub failed";
    case 52: return "GitHub sent no answer";
    case 56: return "the connection to GitHub broke off";
    case 60: return "GitHub's certificate could not be checked";
    default: return "curl failed (exit code " + std::to_string(code) + ")";
    }
}

}  // namespace

Json fetch(const std::string& url) {
    // curl -s -L --max-time 8 -H … -w "\n%{http_code}" url: the body, then the status on a line of its own
    std::string agent = "User-Agent: Tiny-DAW-Linux";
    std::string timeout = std::to_string(TIMEOUT_S);
    std::vector<std::string> args = {"curl", "-s", "-L", "--max-time", timeout, "-H",
                                     "Accept: application/vnd.github+json", "-H", agent, "-w", "\n%{http_code}", url};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) throw FetchError(std::string("could not start curl: ") + std::strerror(errno));
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    int err = posix_spawnp(&pid, "curl", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (err != 0) {
        close(fds[0]);
        throw FetchError(err == ENOENT ? "curl is not installed (it is needed to check for updates)"
                                       : std::string("could not start curl: ") + std::strerror(err));
    }
    std::string out;
    char buf[8192];
    while (true) {
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n > 0) out.append(buf, (size_t)n);
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 127) throw FetchError("curl is not installed (it is needed to check for updates)");
    if (code != 0) throw FetchError(code < 0 ? "curl stopped unexpectedly" : curlProblem(code));

    size_t nl = out.rfind('\n');
    std::string statusText = nl == std::string::npos ? out : out.substr(nl + 1);
    std::string body = nl == std::string::npos ? "" : out.substr(0, nl);
    int http = std::atoi(statusText.c_str());
    if (http < 200 || http >= 300) {
        std::string reason = reasonPhrase(http);
        throw FetchError("HTTP Error " + std::to_string(http) + (reason.empty() ? "" : ": " + reason), http);
    }
    std::string error;
    Json answer = Json::parse(body, &error);
    if (!error.empty()) throw FetchError("GitHub's answer could not be read: " + error);
    return answer;
}

namespace {
std::string lstripV(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == 'v') ++i;
    return s.substr(i);
}
}  // namespace

void latest(std::string& versionOut, std::string& urlOut, const FetchFn& fetchFn) {
    try {
        Json release = fetchFn(API + "/releases/latest");
        if (!release.isObject()) throw FetchError("unexpected answer from GitHub");
        const Json& tag = release["tag_name"];
        if (tag.isString() && parse(tag.toString())) {
            versionOut = lstripV(tag.toString());
            std::string page = release["html_url"].toString();
            urlOut = page.empty() ? PAGE + "/releases" : page;
            return;
        }
    } catch (const FetchError& e) {
        if (e.status != 404) throw;  // 404: no releases yet, look at the tags
    }
    Json tags = fetchFn(API + "/tags");
    if (!tags.isArray()) throw FetchError("unexpected answer from GitHub");
    std::string newest;
    std::vector<int> best;
    for (const Json& t : tags.items()) {
        if (!t.isObject()) throw FetchError("unexpected answer from GitHub");
        const Json& name = t["name"];
        if (!name.isString()) continue;
        auto v = parse(name.toString());
        if (!v) continue;
        if (newest.empty() || *v > best) {  // (the first of equal ones, as max())
            newest = name.toString();
            best = *v;
        }
    }
    if (newest.empty()) {
        versionOut.clear();
        urlOut = PAGE;
        return;
    }
    versionOut = lstripV(newest);
    urlOut = PAGE + "/releases/tag/" + newest;
}

std::string builtFrom() {
    static const std::regex full("^[0-9a-f]{40}$");
    std::string commit = version::COMMIT;
    return std::regex_match(commit, full) ? commit : std::string();
}

Result check(const FetchFn& fetchFn) { return check(fetchFn, builtFrom()); }

Result check(const FetchFn& fetchFn, const std::string& commit) {
    if (commit.empty()) return {"off", "", PAGE, 0};
    Json compared;
    try {
        compared = (fetchFn ? fetchFn : FetchFn(fetch))(API + "/compare/" + commit + "...main");
    } catch (const FetchError& e) {
        if (e.status == 404 || e.status == 422) return {"off", "", PAGE, 0};  // (a commit GitHub doesn't know)
        std::string why = e.what();
        return {"failed", why.empty() ? "error" : why, PAGE, 0};
    } catch (const std::exception& e) {
        std::string why = e.what();
        return {"failed", why.empty() ? "error" : why, PAGE, 0};
    }
    if (!compared.isObject()) return {"failed", "GitHub answered strangely", PAGE, 0};
    std::string status = compared["status"].isString() ? compared["status"].toString() : "";
    int count = 0;
    if (status == "ahead" || status == "diverged") {
        const Json& ahead = compared["ahead_by"];
        if (!ahead.isNumber() && !ahead.isNull()) return {"failed", "GitHub answered strangely", PAGE, 0};
        count = ahead.toInt(0);
    }
    if (count > 0) return {"waiting", std::to_string(count), PAGE + "/commits/main", count};
    return {"current", "", PAGE, 0};
}

}  // namespace update_check
