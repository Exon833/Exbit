// Discord Rich Presence (core/discord_presence.h, core/discord_ipc.h) against a fake Discord: a Unix socket
// on Linux, a named pipe on Windows. The frames' bytes, the handshake, SET_ACTIVITY's text, the rate limit,
// ping / pong, a close and the reconnect after it, a refused handshake, and the off and empty-ID states
// sending nothing.
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "check.h"
#include "core/discord_ipc.h"
#include "core/discord_presence.h"
#include "core/json.h"
#include "core/process_memory.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

int millisecondsLeft(Clock::time_point deadline) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? (int)left : 0;
}

// a stand-in for the Discord program: it listens where Exbit looks, takes one client at a time, and reads
// and writes frames byte by byte as Discord does (not with the code under test)
class FakeDiscord {
public:
    ~FakeDiscord() { stopListening(); }

#ifdef _WIN32
    // makes the named pipe `pipeName` (\\.\pipe\…)
    bool listen(const std::string& pipeName) {
        std::wstring wide(pipeName.begin(), pipeName.end());
        pipe_ = CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
        return pipe_ != INVALID_HANDLE_VALUE;
    }
    // waits for Exbit to connect
    bool accept(int timeoutMilliseconds) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ConnectNamedPipe(pipe_, &overlapped) != 0;
        DWORD error = GetLastError();
        if (!connected && error == ERROR_PIPE_CONNECTED) connected = true;
        if (!connected && error == ERROR_IO_PENDING) {
            if (WaitForSingleObject(overlapped.hEvent, (DWORD)timeoutMilliseconds) == WAIT_OBJECT_0) {
                DWORD unused = 0;
                connected = GetOverlappedResult(pipe_, &overlapped, &unused, FALSE) != 0;
            } else {
                CancelIo(pipe_);
                DWORD unused = 0;
                GetOverlappedResult(pipe_, &overlapped, &unused, TRUE);
            }
        }
        CloseHandle(overlapped.hEvent);
        clientConnected_ = connected;
        return connected;
    }
    // what Exbit wrote, within the time given; false if it closed the connection or wrote nothing
    bool readMore(int timeoutMilliseconds) {
        auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
                clientConnected_ = false;
                return false;
            }
            if (available) {
                std::string chunk(available, '\0');
                OVERLAPPED overlapped{};
                overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                DWORD got = 0;
                if (!ReadFile(pipe_, &chunk[0], available, nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) {
                    CloseHandle(overlapped.hEvent);
                    return false;
                }
                bool ok = GetOverlappedResult(pipe_, &overlapped, &got, TRUE) != 0;
                CloseHandle(overlapped.hEvent);
                if (!ok) return false;
                bytes_.append(chunk.data(), got);
                return true;
            }
            if (Clock::now() >= deadline) return false;
            Sleep(5);
        }
    }
    bool writeBytes(const std::string& bytes) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD written = 0;
        if (!WriteFile(pipe_, bytes.data(), (DWORD)bytes.size(), nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(overlapped.hEvent);
            return false;
        }
        bool ok = GetOverlappedResult(pipe_, &overlapped, &written, TRUE) != 0;
        CloseHandle(overlapped.hEvent);
        return ok && written == bytes.size();
    }
    // hangs up on Exbit (after it has read what was written)
    void dropClient() {
        if (clientConnected_) FlushFileBuffers(pipe_);
        DisconnectNamedPipe(pipe_);
        clientConnected_ = false;
        bytes_.clear();
    }
    void stopListening() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(pipe_);
            CloseHandle(pipe_);
        }
        pipe_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    bool clientConnected_ = false;
#else
    // makes the Unix socket at `socketPath`
    bool listen(const std::string& socketPath) {
        path_ = socketPath;
        unlink(path_.c_str());
        listener_ = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        snprintf(address.sun_path, sizeof address.sun_path, "%s", path_.c_str());
        return listener_ >= 0 && bind(listener_, (sockaddr*)&address, sizeof address) == 0 && ::listen(listener_, 4) == 0;
    }
    // waits for Exbit to connect
    bool accept(int timeoutMilliseconds) {
        pollfd waiting{listener_, POLLIN, 0};
        if (poll(&waiting, 1, timeoutMilliseconds) != 1) return false;
        client_ = ::accept(listener_, nullptr, nullptr);
        return client_ >= 0;
    }
    // what Exbit wrote, within the time given; false if it closed the connection or wrote nothing
    bool readMore(int timeoutMilliseconds) {
        pollfd waiting{client_, POLLIN, 0};
        if (client_ < 0 || poll(&waiting, 1, timeoutMilliseconds) != 1) return false;
        char chunk[4096];
        ssize_t got = recv(client_, chunk, sizeof chunk, 0);
        if (got <= 0) return false;
        bytes_.append(chunk, (size_t)got);
        return true;
    }
    bool writeBytes(const std::string& bytes) {
        return client_ >= 0 && send(client_, bytes.data(), bytes.size(), MSG_NOSIGNAL) == (ssize_t)bytes.size();
    }
    // hangs up on Exbit
    void dropClient() {
        if (client_ >= 0) close(client_);
        client_ = -1;
        bytes_.clear();
    }
    void stopListening() {
        dropClient();
        if (listener_ >= 0) close(listener_);
        listener_ = -1;
        if (!path_.empty()) unlink(path_.c_str());
    }

private:
    std::string path_;
    int listener_ = -1, client_ = -1;
#endif

public:
    // one frame from Exbit within the time given (decoded here by hand: little-endian opcode and length)
    bool readFrame(uint32_t& opcode, Json& body, int timeoutMilliseconds) {
        auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        for (;;) {
            if (bytes_.size() >= 8) {
                auto byteAt = [this](size_t i) { return (uint32_t)(unsigned char)bytes_[i]; };
                uint32_t length = byteAt(4) | byteAt(5) << 8 | byteAt(6) << 16 | byteAt(7) << 24;
                if (bytes_.size() >= 8 + (size_t)length) {
                    opcode = byteAt(0) | byteAt(1) << 8 | byteAt(2) << 16 | byteAt(3) << 24;
                    body = Json::parse(bytes_.substr(8, length));
                    bytes_.erase(0, 8 + length);
                    return true;
                }
            }
            if (!readMore(millisecondsLeft(deadline))) return false;
        }
    }
    // a frame to Exbit (encoded by hand)
    bool writeFrame(uint32_t opcode, const std::string& json) {
        std::string bytes;
        for (uint32_t value : {opcode, (uint32_t)json.size()})
            for (int shift = 0; shift < 32; shift += 8) bytes += (char)((value >> shift) & 0xff);
        return writeBytes(bytes + json);
    }
    // waits until Exbit closes its end (frames written before that are skipped)
    bool waitForClientToClose(int timeoutMilliseconds) {
        auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        while (Clock::now() < deadline)
            if (!readMore(millisecondsLeft(deadline))) return Clock::now() < deadline;
        return false;
    }
    // the handshake Exbit sends, answered with READY, as Discord does
    bool shakeHands(const std::string& expectedApplicationId) {
        uint32_t opcode = 99;
        Json hello;
        if (!readFrame(opcode, hello, 3000)) return false;
        CHECK(opcode == discord_ipc::HANDSHAKE);
        CHECK(hello["v"].toInt() == 1);
        CHECK(hello["client_id"].toString() == expectedApplicationId);
        return writeFrame(discord_ipc::FRAME,
                          "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\",\"data\":{\"v\":1,\"user\":{\"username\":\"test\"}}}");
    }

private:
    std::string bytes_;
};

long thisProcessId() {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

void testFrameEncoding() {
    std::string bytes = discord_ipc::encodeFrame(discord_ipc::FRAME, "{}");
    CHECK(bytes == std::string("\x01\x00\x00\x00\x02\x00\x00\x00{}", 10));
    std::string big = discord_ipc::encodeFrame(0x01020304u, std::string(0x0105, 'x'));
    CHECK(big.substr(0, 8) == std::string("\x04\x03\x02\x01\x05\x01\x00\x00", 8) && big.size() == 8 + 0x105);

    // two frames that arrive together, the second cut short: one taken, the rest kept for later
    std::string received = discord_ipc::encodeFrame(discord_ipc::PING, "{\"a\":1}") +
                           discord_ipc::encodeFrame(discord_ipc::CLOSE, "{\"code\":1000}").substr(0, 11);
    discord_ipc::Frame frame;
    CHECK(discord_ipc::decodeFrame(received, frame) == discord_ipc::DecodeResult::Complete);
    CHECK(frame.opcode == discord_ipc::PING && frame.json == "{\"a\":1}");
    CHECK(discord_ipc::decodeFrame(received, frame) == discord_ipc::DecodeResult::Incomplete);
    CHECK(received.size() == 11);
    received += std::string("ode\":1000}");
    CHECK(discord_ipc::decodeFrame(received, frame) == discord_ipc::DecodeResult::Complete);
    CHECK(frame.opcode == discord_ipc::CLOSE && frame.json == "{\"code\":1000}" && received.empty());
    // fewer than 8 bytes: not a frame yet
    std::string header = std::string("\x01\x00\x00", 3);
    CHECK(discord_ipc::decodeFrame(header, frame) == discord_ipc::DecodeResult::Incomplete);
    // a length no message has: a broken stream
    std::string garbage = std::string("\x01\x00\x00\x00\xff\xff\xff\x7f", 8);
    CHECK(discord_ipc::decodeFrame(garbage, frame) == discord_ipc::DecodeResult::Invalid);
}

void testText() {
    // the real program's pace: Discord takes a presence change about every 15 s at most, and retrying sooner
    // than 30 s after a failure only adds load (the tests below use their own, shorter times)
    discord_presence::Presence::Timing programTiming;
    CHECK(programTiming.minimumUpdateSeconds >= 15);
    CHECK(programTiming.retrySeconds >= 30);
    auto text = discord_presence::describeActivity(3, 2, 0.123, 180ull * 1024 * 1024);
    CHECK(text.details == "3 audio · 2 MIDI lanes");
    CHECK(text.state == "CPU 12 % · RAM 180 MB");
    CHECK(discord_presence::describeActivity(0, 0, -1, 0).state == "CPU 0 % · RAM 0 MB");
    CHECK(discord_presence::describeActivity(1, 0, 1.5, 1).details == "1 audio · 0 MIDI lanes");
    CHECK(discord_presence::describeActivity(1, 0, 1.5, 1).state == "CPU 150 % · RAM 0 MB");

    Json command = Json::parse(discord_presence::setActivityCommand(42, &text, "n1"));
    CHECK(command["cmd"].toString() == "SET_ACTIVITY");
    CHECK(command["args"]["pid"].toInt() == 42);
    CHECK(command["args"]["activity"]["details"].toString() == text.details);
    CHECK(command["args"]["activity"]["state"].toString() == text.state);
    CHECK(command["nonce"].toString() == "n1");
    Json cleared = Json::parse(discord_presence::setActivityCommand(42, nullptr, "n2"));
    CHECK(cleared["cmd"].toString() == "SET_ACTIVITY" && cleared["args"]["pid"].toInt() == 42);
    CHECK(cleared["args"]["activity"].isNull());

    // the application ID: Exbit's own unless the setting names another; none at all if the default were cleared
    CHECK(!discord_presence::DEFAULT_APPLICATION_ID.empty());
    CHECK(discord_presence::effectiveApplicationId("") == discord_presence::DEFAULT_APPLICATION_ID);
    CHECK(discord_presence::effectiveApplicationId("  \t") == discord_presence::DEFAULT_APPLICATION_ID);
    CHECK(discord_presence::effectiveApplicationId(" 1234 ") == "1234");
    CHECK(discord_presence::effectiveApplicationId("", "").empty());

    // the program's own RAM: more than a megabyte, less than a terabyte
    uint64_t resident = process_memory::residentBytes();
    CHECK(resident > (1u << 20) && resident < (1ull << 40));
}

void testEndpoints(const std::string& runtimeFolder) {
#ifdef _WIN32
    (void)runtimeFolder;
    auto endpoints = discord_ipc::candidateEndpoints();
    CHECK(endpoints.size() == 10);
    CHECK(endpoints.front() == "\\\\.\\pipe\\discord-ipc-0" && endpoints.back() == "\\\\.\\pipe\\discord-ipc-9");
#else
    auto endpoints = discord_ipc::candidateEndpoints();
    auto has = [&](const std::string& path) {
        for (auto& e : endpoints)
            if (e == path) return true;
        return false;
    };
    CHECK(endpoints.front() == runtimeFolder + "/discord-ipc-0");
    CHECK(has(runtimeFolder + "/discord-ipc-9"));
    CHECK(has(runtimeFolder + "/app/com.discordapp.Discord/discord-ipc-0"));
    CHECK(has(runtimeFolder + "/snap.discord/discord-ipc-3"));
    // no XDG_RUNTIME_DIR: TMPDIR, else /tmp
    std::string saved = getenv("XDG_RUNTIME_DIR");
    const char* tmpdir = getenv("TMPDIR");
    std::string savedTmpdir = tmpdir ? tmpdir : "";
    unsetenv("XDG_RUNTIME_DIR");
    setenv("TMPDIR", "/some/tmp", 1);
    CHECK(discord_ipc::candidateEndpoints().front() == "/some/tmp/discord-ipc-0");
    unsetenv("TMPDIR");
    CHECK(discord_ipc::candidateEndpoints().front() == "/tmp/discord-ipc-0");
    setenv("XDG_RUNTIME_DIR", saved.c_str(), 1);
    if (tmpdir) setenv("TMPDIR", savedTmpdir.c_str(), 1);
#endif
}

discord_presence::Presence::Options fastOptions(const std::string& endpoint) {
    discord_presence::Presence::Options options;
    options.timing.minimumUpdateSeconds = 1.0;
    options.timing.retrySeconds = 0.3;
    options.timing.handshakeTimeoutMilliseconds = 2000;
    options.timing.writeTimeoutMilliseconds = 1000;
#ifdef _WIN32
    options.endpoints = {endpoint};
#else
    (void)endpoint;  // (found the way Discord is found: through XDG_RUNTIME_DIR)
#endif
    options.readResidentBytes = [] { return (uint64_t)180 * 1024 * 1024; };
    return options;
}

// off, or on without an application ID: nobody connects
void testSendsNothingWhenOffOrWithoutId(const std::string& endpoint) {
    FakeDiscord discord;
    CHECK(discord.listen(endpoint));
    {
        discord_presence::Presence presence(fastOptions(endpoint));
        presence.setUsage(3, 2, 0.12);
        CHECK(presence.state() == discord_presence::Presence::State::Off);
        CHECK(!discord.accept(700));
        presence.configure(false, "123456789");
        CHECK(!discord.accept(700));
        CHECK(presence.state() == discord_presence::Presence::State::Off);
        presence.configure(true, "");
        CHECK(!discord.accept(700));
        CHECK(presence.state() == discord_presence::Presence::State::NoApplicationId);
        CHECK(presence.stateText().find("application ID") != std::string::npos);
        presence.configure(true, "   ");  // (blanks are no ID either)
        CHECK(!discord.accept(700));
    }
}

// on, with an ID: handshake, SET_ACTIVITY, the rate limit, ping / pong, a close and the reconnect, turned off
void testTalksToDiscord(const std::string& endpoint) {
    FakeDiscord discord;
    CHECK(discord.listen(endpoint));
    discord_presence::Presence presence(fastOptions(endpoint));
    presence.setUsage(3, 2, 0.123);
    presence.configure(true, "123456789");
    CHECK(discord.accept(3000));
    CHECK(discord.shakeHands("123456789"));

    uint32_t opcode = 99;
    Json command;
    CHECK(discord.readFrame(opcode, command, 3000));
    CHECK(opcode == discord_ipc::FRAME);
    CHECK(command["cmd"].toString() == "SET_ACTIVITY");
    CHECK(command["args"]["pid"].toInt() == thisProcessId());
    CHECK(command["args"]["activity"]["details"].toString() == "3 audio · 2 MIDI lanes");
    CHECK(command["args"]["activity"]["state"].toString() == "CPU 12 % · RAM 180 MB");
    CHECK(command["args"]["activity"]["assets"].isNull());
    std::string firstNonce = command["nonce"].toString();
    CHECK(!firstNonce.empty());
    CHECK(discord.writeFrame(discord_ipc::FRAME, "{\"cmd\":\"SET_ACTIVITY\",\"data\":{},\"evt\":null,\"nonce\":\"" + firstNonce + "\"}"));
    // Connected (the status said in Options → Discord status)
    for (int i = 0; i < 100 && presence.state() != discord_presence::Presence::State::Connected; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(presence.state() == discord_presence::Presence::State::Connected);

    // the rate limit: a change straight after is held back until a second has passed since the last one
    auto changedAt = Clock::now();
    presence.setUsage(4, 2, 0.123);
    CHECK(!discord.readFrame(opcode, command, 500));
    CHECK(discord.readFrame(opcode, command, 2000));
    CHECK(Clock::now() - changedAt >= std::chrono::milliseconds(900));
    CHECK(command["args"]["activity"]["details"].toString() == "4 audio · 2 MIDI lanes");
    CHECK(command["nonce"].toString() != firstNonce);
    // the same text again: nothing sent
    presence.setUsage(4, 2, 0.12);
    CHECK(!discord.readFrame(opcode, command, 1500));

    // a ping is answered with a pong carrying the same body
    CHECK(discord.writeFrame(discord_ipc::PING, "{\"ping\":17}"));
    CHECK(discord.readFrame(opcode, command, 2000));
    CHECK(opcode == discord_ipc::PONG && command["ping"].toInt() == 17);

    // Discord closes: Exbit comes back by itself, says hello again and sends its activity again
    CHECK(discord.writeFrame(discord_ipc::CLOSE, "{\"code\":1000,\"message\":\"bye\"}"));
    discord.dropClient();
    CHECK(discord.accept(3000));
    CHECK(discord.shakeHands("123456789"));
    CHECK(discord.readFrame(opcode, command, 3000));
    CHECK(opcode == discord_ipc::FRAME && command["args"]["activity"]["details"].toString() == "4 audio · 2 MIDI lanes");

    // turned off: the activity cleared, the connection closed, and nothing more
    presence.configure(false, "123456789");
    CHECK(discord.readFrame(opcode, command, 2000));
    CHECK(opcode == discord_ipc::FRAME && command["cmd"].toString() == "SET_ACTIVITY");
    CHECK(command["args"]["pid"].toInt() == thisProcessId() && command["args"]["activity"].isNull());
    CHECK(discord.waitForClientToClose(2000));
    discord.dropClient();
    CHECK(!discord.accept(800));
    CHECK(presence.state() == discord_presence::Presence::State::Off);

    // on again, then the program quits: cleared and closed as well
    presence.configure(true, "123456789");
    CHECK(discord.accept(3000));
    CHECK(discord.shakeHands("123456789"));
    CHECK(discord.readFrame(opcode, command, 3000));
}

// quitting while connected clears the activity
void testClearsOnQuit(const std::string& endpoint) {
    FakeDiscord discord;
    CHECK(discord.listen(endpoint));
    uint32_t opcode = 99;
    Json command;
    {
        discord_presence::Presence presence(fastOptions(endpoint));
        presence.setUsage(1, 1, 0.5);
        presence.configure(true, discord_presence::effectiveApplicationId(""));  // (the setting empty: Exbit's own)
        CHECK(discord.accept(3000));
        CHECK(discord.shakeHands(discord_presence::DEFAULT_APPLICATION_ID));
        CHECK(discord.readFrame(opcode, command, 3000));
        CHECK(command["args"]["activity"]["details"].toString() == "1 audio · 1 MIDI lanes");
    }
    CHECK(discord.readFrame(opcode, command, 2000));
    CHECK(opcode == discord_ipc::FRAME && command["args"]["activity"].isNull());
    CHECK(discord.waitForClientToClose(2000));
}

// a refused handshake (an application ID Discord doesn't know): no activity sent, tried again later
void testRefusedHandshakeIsRetried(const std::string& endpoint) {
    FakeDiscord discord;
    CHECK(discord.listen(endpoint));
    discord_presence::Presence presence(fastOptions(endpoint));
    presence.setUsage(2, 0, 0.0);
    presence.configure(true, "999");
    CHECK(discord.accept(3000));
    uint32_t opcode = 99;
    Json hello;
    CHECK(discord.readFrame(opcode, hello, 3000) && opcode == discord_ipc::HANDSHAKE);
    CHECK(discord.writeFrame(discord_ipc::CLOSE, "{\"code\":4000,\"message\":\"Invalid Client ID\"}"));
    discord.dropClient();
    for (int i = 0; i < 100 && presence.state() != discord_presence::Presence::State::WaitingToRetry; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(presence.state() == discord_presence::Presence::State::WaitingToRetry);
    CHECK(presence.stateText().find("Invalid Client ID") != std::string::npos);
    // after retrySeconds it tries again (quietly: the program goes on)
    CHECK(discord.accept(3000));
    CHECK(discord.shakeHands("999"));
    CHECK(discord.readFrame(opcode, hello, 3000) && opcode == discord_ipc::FRAME);
    CHECK(hello["args"]["activity"]["details"].toString() == "2 audio · 0 MIDI lanes");
}

// no Discord at all: it waits to retry, never stuck, and quits at once
void testNoDiscord(const std::string& endpoint) {
    discord_presence::Presence presence(fastOptions(endpoint));
    presence.configure(true, "123");
    for (int i = 0; i < 100 && presence.state() != discord_presence::Presence::State::WaitingToRetry; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(presence.state() == discord_presence::Presence::State::WaitingToRetry);
    CHECK(presence.stateText().find("Discord") != std::string::npos);
}

}  // namespace

int main() {
    testFrameEncoding();
    testText();
#ifdef _WIN32
    std::string runtimeFolder;
    std::string endpoint = "\\\\.\\pipe\\exbit-test-discord-ipc-" + std::to_string(thisProcessId());
#else
    // a runtime folder of its own, so the real Discord (if it runs here) is never talked to
    const char* tmp = getenv("TMPDIR");
    std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/exbit-discord-XXXXXX";
    std::string runtimeFolder = mkdtemp(&pattern[0]) ? pattern : "";
    CHECK(!runtimeFolder.empty());
    setenv("XDG_RUNTIME_DIR", runtimeFolder.c_str(), 1);
    std::string endpoint = runtimeFolder + "/discord-ipc-0";
#endif
    testEndpoints(runtimeFolder);
    testSendsNothingWhenOffOrWithoutId(endpoint);
    testTalksToDiscord(endpoint);
    testClearsOnQuit(endpoint);
    testRefusedHandshakeIsRetried(endpoint);
    testNoDiscord(endpoint);
#ifndef _WIN32
    rmdir(runtimeFolder.c_str());
#endif
    TEST_MAIN_END();
}
