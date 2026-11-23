// The OS layer (core/os.h) and the path helpers it serves (core/util.h), on Linux and Windows: shared libraries,
// mapped files, child processes through a pipe, the wake-up event across threads, the clock, replacing renames,
// and (Windows) drive letters in paths and UTF-8 file names.
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <string>
#include <thread>

#include "check.h"
#include "core/os.h"
#include "core/util.h"

int main() {
    std::string dir = util::absolutePath("build/test-os");
    util::makeDirs(dir);

    // --- shared libraries ---
    std::string err;
#ifdef _WIN32
    void* lib = os::openLibrary("kernel32.dll", &err);
    CHECK(lib && os::librarySymbol(lib, "GetTickCount64"));
#else
    void* lib = os::openLibrary("libc.so.6", &err);
    CHECK(lib && os::librarySymbol(lib, "getpid"));
#endif
    CHECK(lib && !os::librarySymbol(lib, "no_such_function_anywhere"));
    os::closeLibrary(lib);
    CHECK(!os::openLibrary(dir + "/missing.library", &err) && !err.empty());

    // --- mapped files (an empty one isn't mapped) ---
    std::string file = dir + "/mapped.bin", data(100000, 'x');
    data[99999] = 'y';
    CHECK(util::writeFile(file, data));
    {
        os::MappedFile m;
        CHECK(m.map(file) && m.size == data.size() && m.data[0] == 'x' && m.data[99999] == 'y');
        m.unmap();
        CHECK(!m.data && m.size == 0);
        CHECK(util::writeFile(file, ""));
        CHECK(!m.map(file) && !m.map(dir + "/missing.bin") && !m.map(dir));
    }

    // --- renames replace what is there ---
    CHECK(util::writeFile(dir + "/a.txt", "new") && util::writeFile(dir + "/b.txt", "old"));
    CHECK(util::renameFile(dir + "/a.txt", dir + "/b.txt"));
    std::string text;
    CHECK(util::readFile(dir + "/b.txt", text) && text == "new" && !util::fileExists(dir + "/a.txt"));
    CHECK(!util::renameFile(dir + "/a.txt", dir + "/c.txt") && errno == ENOENT);

    // --- a child's output through a pipe; its exit status; a program that isn't there ---
    int fds[2];
    CHECK(os::makePipe(fds));
#ifdef _WIN32
    std::vector<std::string> args = {"cmd", "/c", "echo hello& exit 3"};
#else
    std::vector<std::string> args = {"sh", "-c", "echo hello; exit 3"};
#endif
    os::Process child;
    CHECK(os::spawn(args, fds[1], child));
    close(fds[1]);
    std::string out;
    char buf[256];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    close(fds[0]);
    CHECK(util::trim(out) == "hello");
    CHECK(os::waitProcess(child) == 3);
    os::Process none;
    int e = 0;
    CHECK(!os::spawn({"tinydaw-no-such-program"}, -1, none, &e) && e == ENOENT);
#ifdef _WIN32
    os::Process slow;
    CHECK(os::spawn({"cmd", "/c", "ping -n 30 127.0.0.1 >NUL"}, -1, slow));
#else
    os::Process slow;
    CHECK(os::spawn({"sleep", "30"}, -1, slow));
#endif
    CHECK(os::waitProcess(slow, false) == -2);  // (still running)
    os::killProcess(slow);
    CHECK(os::waitProcess(slow) != 0);

    // --- the wake-up event: from another thread, and a wait that times out ---
    {
        os::WakeEvent ev;
        double t0 = os::monotonicSeconds();
        CHECK(!ev.wait(20));
        CHECK(os::monotonicSeconds() - t0 >= 0.015);
        std::thread th([&ev] {
            os::sleepMs(30);
            ev.signal();
        });
        CHECK(ev.wait(5000));
        th.join();
        ev.signal();
        ev.signal();
        CHECK(ev.wait(0) && !ev.wait(0));  // (two wakes before a wait count once)
    }
    CHECK(!os::executablePath().empty() && util::fileExists(os::executablePath()));

#ifdef _WIN32
    // --- Windows' paths: drive letters, '\\' in, '/' out ---
    CHECK(util::joinPath("C:/a", "D:/b") == "D:/b");
    CHECK(util::joinPath("C:/a", "b") == "C:/a/b");
    CHECK(util::dirName("C:/x") == "C:/" && util::dirName("C:/") == "C:/" && util::dirName("C:/x/y") == "C:/x");
    CHECK(util::absolutePath("c:\\x\\..\\y\\.\\z") == "C:/y/z");
    CHECK(util::absolutePath("C:/") == "C:/");
    CHECK(util::absolutePath("/x").size() == 4 && util::absolutePath("/x")[1] == ':');  // (the current drive)
    CHECK(os::nativePath("C:/a/b") == "C:\\a\\b" && os::portablePath("C:\\a\\b") == "C:/a/b");
    CHECK(os::narrow(os::widen("Grüße Ωmega ♪")) == "Grüße Ωmega ♪");
    // UTF-8 names through the C library (the manifest's code page) and through listDir
    std::string named = dir + "/Grüße Ωmega.txt";
    CHECK(util::writeFile(named, "ok") && util::fileExists(named) && util::readFile(named, text) && text == "ok");
    bool listed = false;
    for (auto& d : util::listDir(dir)) listed = listed || d.name == "Grüße Ωmega.txt";
    CHECK(listed);
    util::removeFile(named);
#endif
    TEST_MAIN_END();
}
