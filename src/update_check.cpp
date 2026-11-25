// Are there updates waiting on GitHub? (update_check.py)
#include "update_check.h"

#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <regex>

#include "core/os.h"
#include "core/util.h"

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
    std::string agent = "User-Agent: Exbit";
    std::string timeout = std::to_string(TIMEOUT_S);
    std::vector<std::string> args = {"curl", "-s", "-L", "--max-time", timeout, "-H",
                                     "Accept: application/vnd.github+json", "-H", agent, "-w", "\n%{http_code}", url};
    // (Windows 10 and later have curl.exe too)
    util::debugf("update check: asking %s\n", url.c_str());
    int fds[2];
    if (!os::makePipe(fds)) throw FetchError(std::string("could not start curl: ") + std::strerror(errno));
    os::Process child;
    int err = 0;
    bool started = os::spawn(args, fds[1], child, &err);
    close(fds[1]);
    if (!started) {
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
    int code = os::waitProcess(child);
    util::debugf("update check: curl exit %d, %zu bytes: %.200s\n", code, out.size(), out.c_str());
    if (code == 127) throw FetchError("curl is not installed (it is needed to check for updates)");
    if (code != 0) throw FetchError(code < 0 ? "curl stopped unexpectedly" : curlProblem(code));
    return answer(out);
}

Json answer(const std::string& out) {
    size_t nl = out.rfind('\n');
    std::string statusText = nl == std::string::npos ? out : out.substr(nl + 1);
    std::string body = nl == std::string::npos ? "" : out.substr(0, nl);
    int http = std::atoi(statusText.c_str());
    if (http < 200 || http >= 300) {
        // (60 requests an hour without an account, counted per network address: every program behind it shares them)
        if ((http == 403 || http == 429) && body.find("rate limit") != std::string::npos)
            throw FetchError("GitHub's limit of 60 requests an hour from this network was reached; trying again later", http);
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

double parseTime(const std::string& iso) {
    int y, mo, d, h, mi, s;
    char rest[16] = "";
    if (std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%15s", &y, &mo, &d, &h, &mi, &s, rest) < 6) return 0;
    struct tm t {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    double at = (double)timegm(&t);
    // (a time zone after it: Z, or ±HH:MM, taken off to give UTC)
    std::string zone = rest;
    if (!zone.empty() && zone[0] == '.') zone = zone.substr(zone.find_first_not_of(".0123456789"));  // (fractions)
    int oh = 0, om = 0;
    if (!zone.empty() && (zone[0] == '+' || zone[0] == '-') && std::sscanf(zone.c_str() + 1, "%2d:%2d", &oh, &om) >= 1)
        at -= (zone[0] == '+' ? 1 : -1) * (oh * 3600.0 + om * 60.0);
    return at;
}

std::string ago(double then, double now) {
    double s = now - then;
    auto n = [](double v, const char* unit) {
        long k = std::max(1L, (long)std::floor(v));
        return std::to_string(k) + " " + unit + (k == 1 ? "" : "s") + " ago";
    };
    if (s < 60) return "just now";
    if (s < 3600) return n(s / 60, "minute");
    if (s < 86400) return n(s / 3600, "hour");
    if (s < 30 * 86400) return n(s / 86400, "day");
    if (s < 365 * 86400) return n(s / (30.44 * 86400), "month");
    return n(s / (365.25 * 86400), "year");
}

std::string builtFrom() {
    static const std::regex full("^[0-9a-f]{40}$");
    std::string commit = version::COMMIT;
    return std::regex_match(commit, full) ? commit : std::string();
}

Result check(const FetchFn& fetchFn) { return check(fetchFn, builtFrom()); }

namespace {
// the main branch's newest commit into r (left as it is when GitHub can't say)
void addLatest(Result& r, const FetchFn& fetchFn) {
    try {
        Json c = fetchFn(API + "/commits/main");
        if (!c.isObject() || !c["sha"].isString()) return;
        r.latest = c["sha"].toString();
        const Json& when = c["commit"]["committer"]["date"];
        if (when.isString()) r.latestTime = parseTime(when.toString());
        r.latestUrl = PAGE + "/commit/" + r.latest;
    } catch (const std::exception&) {
    }
}
Result compare(const FetchFn& fetchFn, const std::string& commit);
}  // namespace

Result check(const FetchFn& fetchFn, const std::string& commit) {
    FetchFn fn = fetchFn ? fetchFn : FetchFn(fetch);
    Result r = compare(fn, commit);
    if (r.state != "failed") addLatest(r, fn);
    return r;
}

namespace {
Result compare(const FetchFn& fetchFn, const std::string& commit) {
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
}  // namespace

}  // namespace update_check
