// Every MIDI port Tiny DAW listens to or plays into, at once (Options → MIDI ports…: ui/midi_ports_dialog.h).
//
// A Backend is one kind of port: the ALSA sequencer (alsaBackend(), audio/midi_in.cpp: one input port "MIDI in"
// subscribed to every input chosen, and events sent straight to each output chosen) and JACK MIDI while a JACK
// server runs (jackBackend(), audio/midi_jack.cpp: a client "Tiny DAW MIDI" with a port per port chosen). Both are
// loaded at run time (dlopen): without them there are simply no ports. Tests use a fake one (tests/test_midi_ports).
//
// The Hub keeps the list: each port by its stable name (ALSA's without its numbers; JACK's "JACK: " + its full
// name) with its settings: On, and Clock in (inputs) or Clock out, Always send Start and a clock offset (outputs).
// They are remembered by name (settings(): JSON in the preferences, Preferences::midiPorts). A port remembered but
// not there now is kept, greyed "not connected", and opened again as soon as it comes back: the Hub looks again
// every RESCAN_S, and at once when ALSA announces a port came or went. A port that goes while open (a keyboard
// unplugged) lets its notes go (the GUI: midi_input's lost()); an output closed gets All Notes Off first.
// Inputs never seen before start On (several at once are expected), but a "Midi Through" port; outputs start Off.
//
// THREADS. GUI thread: tick(), poll(), the settings. Each backend's input arrives on a thread of its own (ALSA's
// reader, JACK's process thread) into a lock-free ring the GUI thread drains (poll()). What goes out is made by the
// audio thread (midi_out::Out: notes of External synth tracks, clock, Start/Stop) into another lock-free ring with
// the moment each must be heard; the Hub's output thread takes them, waits until each one's moment and sends it
// (ALSA: direct to the port, so a message leaves within a few microseconds of its time); a backend that places
// events itself (JACK: frame offsets in its cycle, sample-accurate) gets them at once with their moment. Notes played
// live into an External synth track (GUI thread: sendNow) go out at once.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/midi_in.h"
#include "core/json.h"
#include "core/spsc.h"

namespace midi_out {
class Out;
}

namespace midi_ports {

constexpr double RESCAN_S = 3.0;   // ports are looked for again this often (and when ALSA announces a change)
constexpr int MAX_SLOTS = 256;     // ports known in one session at most (each keeps its slot for good)
constexpr const char* JACK_PREFIX = "JACK: ";
constexpr double MAX_OFFSET_MS = 100.0;  // a clock output's offset: -100 … 100 ms

// a port a backend sees now
struct Found {
    std::string key;      // its stable name (what is remembered)
    bool input = true;    // true: we listen to it (a keyboard); false: we send to it (a synth)
    int client = 0, port = 0;  // (ALSA's address)
    std::string full;     // (JACK: its full name)
};

class Backend {
public:
    Backend() : in_(8192) {}
    virtual ~Backend() = default;
    virtual const char* name() const = 0;          // "ALSA", "JACK" (shown)
    virtual void list(std::vector<Found>& out) = 0;  // every port it sees now (GUI thread; appended)
    virtual bool open(const Found& port, int slot) = 0;  // listen to it / ready to send to it, as `slot`
    virtual void close(int slot) = 0;
    // (the output thread) send `bytes` to output `slot`: now, or at monotonic `when` if timed()
    virtual void send(int slot, const uint8_t* bytes, int n, double when) = 0;
    virtual bool timed() const { return false; }   // send() places a message at its moment itself
    bool changed() { return changed_.exchange(false); }  // ports came or went since asked last
    // (GUI thread) what arrived on its inputs since the last call (appended, oldest first)
    size_t poll(std::vector<midi_in::Event>& out);
    int dropped() const { return in_.dropped(); }

protected:
    void deliver(const midi_in::Event& e) { in_.push(e); }  // (its one input thread)
    // a raw MIDI message from input `slot` at monotonic `time` (running status not kept: whole messages)
    void deliverBytes(int slot, const uint8_t* b, int n, double time);
    std::atomic<bool> changed_{false};

private:
    Spsc<midi_in::Event> in_;
};

std::unique_ptr<Backend> alsaBackend();  // the ALSA sequencer's (null without one)
std::unique_ptr<Backend> jackBackend();  // JACK MIDI's (null without a JACK server)
// JACK's ports' keys and the like: is this a JACK port's key?
inline bool isJack(const std::string& key) { return key.compare(0, 6, JACK_PREFIX) == 0; }

// a port in the list, with its settings
struct Port {
    std::string key;
    bool input = true;
    bool present = false;   // there now (else: "not connected", greyed)
    bool on = false;        // listened to / played into
    bool clock = false;     // inputs: Clock in (the transport may follow it); outputs: Clock out
    bool alwaysStart = false;  // outputs: Start (never Continue) when playback starts anywhere
    double offsetMs = 0;    // outputs: its clock and Start/Stop this much later (negative: earlier)
    int slot = -1;          // its number for good in this session (events: Event::port)
    bool open = false;      // opened by its backend
    Backend* backend = nullptr;
    Found found;
    std::string label() const;  // its name as shown (with "— not connected")
};

class Hub {
public:
    Hub();
    ~Hub();
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    void addBackend(std::unique_ptr<Backend> b);  // (start() adds ALSA's and JACK's; tests a fake)
    // the output thread and (system) the system's backends: ALSA's, JACK's if a JACK port is on (GUI thread, once)
    void start(bool system = true);
    void stop();         // every port closed (outputs: All Notes Off first), the threads stopped
    bool running() const { return running_; }
    // JACK MIDI's ports (and its transport: jackBackend()), from now on, if a JACK server runs; true if so
    bool useJack();
    Backend* jack() const { return jack_; }

    // GUI thread, every frame: looks for ports again every RESCAN_S (or when announced), opens those
    // that came back; true when the list changed (a port came, went or was opened)
    bool tick(double now);
    void rescan();       // now
    const std::vector<Port>& ports() const { return ports_; }
    const Port* find(const std::string& key, bool input) const;
    const Port* bySlot(int slot) const;
    // a port's settings (made in the list if it isn't there yet, remembered as "not connected")
    void setOn(const std::string& key, bool input, bool on);
    void setClock(const std::string& key, bool input, bool on);
    void setAlwaysStart(const std::string& key, bool on);
    void setOffsetMs(const std::string& key, double ms);
    bool anyInputOpen() const;
    std::string openInputs() const;   // their names, ", " between ("" none)

    Json settings() const;            // {"in": {key: {on, clock}}, "out": {key: {on, clock, start, offset}}}
    void load(const Json& j);         // (replaces the settings; ports open or close to match)
    std::function<void()> changed;    // the settings changed (the main window saves them)

    // (GUI thread) what arrived from every input that is On since the last call (appended, oldest first);
    // lost: slots of inputs that went away while open (their notes must be let go), since the last call
    size_t poll(std::vector<midi_in::Event>& out, std::vector<int>* lost = nullptr);
    // (GUI thread) a message to an output now (notes played live into an External synth track)
    void sendNow(int slot, const uint8_t* bytes, int n);
    // the audio thread's half: what the engine plays into (Engine::setMidiOut); its clock ports follow the list
    std::shared_ptr<midi_out::Out> out;
    int sent() const { return sent_.load(); }  // messages sent (tests, the dialog)

private:
    std::vector<std::unique_ptr<Backend>> backends_;
    std::vector<Port> ports_;
    int nextSlot_ = 0;
    bool running_ = false, scanned_ = false;
    double lastScan_ = -1e9;
    std::vector<int> lost_;
    // the output thread
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::mutex nowLock_;
    std::vector<std::pair<int, std::vector<uint8_t>>> now_;  // sendNow's, for the output thread
    Backend* jack_ = nullptr;
    bool jackTried_ = false;
    std::vector<Backend*> slotBackend_;  // slot -> its backend while open (read by the output thread under nowLock_)
    std::atomic<int> sent_{0};
    Port& entry(const std::string& key, bool input);
    void apply(Port& p);       // open or close it to match its settings
    void closePort(Port& p, bool notesOff);
    void publish();            // the output ports' clock settings to `out`
    void outputLoop();
};

}  // namespace midi_ports
