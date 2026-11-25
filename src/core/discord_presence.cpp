// Discord Rich Presence: the text, the SET_ACTIVITY command and the background thread (core/discord_presence.h)
#include "core/discord_presence.h"

#include <chrono>
#include <cmath>
#include <unistd.h>

#include "core/discord_ipc.h"
#include "core/json.h"
#include "core/process_memory.h"
#include "core/util.h"

namespace discord_presence {

using Clock = std::chrono::steady_clock;

ActivityText describeActivity(int audioLanes, int midiLanes, double cpuLoad, uint64_t residentBytes) {
    long cpuPercent = cpuLoad > 0 ? std::lround(cpuLoad * 100) : 0;
    uint64_t megabytes = (residentBytes + 512 * 1024) / (1024 * 1024);
    return {std::to_string(audioLanes) + " audio · " + std::to_string(midiLanes) + " MIDI lanes",
            "CPU " + std::to_string(cpuPercent) + " % · RAM " + std::to_string(megabytes) + " MB"};
}

std::string setActivityCommand(long processId, const ActivityText* activity, const std::string& nonce) {
    Json arguments = Json::object();
    arguments["pid"] = Json(processId);
    if (activity) {
        Json shown = Json::object();
        shown["details"] = Json(activity->details);
        shown["state"] = Json(activity->state);
        arguments["activity"] = shown;
    }
    Json command = Json::object();
    command["cmd"] = Json("SET_ACTIVITY");
    command["args"] = arguments;
    command["nonce"] = Json(nonce);
    return command.dump();
}

namespace {
// the ID without the blanks around it (pasted from the developer portal)
std::string trimmed(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
// how long (in the loop's waits) the thread sleeps between looks at Discord's messages while connected
constexpr double CONNECTED_LOOK_SECONDS = 0.25;
}  // namespace

std::string effectiveApplicationId(const std::string& setting, const std::string& fallback) {
    std::string chosen = trimmed(setting);
    return chosen.empty() ? trimmed(fallback) : chosen;
}

Presence::Presence() : Presence(Options()) {}

Presence::Presence(Options options) : options_(std::move(options)) {
    if (options_.endpoints.empty()) options_.endpoints = discord_ipc::candidateEndpoints();
    if (!options_.readResidentBytes) options_.readResidentBytes = process_memory::residentBytes;
    thread_ = std::thread([this] { run(); });
}

Presence::~Presence() {
    {
        std::lock_guard<std::mutex> guard(lock_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Presence::configure(bool enabled, const std::string& applicationId) {
    std::string id = trimmed(applicationId);
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (enabled == enabled_ && id == applicationId_) return;
        enabled_ = enabled;
        applicationId_ = id;
    }
    if (!enabled) state_ = State::Off;
    else if (id.empty()) state_ = State::NoApplicationId;
    else state_ = State::Connecting;
    wake_.notify_all();
}

void Presence::setUsage(int audioLanes, int midiLanes, double cpuLoad) {
    std::lock_guard<std::mutex> guard(lock_);
    audioLanes_ = audioLanes;
    midiLanes_ = midiLanes;
    cpuLoad_ = cpuLoad;
}

std::string Presence::stateText() const {
    switch (state_.load()) {
        case State::Off: return "Off";
        case State::NoApplicationId:
            return "No application ID: nothing is shown";
        case State::Connecting: return "Connecting to Discord…";
        case State::Connected: return "Shown in Discord";
        case State::WaitingToRetry: {
            std::lock_guard<std::mutex> guard(lock_);
            return lastFailure_ + " (tried again every " + std::to_string((int)std::lround(options_.timing.retrySeconds)) +
                   " s)";
        }
    }
    return "";
}

void Presence::sleepFor(double seconds) {
    std::unique_lock<std::mutex> guard(lock_);
    if (stopping_ || seconds <= 0) return;
    // (woken by configure() and the destructor; a change of numbers is picked up at the next look)
    wake_.wait_for(guard, std::chrono::duration<double>(seconds));
}

void Presence::run() {
    const Timing timing = options_.timing;
    const long processId = (long)getpid();
    discord_ipc::Connection connection;
    std::string connectedApplicationId;
    long nonceCount = 0;
    ActivityText lastSent;
    bool sentOnThisConnection = false, sentEver = false;
    Clock::time_point lastSentAt{}, nextTry = Clock::now();
    std::string lastLoggedFailure;

    auto nextNonce = [&] { return "tinydaw-" + std::to_string(processId) + "-" + std::to_string(++nonceCount); };
    // a try that failed (or a connection that broke): said once in the debug output, tried again later
    auto failed = [&](const std::string& why) {
        connection.close();
        if (why != lastLoggedFailure) util::debugf("discord: %s; trying again every %g s\n", why.c_str(), timing.retrySeconds);
        lastLoggedFailure = why;
        {
            std::lock_guard<std::mutex> guard(lock_);
            lastFailure_ = why;
        }
        nextTry = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timing.retrySeconds));
        state_ = State::WaitingToRetry;
    };
    // the activity cleared and the connection closed (turned off, another ID, quitting)
    auto leave = [&] {
        if (connection.isOpen()) {
            connection.sendFrame(discord_ipc::FRAME, setActivityCommand(processId, nullptr, nextNonce()),
                                 timing.writeTimeoutMilliseconds);
            util::debugf("discord: activity cleared, connection closed\n");
        }
        connection.close();
        sentOnThisConnection = false;
    };
    // what Discord sent: pings answered; false if it closed the connection (`why` says what it said)
    auto answerDiscord = [&](const discord_ipc::Frame& frame, std::string& why) {
        Json body = Json::parse(frame.json);
        if (frame.opcode == discord_ipc::PING) {
            connection.sendFrame(discord_ipc::PONG, frame.json, timing.writeTimeoutMilliseconds);
        } else if (frame.opcode == discord_ipc::CLOSE) {
            std::string message = body["message"].toString();
            why = "Discord closed the connection" + (message.empty() ? std::string() : ": " + message);
            connection.close();
            return false;
        } else if (frame.opcode == discord_ipc::FRAME && body["evt"].toString() == "ERROR") {
            util::debugf("discord: error: %s\n", body["data"]["message"].toString().c_str());
        }
        return connection.isOpen();
    };
    // connects to the first place Discord listens, says hello and waits for READY; false and `why` if it couldn't
    auto connectAndShakeHands = [&](const std::string& applicationId, std::string& why) {
        bool opened = false;
        for (auto& endpoint : options_.endpoints)
            if ((opened = connection.open(endpoint))) {
                util::debugf("discord: connected to %s\n", endpoint.c_str());
                break;
            }
        if (!opened) {
            why = "Discord isn't running";
            return false;
        }
        Json hello = Json::object();
        hello["v"] = Json(1);
        hello["client_id"] = Json(applicationId);
        if (!connection.sendFrame(discord_ipc::HANDSHAKE, hello.dump(), timing.writeTimeoutMilliseconds)) {
            why = "Discord didn't take the handshake";
            return false;
        }
        auto deadline = Clock::now() + std::chrono::milliseconds(timing.handshakeTimeoutMilliseconds);
        while (Clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> guard(lock_);
                if (stopping_) break;
            }
            discord_ipc::Frame frame;
            if (connection.receiveFrame(frame, 100)) {
                if (frame.opcode == discord_ipc::FRAME && Json::parse(frame.json)["evt"].toString() == "READY") return true;
                if (frame.opcode == discord_ipc::CLOSE) {  // (an application ID Discord doesn't know, most often)
                    std::string message = Json::parse(frame.json)["message"].toString();
                    why = "Discord refused the application ID" + (message.empty() ? std::string() : ": " + message);
                    connection.close();
                    return false;
                }
                if (!answerDiscord(frame, why)) return false;
            } else if (!connection.isOpen()) {
                why = "Discord closed the connection";
                return false;
            }
        }
        why = "Discord didn't answer the handshake";
        return false;
    };

    for (;;) {
        bool enabled, stopping;
        std::string applicationId;
        int audioLanes, midiLanes;
        double cpuLoad;
        {
            std::lock_guard<std::mutex> guard(lock_);
            enabled = enabled_;
            stopping = stopping_;
            applicationId = applicationId_;
            audioLanes = audioLanes_;
            midiLanes = midiLanes_;
            cpuLoad = cpuLoad_;
        }
        if (stopping) {
            leave();
            return;
        }
        if (!enabled || applicationId.empty()) {
            leave();
            state_ = enabled ? State::NoApplicationId : State::Off;
            nextTry = Clock::now();  // (turned on, it connects at once)
            lastLoggedFailure.clear();
            sleepFor(3600);
            continue;
        }
        if (connection.isOpen() && applicationId != connectedApplicationId) leave();  // (another ID: hello again)
        if (!connection.isOpen()) {
            auto now = Clock::now();
            if (now < nextTry) {
                state_ = State::WaitingToRetry;
                sleepFor(std::chrono::duration<double>(nextTry - now).count());
                continue;
            }
            state_ = State::Connecting;
            std::string why;
            if (!connectAndShakeHands(applicationId, why)) {
                failed(why);
                continue;
            }
            util::debugf("discord: READY (application %s)\n", applicationId.c_str());
            connectedApplicationId = applicationId;
            sentOnThisConnection = false;
            lastLoggedFailure.clear();
            state_ = State::Connected;
        }
        // what Discord sent since the last look
        discord_ipc::Frame frame;
        std::string why;
        bool lost = false;
        while (!lost && connection.receiveFrame(frame, 0)) lost = !answerDiscord(frame, why);
        if (lost || !connection.isOpen()) {
            failed(why.empty() ? "The connection to Discord was lost" : why);
            continue;
        }
        // the activity, when it changed and the rate limit allows
        double sinceLastSent = sentEver ? std::chrono::duration<double>(Clock::now() - lastSentAt).count() : 1e9;
        if (sinceLastSent >= timing.minimumUpdateSeconds) {
            ActivityText text = describeActivity(audioLanes, midiLanes, cpuLoad, options_.readResidentBytes());
            if (!sentOnThisConnection || text != lastSent) {
                if (!connection.sendFrame(discord_ipc::FRAME, setActivityCommand(processId, &text, nextNonce()),
                                          timing.writeTimeoutMilliseconds)) {
                    failed("Discord didn't take the activity");
                    continue;
                }
                util::debugf("discord: activity \"%s\" / \"%s\"\n", text.details.c_str(), text.state.c_str());
                lastSent = text;
                lastSentAt = Clock::now();
                sentOnThisConnection = sentEver = true;
            }
        }
        sleepFor(CONNECTED_LOOK_SECONDS);
    }
}

}  // namespace discord_presence
