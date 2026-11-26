// The logs of Windows plugin installs (see install_log.h).
#include "plugins/install_log.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include <unistd.h>

#include "core/util.h"
#include "paths.h"

namespace install_log {

std::string folder() { return paths::makeDataDir("install-logs"); }

std::string newLogPath(const std::string& dir, const std::string& installer, long long unixSeconds) {
    std::time_t when = (std::time_t)unixSeconds;
    std::tm local{};
    localtime_r(&when, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &local);
    std::string name = util::stem(installer);
    for (char& c : name)
        if (std::string("/\\:*?\"<>|").find(c) != std::string::npos || (unsigned char)c < 32)
            c = '_';
    return util::joinPath(dir, std::string(stamp) + " " + name + ".log");
}

int rotate(const std::string& dir, int keep) {
    std::vector<std::string> logs;
    for (const auto& entry : util::listDir(dir))
        if (util::extension(entry.name) == ".log")
            logs.push_back(entry.name);
    std::sort(logs.begin(), logs.end());  // (their names begin with the time)
    int deleted = 0;
    for (size_t i = 0; i + keep < logs.size(); i++)
        if (util::removeFile(util::joinPath(dir, logs[i])))
            deleted++;
    return deleted;
}

std::string lastLines(const std::string& file, int lines) {
    std::string text;
    if (!util::readFile(file, text))
        return "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    size_t start = text.size();
    for (int found = 0; found < lines && start > 0; found++) {
        size_t newline = text.rfind('\n', start - 1);
        if (newline == std::string::npos)
            return text;
        start = newline;
    }
    return start < text.size() && text[start] == '\n' ? text.substr(start + 1) : text.substr(start);
}

// (cut in place, not written anew and renamed: the installer may still be writing to it, at its end)
void limitSize(const std::string& file, long long largest) {
    if (util::fileSize(file) <= largest)
        return;
    std::string text;
    if (!util::readFile(file, text) || (long long)text.size() <= largest)
        return;
    std::string end = text.substr(text.size() - (size_t)largest);
    size_t lineStart = end.find('\n');  // (from a whole line)
    if (lineStart != std::string::npos)
        end = end.substr(lineStart + 1);
    end = util::format("(the log was cut to its last %lld bytes)\n", largest) + end;
    if (FILE* log = std::fopen(file.c_str(), "r+")) {
        std::fwrite(end.data(), 1, end.size(), log);
        std::fflush(log);
        if (ftruncate(fileno(log), (off_t)end.size()) != 0)
            util::debugf("install log: can't shorten %s\n", file.c_str());
        std::fclose(log);
    }
}

}  // namespace install_log
