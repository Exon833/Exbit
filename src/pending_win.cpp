// Windows: stand-ins for the sources not ported yet (the Makefile's WIN_PENDING), so the program builds and runs
// without them: no MIDI ports, no Wine (Windows plugins load directly there), no terminal UI, and no ALSA, JACK or
// PulseAudio (audio/devices.cpp lists none: the program starts with no sound device). Whoever ports one of those
// files takes it off WIN_PENDING and its stand-ins out of here.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "audio/devices.h"
#include "audio/midi_in.h"
#include "audio/midi_out.h"
#include "audio/midi_ports.h"
#include "plugins/wine.h"
#include "tui/app.h"

// --- audio/devices_{jack,alsa,pulse}.cpp: no such drivers on Windows ---
namespace audio::detail {
bool jackAvailable() { return false; }
std::vector<DeviceInfo> jackDevices(bool) { return {}; }
bool jackDeviceInfo(const std::string&, DeviceInfo&, bool) { return false; }
std::string jackDefault(bool) { return std::string(); }
std::unique_ptr<Stream> jackOpen(const StreamConfig&, Callback, std::string& error) {
    error = "no sound devices on Windows yet";
    return nullptr;
}
bool alsaShared(const std::string&) { return false; }
std::vector<DeviceInfo> alsaDevices(bool) { return {}; }
bool alsaDeviceInfo(const std::string&, DeviceInfo&, bool) { return false; }
std::unique_ptr<Stream> alsaOpen(const StreamConfig&, Callback, std::string& error) {
    error = "no sound devices on Windows yet";
    return nullptr;
}
bool pulseAvailable() { return false; }
std::vector<DeviceInfo> pulseDevices(bool) { return {}; }
bool pulseDeviceInfo(const std::string&, DeviceInfo&, bool) { return false; }
std::string pulseDefault(bool) { return std::string(); }
std::unique_ptr<Stream> pulseOpen(const StreamConfig&, Callback, std::string& error) {
    error = "no sound devices on Windows yet";
    return nullptr;
}
}  // namespace audio::detail

// --- audio/midi_in.cpp: its system-independent part (as there), no ALSA ports ---
namespace midi_in {
Event Event::noteOn(int note, int velocity, int channel, double time) {
    Event e;
    e.type = velocity > 0 ? NoteOn : NoteOff;
    e.data1 = (uint8_t)note;
    e.data2 = (uint8_t)velocity;
    e.channel = (uint8_t)channel;
    e.time = time;
    return e;
}

Event Event::noteOff(int note, int channel, double time) {
    Event e;
    e.type = NoteOff;
    e.data1 = (uint8_t)note;
    e.channel = (uint8_t)channel;
    e.time = time;
    return e;
}

void Sustain::handle(const Event& e, std::vector<Event>& out) {
    if (e.type == Event::Control && e.data1 == SUSTAIN) {
        if (e.data2 >= 64) pedal_ = true;
        else pedalUp(out, e.time);
        return;
    }
    if (e.type == Event::Control && (e.data1 == ALL_NOTES_OFF || e.data1 == ALL_SOUND_OFF)) {
        releaseAll(out, e.time);
        out.push_back(e);
        return;
    }
    if (e.type == Event::NoteOff && pedal_ && sounding_.count(e.data1)) {
        sustained_.insert(e.data1);
        return;
    }
    if (e.type == Event::NoteOn && sustained_.count(e.data1)) {
        sustained_.erase(e.data1);
        out.push_back(Event::noteOff(e.data1, channel_[e.data1], e.time));
    }
    if (e.type == Event::NoteOn) {
        sounding_.insert(e.data1);
        channel_[e.data1] = e.channel;
    } else if (e.type == Event::NoteOff) {
        sounding_.erase(e.data1);
    }
    out.push_back(e);
}

void Sustain::pedalUp(std::vector<Event>& out, double time) {
    pedal_ = false;
    for (int note : sustained_) {
        sounding_.erase(note);
        out.push_back(Event::noteOff(note, channel_[note], time));
    }
    sustained_.clear();
}

void Sustain::releaseAll(std::vector<Event>& out, double time) {
    pedal_ = false;
    sustained_.clear();
    for (int note : sounding_) out.push_back(Event::noteOff(note, channel_[note], time));
    sounding_.clear();
}
}  // namespace midi_in

// --- audio/midi_ports.cpp: a hub with no ports ---
namespace midi_ports {
std::string Port::label() const { return present ? key : key + " — not connected"; }
Hub::Hub() : out(std::make_shared<midi_out::Out>()) {}
Hub::~Hub() {}
void Hub::addBackend(std::unique_ptr<Backend> b) { backends_.push_back(std::move(b)); }
void Hub::start(bool) { running_ = true; }
void Hub::stop() { running_ = false; }
bool Hub::useJack() { return false; }
bool Hub::tick(double) { return false; }
void Hub::rescan() {}
const Port* Hub::find(const std::string&, bool) const { return nullptr; }
const Port* Hub::bySlot(int) const { return nullptr; }
void Hub::setOn(const std::string&, bool, bool) {}
void Hub::setClock(const std::string&, bool, bool) {}
void Hub::setAlwaysStart(const std::string&, bool) {}
void Hub::setOffsetMs(const std::string&, double) {}
bool Hub::anyInputOpen() const { return false; }
std::string Hub::openInputs() const { return std::string(); }
Json Hub::settings() const { return Json::object(); }
void Hub::load(const Json&) {}
size_t Hub::poll(std::vector<midi_in::Event>&, std::vector<int>*) { return 0; }
void Hub::sendNow(int, const uint8_t*, int) {}
}  // namespace midi_ports

// --- plugins/wine.cpp and bridged.cpp: Wine is Linux's; on Windows plugins load in the program itself ---
namespace wine {
bool isPrefix(const std::string&) { return false; }
std::string defaultPrefix() { return std::string(); }
std::vector<std::string> prefixes() { return {}; }
void setPrefixes(const std::vector<std::string>&) {}
std::string prefixOf(const std::string&) { return std::string(); }
std::string prefixFor(const std::string&) { return std::string(); }
std::vector<std::string> vst3Folders(const std::vector<std::string>&) { return {}; }
std::vector<std::string> vst2UsualFolders(const std::vector<std::string>&) { return {}; }
std::string prefixArch(const std::string&) { return std::string(); }
std::string winePath() { return std::string(); }
std::string automaticWine(const std::string&, std::string& source) {
    source.clear();
    return std::string();
}
void setWinePrograms(const std::map<std::string, std::string>&) {}
std::string wineVersion(const std::string&) { return std::string(); }
int wineMajor(const std::string&) { return 0; }
bool available(std::string& why) {
    why = "Wine isn't used on Windows";
    return false;
}
int startPrefix(const std::string&, const std::string&, std::string& why) {
    why = "Wine isn't used on Windows";
    return -1;
}
int startServerWait(const std::string&, const std::string&, std::string& why) {
    why = "Wine isn't used on Windows";
    return -1;
}
int startInstaller(const std::string&, const std::string&, std::string& why) {
    why = "Wine isn't used on Windows";
    return -1;
}
bool finished(int, int& code) {
    code = -1;
    return true;
}
void stop(int) {}
}  // namespace wine

namespace plug {
int bridgedHostPid(Plugin&) { return -1; }
bool isBridged(const Plugin&) { return false; }
std::vector<std::string> bridgedDraggedFiles(Plugin&) { return {}; }
std::string bridgedDropFiles(Plugin&, const std::vector<std::string>&, int, int) { return std::string(); }
bool bridgedHostEnded(const Plugin&) { return false; }
void endBridgedHost(Plugin&) {}
}  // namespace plug

// --- tui/: the terminal UI ---
namespace tui {
int run(const std::vector<std::string>&) {
    fprintf(stderr, "Tiny DAW: the terminal UI (--tui) isn't on Windows yet\n");
    return 1;
}
}  // namespace tui
