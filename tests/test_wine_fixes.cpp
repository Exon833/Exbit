// Exbit's Wine fixes (plugins/wine_prefix_fixes.h, src/winefix) and what an install needs around them
// (plugins/wine.h's startInstaller with a log, plugins/installer_activity.h), with Wine: a prefix made as
// Exbit makes one; Wine's own msi.dll and dxgi.dll tried with the test programs of tests/winefix (an .msi
// changed as Kontakt 8's setup changes its own; WaitForVBlank, which JUCE 8 programs wait on); the fixes
// put in, checked, put in again, brought up to date, taken out; a broken stand-in taken out again by the
// check; a DLL that isn't Wine's (DXVK's) left alone; the prefix's activity looked at. Without Wine (or
// EXBIT_NO_WINE=1) only the parts that need none.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "check.h"
#include "core/program_environment.h"
#include "core/util.h"
#include "plugins/installer_activity.h"
#include "plugins/wine.h"
#include "plugins/wine_prefix_fixes.h"

namespace {

std::string tempRoot;

double secondsNow() { return util::wallClock(); }

// waits for a child started by plugins/wine.h (up to `seconds`); its exit code (-2: still running)
int waitFor(int pid, double seconds) {
    int code = -2;
    double start = secondsNow();
    while (pid > 0 && !wine::finished(pid, code) && secondsNow() - start < seconds) usleep(50000);
    if (pid > 0 && code == -2) wine::stop(pid);
    return code;
}

// a prefix made as Options → Wine prefixes → New prefix… makes one ("" if Wine couldn't)
std::string makePrefix() {
    std::string dir = util::joinPath(tempRoot, "prefixes/Plugins"), why, wine = wine::winePath();
    setenv("WINEDLLOVERRIDES", "mscoree,mshtml=", 1);  // (no Mono or Gecko asked for: a test)
    int code = waitFor(wine::startPrefix(dir, wine, why), 240);
    waitFor(wine::startServerWait(dir, wine, why), 120);
    unsetenv("WINEDLLOVERRIDES");
    CHECK(code == 0 && wine::isPrefix(dir));
    return wine::isPrefix(dir) ? dir : "";
}

// runs a Windows program in the prefix as Install Windows plugin… runs an installer, its output to `log`;
// its exit code
int runAsInstaller(const std::string& prefix, const std::string& program, const std::string& log) {
    std::string why;
    util::removeFile(log);
    int pid = wine::startInstaller(prefix, program, why, log);
    if (pid < 0) printf("  can't start %s: %s\n", program.c_str(), why.c_str());
    return waitFor(pid, 600);
}

std::string fileText(const std::string& path) {
    std::string text;
    util::readFile(path, text);
    return text;
}

// a DLL of Wine's own: its placeholder (older Wines) or its builtin DLL itself (Wine 11 copies those in)
bool winesOwn(const std::string& kind) { return kind == "placeholder" || kind == "builtin"; }

// the prefix's DLL override for `dll` ("msi"), asked of Wine ("" none)
std::string overrideOf(const std::string& prefix, const std::string& dll) {
    std::string output;
    if (wine::runInPrefix(prefix, {"reg", "query", "HKCU\\Software\\Wine\\DllOverrides", "/v", dll}, 60, &output) != 0)
        return "";
    size_t at = output.find("REG_SZ");
    if (at == std::string::npos) return "";
    return util::trim(output.substr(at + 6, output.find('\n', at) - at - 6));
}

// what each file is: Wine's placeholder, Wine's own, Exbit's, something else, missing
void testDllKinds(const std::string& prefix) {
    std::string system32 = util::joinPath(prefix, "drive_c/windows/system32");
    CHECK(winesOwn(wine::dllKind(util::joinPath(system32, "msi.dll"))));
    CHECK(wine::dllKind(util::joinPath(system32, "missing.dll")).empty());
    std::string text = util::joinPath(tempRoot, "text.dll");
    util::writeFile(text, "not a DLL");
    CHECK(wine::dllKind(text) == "other");
    if (util::fileExists("/usr/lib/wine/x86_64-windows/msi.dll"))
        CHECK(wine::dllKind("/usr/lib/wine/x86_64-windows/msi.dll") == "builtin");
    std::string fixes = wine::fixesFolder();
    if (!fixes.empty()) CHECK(wine::dllKind(util::joinPath(fixes, "x86_64-windows/msi.dll")) == "exbit");
}

// Wine's own DLLs, then the fixes put in, checked, used, put in again, updated, taken out
void testFixes(const std::string& prefix) {
    std::string tests = util::absolutePath(environment::readProgramVariable("WIN_TEST_PLUGINS")
                                               ? environment::readProgramVariable("WIN_TEST_PLUGINS")
                                               : "build/win-test-plugins");
    std::string msiTest = util::joinPath(tests, "winefix_msi_commit.exe"), vblankTest = util::joinPath(tests, "winefix_vblank.exe");
    std::string log = util::joinPath(tempRoot, "install.log"), report;
    std::string system32 = util::joinPath(prefix, "drive_c/windows/system32"),
                syswow64 = util::joinPath(prefix, "drive_c/windows/syswow64");
    CHECK(!wine::prefixHasFixes(prefix));
    // Wine's own (informational: a Wine that has fixed these would say OK)
    int code = runAsInstaller(prefix, msiTest, log);
    printf("  Wine's own msi.dll: %s (exit %d)\n", code == 0 ? "OK" : "BROKEN", code);
    CHECK(fileText(log).find(code == 0 ? "OK" : "BROKEN") != std::string::npos);  // (the program's output, in the log)
    code = runAsInstaller(prefix, vblankTest, log);
    printf("  Wine's own WaitForVBlank: %s (exit %d)\n", code == 0 ? "OK" : code == 2 ? "no screen" : "BROKEN", code);

    // put in
    CHECK(wine::applyFixes(prefix, report));
    printf("  applied:\n%s\n", report.c_str());
    CHECK(wine::prefixHasFixes(prefix));
    for (const char* dll : {"msi.dll", "dxgi.dll"}) {
        CHECK(wine::dllKind(util::joinPath(system32, dll)) == "exbit");
        CHECK(wine::dllKind(util::joinPath(syswow64, dll)) == "exbit");
        CHECK(overrideOf(prefix, util::stem(dll)) == "native,builtin");
        // Wine's placeholders kept, for taking the fixes out
        CHECK(winesOwn(wine::dllKind(util::joinPath(prefix, std::string(".exbit-wine-fixes/system32/") + dll))));
    }
    // used: the database reads back as written, WaitForVBlank waits for a refresh
    CHECK(runAsInstaller(prefix, msiTest, log) == 0);
    CHECK(fileText(log).find("\nOK") != std::string::npos);
    code = runAsInstaller(prefix, vblankTest, log);
    printf("  Exbit's WaitForVBlank: %s\n", util::trim(fileText(log)).c_str());
    CHECK(code == 0 || code == 2);
    // the same again: nothing to do
    CHECK(wine::applyFixes(prefix, report));
    CHECK(wine::prefixHasFixes(prefix));
    // an older stand-in in the prefix: not as Exbit's now, and brought up to date
    std::string old = fileText(util::joinPath(system32, "msi.dll"));
    util::writeFile(util::joinPath(system32, "msi.dll"), old + "older");
    CHECK(!wine::prefixHasFixes(prefix));
    CHECK(wine::applyFixes(prefix, report));
    CHECK(wine::prefixHasFixes(prefix) && fileText(util::joinPath(system32, "msi.dll")) == old);

    // taken out: Wine's placeholders back, no overrides
    CHECK(wine::removeFixes(prefix, report));
    CHECK(!wine::prefixHasFixes(prefix));
    for (const char* dll : {"msi.dll", "dxgi.dll"}) {
        CHECK(winesOwn(wine::dllKind(util::joinPath(system32, dll))));
        CHECK(winesOwn(wine::dllKind(util::joinPath(syswow64, dll))));
        CHECK(overrideOf(prefix, util::stem(dll)).empty());
    }
}

// a stand-in that doesn't work there is taken out again by the check; a DLL that isn't Wine's is left alone
void testFallbacks(const std::string& prefix) {
    std::string fixes = wine::fixesFolder(), broken = util::joinPath(tempRoot, "broken-fixes"), report;
    std::string system32 = util::joinPath(prefix, "drive_c/windows/system32");
    for (const char* sub : {"x86_64-windows", "i386-windows"}) {
        util::makeDirs(util::joinPath(broken, sub));
        util::writeFile(util::joinPath(broken, std::string(sub) + "/msi.dll"), "MZ not really a DLL");
        util::writeFile(util::joinPath(broken, std::string(sub) + "/dxgi.dll"),
                        fileText(util::joinPath(fixes, std::string(sub) + "/dxgi.dll")));
    }
    for (const char* exe : {"exbit-wine-fix-check.exe", "exbit-wine-fix-check-32.exe"})
        util::writeFile(util::joinPath(broken, exe), fileText(util::joinPath(fixes, exe)));
    setenv("EXBIT_WINE_FIXES", broken.c_str(), 1);
    CHECK(wine::fixesFolder() == broken);
    CHECK(!wine::applyFixes(prefix, report));
    printf("  a broken msi.dll:\n%s\n", report.c_str());
    CHECK(winesOwn(wine::dllKind(util::joinPath(system32, "msi.dll"))));
    CHECK(overrideOf(prefix, "msi").empty());
    CHECK(wine::dllKind(util::joinPath(system32, "dxgi.dll")) == "exbit");  // (the other one works)
    CHECK(wine::removeFixes(prefix, report));
    unsetenv("EXBIT_WINE_FIXES");

    // DXVK's dxgi.dll (here: any DLL not Wine's) with its override: left as it is
    std::string tests = util::absolutePath(environment::readProgramVariable("WIN_TEST_PLUGINS") ? environment::readProgramVariable("WIN_TEST_PLUGINS") : "build/win-test-plugins");
    std::string dxvk = fileText(util::joinPath(tests, "winefix_vblank.exe"));  // (a DLL of no one we know)
    util::writeFile(util::joinPath(system32, "dxgi.dll"), dxvk);
    CHECK(wine::applyFixes(prefix, report));
    printf("  with another dxgi.dll:\n%s\n", report.c_str());
    CHECK(report.find("left") != std::string::npos);
    CHECK(fileText(util::joinPath(system32, "dxgi.dll")) == dxvk);
    CHECK(wine::dllKind(util::joinPath(system32, "msi.dll")) == "exbit");
    CHECK(wine::prefixHasFixes(prefix));  // (all it could put in)
    CHECK(wine::removeFixes(prefix, report));
    CHECK(fileText(util::joinPath(system32, "dxgi.dll")) == dxvk);
}

// the prefix's activity, as the watchdog sees it
void testActivity(const std::string& prefix) {
    std::string why;
    // a program kept running in the prefix (its wineserver with it)
    int pid = wine::startInstaller(prefix, util::joinPath(prefix, "drive_c/windows/system32/notepad.exe"), why);
    CHECK(pid > 0);
    usleep(4000000);
    double used = installer_activity::processorSeconds(prefix);
    int windows = installer_activity::visibleWindows(prefix);
    printf("  the prefix: %.2f s of processor time, %d windows\n", used, windows);
    CHECK(used > 0);
    CHECK(windows >= (getenv("DISPLAY") ? 1 : 0));  // (notepad's, where there is a screen)
    // ended: every program of the prefix
    CHECK(wine::endPrefixPrograms(prefix, why));
    CHECK(waitFor(pid, 30) != -2);
    usleep(1000000);
    CHECK(installer_activity::processorSeconds(prefix) == 0);
    CHECK(installer_activity::processorSeconds(util::joinPath(tempRoot, "no-prefix")) == 0);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    char tmpl[] = "/tmp/exbit-wine-fixes-test-XXXXXX";
    tempRoot = mkdtemp(tmpl);
    unsetenv("EXBIT_WINE_FIXES");
    if (environment::readProgramVariable("NO_WINE") || wine::winePath().empty()) {
        printf("(no Wine: nothing to try the fixes on)\n");
    } else {
        std::string prefix = makePrefix();
        if (!prefix.empty()) {
            testDllKinds(prefix);
            if (wine::fixesFolder().empty()) {
                printf("(Exbit's Wine fixes weren't built: make needs MinGW's x86_64-w64-mingw32-gcc)\n");
            } else {
                testFixes(prefix);
                testFallbacks(prefix);
            }
            testActivity(prefix);
            std::string why;
            wine::endPrefixPrograms(prefix, why);
        }
    }
    std::system(("rm -rf '" + tempRoot + "'").c_str());
    TEST_MAIN_END();
}
