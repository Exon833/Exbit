// The logs of Windows plugin installs (plugins/install_log.h): their names, the newest few kept, their
// last lines, a long one cut to its end.
#include <unistd.h>

#include <ctime>

#include <cstdio>
#include <string>

#include "check.h"
#include "core/util.h"
#include "paths.h"
#include "plugins/install_log.h"

namespace {

// a log's name: the local time (here UTC: main sets TZ) and the installer's name without its folder and extension
void testNames() {
    long long when = 1795711267;  // 2026-11-26 16:41:07 UTC
    std::string name = util::baseName(install_log::newLogPath("/logs", "/home/me/Desktop/Kontakt 8 Setup PC.exe", when));
    CHECK(name == "2026-11-26_16-41-07 Kontakt 8 Setup PC.log");
    CHECK(util::dirName(install_log::newLogPath("/logs", "x.msi", when)) == "/logs");
    // characters a file name can't have (on Windows either) become '_'
    name = util::baseName(install_log::newLogPath("/logs", "/a/b:c*d?\"e<f>g|h.exe", when));
    CHECK(name == "2026-11-26_16-41-07 b_c_d__e_f_g_h.log");
}

// all but the newest (by name: they begin with the time) are deleted; other files are left
void testRotation(const std::string& dir) {
    for (int day = 10; day <= 24; day++)
        util::writeFile(util::joinPath(dir, util::format("2026-11-%02d_10-00-00 Setup.log", day)), "x\n");
    util::writeFile(util::joinPath(dir, "notes.txt"), "mine");
    CHECK(install_log::rotate(dir, 10) == 5);
    int logs = 0;
    for (const auto& entry : util::listDir(dir))
        if (util::extension(entry.name) == ".log") logs++;
    CHECK(logs == 10);
    CHECK(!util::fileExists(util::joinPath(dir, "2026-11-14_10-00-00 Setup.log")));
    CHECK(util::fileExists(util::joinPath(dir, "2026-11-15_10-00-00 Setup.log")));
    CHECK(util::fileExists(util::joinPath(dir, "2026-11-24_10-00-00 Setup.log")));
    CHECK(util::fileExists(util::joinPath(dir, "notes.txt")));
    CHECK(install_log::rotate(dir, 10) == 0);
    CHECK(install_log::rotate(util::joinPath(dir, "missing"), 10) == 0);
}

void testLastLines(const std::string& dir) {
    std::string file = util::joinPath(dir, "tail.log");
    util::writeFile(file, "one\ntwo\nthree\nfour\n\n\n");
    CHECK(install_log::lastLines(file, 2) == "three\nfour");
    CHECK(install_log::lastLines(file, 10) == "one\ntwo\nthree\nfour");
    util::writeFile(file, "no end of line");
    CHECK(install_log::lastLines(file, 3) == "no end of line");
    CHECK(install_log::lastLines(util::joinPath(dir, "missing.log"), 3).empty());
    util::writeFile(file, "");
    CHECK(install_log::lastLines(file, 3).empty());
}

// a long log keeps its end (where an install's failure is), after a line saying it was cut
void testLimit(const std::string& dir) {
    std::string file = util::joinPath(dir, "long.log"), text;
    for (int line = 0; line < 2000; line++) text += util::format("line %04d\n", line);
    util::writeFile(file, text);
    install_log::limitSize(file, 1000);
    std::string cut;
    util::readFile(file, cut);
    CHECK(cut.size() < 1200);
    CHECK(cut.find("line 1999\n") != std::string::npos);
    CHECK(cut.find("line 0000") == std::string::npos);
    CHECK(util::startsWith(cut, "(") && cut.find("cut") != std::string::npos);
    std::string small = util::joinPath(dir, "small.log");
    util::writeFile(small, "short\n");
    install_log::limitSize(small, 1000);
    util::readFile(small, cut);
    CHECK(cut == "short\n");
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/exbit-install-log-XXXXXX";
    std::string root = mkdtemp(tmpl);
    setenv("TZ", "UTC", 1);
    tzset();
    paths::setDataDir(root);
    testNames();
    std::string folder = install_log::folder();
    CHECK(folder == util::joinPath(root, "install-logs") && util::isDir(folder));
    testRotation(folder);
    testLastLines(root);
    testLimit(root);
    std::system(("rm -rf '" + root + "'").c_str());
    TEST_MAIN_END();
}
