// Discord Rich Presence: while Tiny DAW runs (and Options → Discord status is on), Discord shows how many audio
// and MIDI lanes the project has, the audio CPU load and the program's RAM, as two lines of text (no pictures).
//
// A background thread does all the talking (discord_ipc.h): the GUI only hands it the numbers. It connects,
// says hello (opcode 0 with the application ID; Discord answers READY), then sends SET_ACTIVITY whenever the
// text changed, at most once every minimumUpdateSeconds (Discord limits how often presence may change). It
// answers Discord's pings, and when Discord isn't running, refuses the ID or goes away, it notes that in the
// debug output and tries again retrySeconds later, quietly. Turned off or destroyed, it clears the activity
// and closes the connection. An empty application ID means nothing is ever sent.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace discord_presence {

// Tiny DAW's own application in Discord's developer portal: Discord shows its name ("Tiny DAW") above the text.
// Used when the setting (Options → Discord status → Application ID…) is empty; "" here would mean nothing is sent.
inline const std::string DEFAULT_APPLICATION_ID = "1575039315958833152";

// the application ID to say hello with: the setting (without blanks around it) when there is one, else `fallback`
std::string effectiveApplicationId(const std::string& setting, const std::string& fallback = DEFAULT_APPLICATION_ID);

// the two lines Discord shows
struct ActivityText {
    std::string details;  // "3 audio · 2 MIDI lanes"
    std::string state;    // "CPU 12 % · RAM 180 MB"
    bool operator==(const ActivityText& o) const { return details == o.details && state == o.state; }
    bool operator!=(const ActivityText& o) const { return !(*this == o); }
};

// the lines for these numbers. cpuLoad: the audio callback's load as a fraction (below 0: no audio running,
// shown as 0 %); residentBytes: the program's RAM
ActivityText describeActivity(int audioLanes, int midiLanes, double cpuLoad, uint64_t residentBytes);

// the JSON of a SET_ACTIVITY command for this process; activity null: the activity cleared
std::string setActivityCommand(long processId, const ActivityText* activity, const std::string& nonce);

class Presence {
public:
    struct Timing {
        double minimumUpdateSeconds = 15;  // presence changes at most this often
        double retrySeconds = 30;          // after a failed or lost connection, the next try
        int handshakeTimeoutMilliseconds = 3000;  // the longest wait for READY
        int writeTimeoutMilliseconds = 1000;      // the longest wait to write a frame
    };
    // where to connect (empty: discord_ipc::candidateEndpoints()) and the RAM reader (null: process_memory's);
    // tests give their own
    struct Options {
        Timing timing;
        std::vector<std::string> endpoints;
        std::function<uint64_t()> readResidentBytes;
    };
    Presence();
    explicit Presence(Options options);
    ~Presence();  // clears the activity, closes the connection, ends the thread
    Presence(const Presence&) = delete;
    Presence& operator=(const Presence&) = delete;

    // (GUI thread) on or off, with Discord's application ID; off or an empty ID: nothing is sent, and a
    // connection there was has its activity cleared and is closed
    void configure(bool enabled, const std::string& applicationId);
    // (GUI thread, cheap: it only stores them) the numbers to show
    void setUsage(int audioLanes, int midiLanes, double cpuLoad);

    enum class State { Off, NoApplicationId, Connecting, Connected, WaitingToRetry };
    State state() const { return state_.load(); }
    // what state() means, for Options → Discord status ("Connected to Discord", "Discord isn't running…")
    std::string stateText() const;

private:
    // the background thread: connects, sends, answers, retries until stopping_
    void run();
    // waits until woken, stopping, or `seconds` passed
    void sleepFor(double seconds);
    Options options_;
    mutable std::mutex lock_;
    std::condition_variable wake_;
    bool enabled_ = false;
    std::string applicationId_;
    int audioLanes_ = 0, midiLanes_ = 0;
    double cpuLoad_ = -1;
    bool stopping_ = false;
    std::atomic<State> state_{State::Off};
    std::string lastFailure_;  // (under lock_) why the last try failed, for stateText()
    std::thread thread_;
};

}  // namespace discord_presence
