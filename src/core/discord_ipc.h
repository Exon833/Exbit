// Discord's local RPC connection (no Discord SDK): the desktop Discord program listens on a Unix socket
// ($XDG_RUNTIME_DIR/discord-ipc-N on Linux, also inside Flatpak's and Snap's folders) or a named pipe
// (\\.\pipe\discord-ipc-N on Windows), N = 0…9. Every message is a frame: a little-endian uint32 opcode,
// a little-endian uint32 length, then that many bytes of UTF-8 JSON.
//
// The connection's reads and writes never wait longer than the time they are given (the socket is
// non-blocking, the pipe overlapped), so a Discord that hangs can't hold up whoever calls them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace discord_ipc {

enum Opcode : uint32_t { HANDSHAKE = 0, FRAME = 1, CLOSE = 2, PING = 3, PONG = 4 };

// the largest JSON body accepted from Discord (anything longer is a broken stream, not a message)
constexpr uint32_t LARGEST_FRAME_BODY = 64 * 1024;

struct Frame {
    uint32_t opcode = 0;
    std::string json;
};

// the bytes of one frame: opcode, length (both little-endian uint32), then the JSON text
std::string encodeFrame(uint32_t opcode, const std::string& json);

enum class DecodeResult { Incomplete, Complete, Invalid };
// takes one whole frame off the front of `receivedBytes` into `frame` (Complete); Incomplete leaves the
// bytes as they are until more arrive; Invalid: the length is beyond LARGEST_FRAME_BODY
DecodeResult decodeFrame(std::string& receivedBytes, Frame& frame);

// where Discord may be listening, in the order they are tried: Linux socket paths ($XDG_RUNTIME_DIR, else
// $TMPDIR, else /tmp; and Flatpak's and Snap's folders under $XDG_RUNTIME_DIR), or Windows pipe names
std::vector<std::string> candidateEndpoints();

// one connection to Discord (a socket on Linux, a pipe on Windows); closed when destroyed
class Connection {
public:
    Connection() = default;
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // connects to the socket path or pipe name without waiting; false (and closed) if nobody listens there
    bool open(const std::string& endpoint);
    bool isOpen() const;
    void close();
    // writes one frame within timeoutMilliseconds; false if it couldn't (the connection is then closed)
    bool sendFrame(uint32_t opcode, const std::string& json, int timeoutMilliseconds);
    // waits up to timeoutMilliseconds (0: only what has arrived) for one whole frame. False with the
    // connection still open: none yet; false with it closed: Discord went away or sent garbage.
    bool receiveFrame(Frame& frame, int timeoutMilliseconds);

private:
    // writes all of `bytes` within the time given; false if it couldn't
    bool writeAll(const std::string& bytes, int timeoutMilliseconds);
    // appends what arrives within the time given to receivedBytes_; false if the connection broke
    bool readSome(int timeoutMilliseconds);
    std::string receivedBytes_;  // read but not yet a whole frame
#ifdef _WIN32
    void* pipeHandle_ = nullptr;  // (a HANDLE; null: closed)
#else
    int socketDescriptor_ = -1;
#endif
};

}  // namespace discord_ipc
