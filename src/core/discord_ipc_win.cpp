// Discord's local RPC connection on Windows: the named pipe \\.\pipe\discord-ipc-N, opened for overlapped
// I/O so that no read or write waits longer than it is given (core/discord_ipc.h)
#include "core/discord_ipc.h"

#include <windows.h>

#include <chrono>

#include "core/os.h"

namespace discord_ipc {

std::vector<std::string> candidateEndpoints() {
    std::vector<std::string> endpoints;
    for (int n = 0; n < 10; n++) endpoints.push_back("\\\\.\\pipe\\discord-ipc-" + std::to_string(n));
    return endpoints;
}

bool Connection::open(const std::string& endpoint) {
    close();
    HANDLE pipe = CreateFileW(os::widen(endpoint).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;  // (none there, or busy with another program)
    pipeHandle_ = pipe;
    return true;
}

bool Connection::isOpen() const { return pipeHandle_ != nullptr; }

void Connection::close() {
    if (pipeHandle_) CloseHandle((HANDLE)pipeHandle_);
    pipeHandle_ = nullptr;
    receivedBytes_.clear();
}

namespace {
// waits up to timeoutMilliseconds for an overlapped read or write to finish (else cancels it); the bytes it moved
bool finishOverlapped(HANDLE pipe, OVERLAPPED& overlapped, bool started, int timeoutMilliseconds, DWORD& transferred) {
    if (!started && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(overlapped.hEvent, (DWORD)(timeoutMilliseconds > 0 ? timeoutMilliseconds : 0)) != WAIT_OBJECT_0) {
        CancelIo(pipe);
        GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);  // (the cancel has to finish before `overlapped` goes)
        return false;
    }
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != 0;
}
}  // namespace

bool Connection::writeAll(const std::string& bytes, int timeoutMilliseconds) {
    HANDLE pipe = (HANDLE)pipeHandle_;
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) return false;
    DWORD written = 0;
    bool started = WriteFile(pipe, bytes.data(), (DWORD)bytes.size(), nullptr, &overlapped) != 0;
    bool ok = finishOverlapped(pipe, overlapped, started, timeoutMilliseconds, written);
    CloseHandle(overlapped.hEvent);
    return ok && written == bytes.size();
}

bool Connection::readSome(int timeoutMilliseconds) {
    HANDLE pipe = (HANDLE)pipeHandle_;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;  // (Discord closed its end)
        if (available) {
            std::string chunk(available, '\0');
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!overlapped.hEvent) return false;
            DWORD got = 0;
            bool started = ReadFile(pipe, &chunk[0], available, nullptr, &overlapped) != 0;
            bool ok = finishOverlapped(pipe, overlapped, started, 1000, got);  // (the bytes are there: at once)
            CloseHandle(overlapped.hEvent);
            if (!ok) return false;
            receivedBytes_.append(chunk.data(), got);
            return true;
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return true;  // (nothing yet)
        Sleep((DWORD)(left < 10 ? left : 10));  // shortcut: a pipe can't be waited on for data, so it is looked at every 10 ms
    }
}

}  // namespace discord_ipc
