// The operating system's services the program needs beyond the C++ library, the same on Linux (core/os_posix.cpp)
// and Windows (core/os_win.cpp): shared libraries, read-only mapped files, child processes and pipes, a wake-up
// event for threads, the monotonic clock, the locale. Paths are UTF-8, with '/' (Windows: "C:/Users/…"); the
// windows, the main loop and the desktop's folders are platform/platform.h's.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace os {

// --- shared libraries (dlopen / LoadLibraryW) ---
// nullptr if it can't be loaded (then *error says why); a name without '/' is looked for where the system looks
void* openLibrary(const std::string& path, std::string* error = nullptr);
void* librarySymbol(void* lib, const char* name);  // nullptr if it has none of that name
void closeLibrary(void* lib);

// --- a file mapped into memory, read-only (mmap / CreateFileMapping) ---
struct MappedFile {
    const uint8_t* data = nullptr;
    size_t size = 0;
    void* handle = nullptr;  // (Windows: the mapping)
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() { unmap(); }
    // false (and nothing mapped) if it isn't a regular file, is empty or can't be mapped
    bool map(const std::string& path);
    void unmap();
};

// --- child processes ---
// a pipe: fds[0] reads, fds[1] writes (C library fds: read(), write(), close()); neither is inherited by children
bool makePipe(int fds[2]);
struct Process {
    long long pid = -1;
    void* handle = nullptr;  // (Windows: the process handle)
};
// Starts args[0] (found in PATH when it has no '/'; Windows: '\\' too, and ".exe" may be left out) with args, its
// stdin and stderr the null device and its stdout `outFd` (-1: the null device too); doesn't wait. False if it
// couldn't start: *err an errno value (ENOENT: no such program).
bool spawn(const std::vector<std::string>& args, int outFd, Process& p, int* err = nullptr);
// its exit status (0…255; -1: it was killed or crashed); block false: -2 while it still runs
int waitProcess(Process& p, bool block = true);
void killProcess(const Process& p);
// starts a program that outlives this one and isn't waited for (stdin, stdout, stderr: the null device)
bool spawnDetached(const std::vector<std::string>& args);
std::string executablePath();  // the running program's file

// --- waking a thread that waits (eventfd / an auto-reset event) ---
class WakeEvent {
public:
    WakeEvent();
    ~WakeEvent();
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;
    void signal();               // any thread; the wakes before a wait count once
    bool wait(int timeoutMs);    // -1: until signalled; true if it was (and clears it)
    void clear();
    // Linux: an fd to poll() for POLLIN (then clear()); Windows: the event's HANDLE, for WaitForMultipleObjects
    intptr_t native() const { return native_; }

private:
    intptr_t native_ = -1;
};

// --- time ---
double monotonicSeconds();       // steady, from an arbitrary start
void sleepMs(int ms);

// --- the locale ---
// the C library's character classes from the environment (Linux: LANG/LC_*; Windows: UTF-8); true if they are UTF-8
bool setUtf8Locale();

#ifdef EXBIT_WINDOWS
// --- Windows' UTF-16 ---
std::wstring widen(const std::string& utf8);
std::string narrow(const wchar_t* utf16, int len = -1);
inline std::string narrow(const std::wstring& s) { return narrow(s.c_str(), (int)s.size()); }
// "C:/x/y" <-> "C:\x\y"
std::string nativePath(const std::string& path);
std::string portablePath(const std::string& path);
#endif

}  // namespace os
