// core/os.h on Linux
#include "core/os.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <langinfo.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <clocale>
#include <cstring>
#include <ctime>
#include <thread>

extern char** environ;

namespace os {

void* openLibrary(const std::string& path, std::string* error) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h && error) {
        const char* e = dlerror();
        *error = e ? e : "cannot load " + path;
    }
    return h;
}

void* librarySymbol(void* lib, const char* name) { return lib ? dlsym(lib, name) : nullptr; }

void closeLibrary(void* lib) {
    if (lib) dlclose(lib);
}

bool MappedFile::map(const std::string& path) {
    unmap();
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    void* m = MAP_FAILED;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
        m = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) return false;
    data = (const uint8_t*)m;
    size = (size_t)st.st_size;
    return true;
}

void MappedFile::unmap() {
    if (data) munmap((void*)data, size);
    data = nullptr;
    size = 0;
}

bool makePipe(int fds[2]) { return pipe2(fds, O_CLOEXEC) == 0; }

bool spawn(const std::vector<std::string>& args, int outFd, Process& p, int* err) {
    if (args.empty()) {
        if (err) *err = EINVAL;
        return false;
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (outFd >= 0) posix_spawn_file_actions_adddup2(&actions, outFd, 1);
    else posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    int e = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (e != 0) {
        if (err) *err = e;
        return false;
    }
    p.pid = pid;
    return true;
}

int waitProcess(Process& p, bool block) {
    if (p.pid <= 0) return -1;
    int status = 0;
    pid_t r;
    while ((r = waitpid((pid_t)p.pid, &status, block ? 0 : WNOHANG)) < 0 && errno == EINTR) {}
    if (r == 0) return -2;
    p.pid = -1;
    if (r < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void killProcess(const Process& p) {
    if (p.pid > 0) kill((pid_t)p.pid, SIGKILL);
}

bool spawnDetached(const std::vector<std::string>& args) {
    Process p;
    if (!spawn(args, -1, p)) return false;
    std::thread([p]() mutable { waitProcess(p); }).detach();  // (reaped when it ends: no zombie)
    return true;
}

std::string executablePath() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return std::string();
    return std::string(buf, (size_t)n);
}

WakeEvent::WakeEvent() { native_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); }

WakeEvent::~WakeEvent() {
    if (native_ >= 0) ::close((int)native_);
}

void WakeEvent::signal() {
    uint64_t one = 1;
    ssize_t r = write((int)native_, &one, sizeof one);
    (void)r;
}

void WakeEvent::clear() {
    uint64_t v;
    ssize_t r = read((int)native_, &v, sizeof v);
    (void)r;
}

bool WakeEvent::wait(int timeoutMs) {
    pollfd fd{(int)native_, POLLIN, 0};
    int r;
    while ((r = poll(&fd, 1, timeoutMs)) < 0 && errno == EINTR) {}
    if (r <= 0) return false;
    clear();
    return true;
}

double monotonicSeconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

void sleepMs(int ms) {
    timespec ts{ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

bool setUtf8Locale() {
    setlocale(LC_CTYPE, "");
    const char* cs = nl_langinfo(CODESET);
    return cs && (!strcasecmp(cs, "UTF-8") || !strcasecmp(cs, "utf8"));
}

}  // namespace os
