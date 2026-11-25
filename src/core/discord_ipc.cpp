// Discord's local RPC frames and the parts of a connection both systems share (core/discord_ipc.h);
// the socket and the pipe themselves: discord_ipc_posix.cpp, discord_ipc_win.cpp
#include "core/discord_ipc.h"

#include <chrono>

namespace discord_ipc {

namespace {
// appends `value` as 4 little-endian bytes
void appendLittleEndian32(std::string& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) bytes += (char)((value >> shift) & 0xff);
}
// the little-endian uint32 at `offset`
uint32_t readLittleEndian32(const std::string& bytes, size_t offset) {
    uint32_t value = 0;
    for (int i = 3; i >= 0; i--) value = value << 8 | (unsigned char)bytes[offset + i];
    return value;
}
}  // namespace

std::string encodeFrame(uint32_t opcode, const std::string& json) {
    std::string bytes;
    bytes.reserve(8 + json.size());
    appendLittleEndian32(bytes, opcode);
    appendLittleEndian32(bytes, (uint32_t)json.size());
    return bytes + json;
}

DecodeResult decodeFrame(std::string& receivedBytes, Frame& frame) {
    if (receivedBytes.size() < 8) return DecodeResult::Incomplete;
    uint32_t length = readLittleEndian32(receivedBytes, 4);
    if (length > LARGEST_FRAME_BODY) return DecodeResult::Invalid;
    if (receivedBytes.size() < 8 + (size_t)length) return DecodeResult::Incomplete;
    frame.opcode = readLittleEndian32(receivedBytes, 0);
    frame.json = receivedBytes.substr(8, length);
    receivedBytes.erase(0, 8 + (size_t)length);
    return DecodeResult::Complete;
}

Connection::~Connection() { close(); }

bool Connection::sendFrame(uint32_t opcode, const std::string& json, int timeoutMilliseconds) {
    if (!isOpen()) return false;
    if (writeAll(encodeFrame(opcode, json), timeoutMilliseconds)) return true;
    close();
    return false;
}

bool Connection::receiveFrame(Frame& frame, int timeoutMilliseconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    for (bool firstTry = true;; firstTry = false) {
        if (!isOpen()) return false;
        DecodeResult result = decodeFrame(receivedBytes_, frame);
        if (result == DecodeResult::Complete) return true;
        if (result == DecodeResult::Invalid) {
            close();
            return false;
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 && !firstTry) return false;  // (once at least: what has arrived already is read)
        if (!readSome(left > 0 ? (int)left : 0)) {
            close();
            return false;
        }
    }
}

}  // namespace discord_ipc
