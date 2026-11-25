// Discord's local RPC connection on Linux: a non-blocking Unix socket (core/discord_ipc.h)
#include "core/discord_ipc.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace discord_ipc {

std::vector<std::string> candidateEndpoints() {
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    const char* temporary = getenv("TMPDIR");
    bool hasRuntime = runtime && *runtime;
    std::string base = hasRuntime ? runtime : temporary && *temporary ? temporary : "/tmp";
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    std::vector<std::string> folders{base + "/"};
    if (hasRuntime) {  // (Discord from Flatpak and from Snap keep their sockets in folders of their own)
        folders.push_back(base + "/app/com.discordapp.Discord/");
        folders.push_back(base + "/snap.discord/");
    }
    std::vector<std::string> endpoints;
    for (auto& folder : folders)
        for (int n = 0; n < 10; n++) endpoints.push_back(folder + "discord-ipc-" + std::to_string(n));
    return endpoints;
}

bool Connection::open(const std::string& endpoint) {
    close();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (endpoint.size() >= sizeof address.sun_path) return false;
    snprintf(address.sun_path, sizeof address.sun_path, "%s", endpoint.c_str());
    socketDescriptor_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socketDescriptor_ < 0) return false;
    // (a Unix socket connects at once or not at all; one whose queue is full counts as not listening)
    if (connect(socketDescriptor_, (sockaddr*)&address, sizeof address) != 0) {
        close();
        return false;
    }
    return true;
}

bool Connection::isOpen() const { return socketDescriptor_ >= 0; }

void Connection::close() {
    if (socketDescriptor_ >= 0) ::close(socketDescriptor_);
    socketDescriptor_ = -1;
    receivedBytes_.clear();
}

bool Connection::writeAll(const std::string& bytes, int timeoutMilliseconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t sent = send(socketDescriptor_, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            done += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        pollfd writable{socketDescriptor_, POLLOUT, 0};
        if (left <= 0 || poll(&writable, 1, (int)left) != 1 || (writable.revents & (POLLERR | POLLHUP))) return false;
    }
    return true;
}

bool Connection::readSome(int timeoutMilliseconds) {
    pollfd readable{socketDescriptor_, POLLIN, 0};
    int ready = poll(&readable, 1, timeoutMilliseconds);
    if (ready < 0) return errno == EINTR;
    if (ready == 0) return true;  // (nothing yet)
    char chunk[4096];
    ssize_t got = recv(socketDescriptor_, chunk, sizeof chunk, MSG_DONTWAIT);
    if (got == 0) return false;  // Discord closed its end
    if (got < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    receivedBytes_.append(chunk, (size_t)got);
    return true;
}

}  // namespace discord_ipc
