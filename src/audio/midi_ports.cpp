// Every MIDI port at once: the list, its settings, and the output thread (midi_ports.h).
#include "audio/midi_ports.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/prctl.h>
#endif
#include <time.h>

#include <algorithm>
#include <cmath>

#include "audio/midi_out.h"
#include "core/util.h"

namespace midi_ports {
namespace {

double monoNow() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

}  // namespace

// --- Backend ---

size_t Backend::poll(std::vector<midi_in::Event>& out) {
    size_t n = 0;
    midi_in::Event e;
    while (in_.pop(e)) out.push_back(e), n++;
    return n;
}

void Backend::deliverBytes(int slot, const uint8_t* b, int n, double time) {
    if (n < 1) return;
    using midi_in::Event;
    Event e;
    uint8_t status = b[0];
    int d1 = n > 1 ? b[1] & 127 : 0, d2 = n > 2 ? b[2] & 127 : 0, ch = status & 15;
    switch (status & 0xF0) {
        case 0x90: e = Event::noteOn(d1, d2, ch, time); break;  // (velocity 0: a note-off)
        case 0x80: e = Event::noteOff(d1, ch, time); break;
        case 0xB0: e = Event::control(d1, d2, ch, time); break;
        case 0xE0:
            e.type = Event::PitchBend;
            e.channel = (uint8_t)ch;
            e.bend = (int16_t)((d1 | (d2 << 7)) - 8192);
            break;
        case 0xF0:
            if (status == 0xF8) e.type = Event::Clock;
            else if (status == 0xFA) e.type = Event::Start;
            else if (status == 0xFB) e.type = Event::Continue;
            else if (status == 0xFC) e.type = Event::Stop;
            else if (status == 0xF2) e.type = Event::SongPos, e.bend = (int16_t)(d1 | (d2 << 7));
            else return;  // (other system messages: not used)
            break;
        default: return;  // (aftertouch, program changes: not used)
    }
    e.time = time;
    e.port = (int16_t)slot;
    deliver(e);
}

// --- Port ---

std::string Port::label() const {
    std::string name = key;
    if (!present) name += " \xe2\x80\x94 not connected";
    return name;
}

// --- Hub ---

Hub::Hub() : out(std::make_shared<midi_out::Out>()) {}

Hub::~Hub() { stop(); }

void Hub::addBackend(std::unique_ptr<Backend> b) {
    if (!b) return;
    util::debugf("midi: %s ports\n", b->name());
    backends_.push_back(std::move(b));
    scanned_ = false;  // (looked at again at the next tick)
}

void Hub::start(bool system) {
    if (running_) return;
    running_ = true;
#ifdef _WIN32
    if (system) addBackend(winmmBackend());
#else
    if (system) addBackend(alsaBackend());
#endif
    bool jackWanted = false;
    for (auto& p : ports_) jackWanted = jackWanted || (p.on && isJack(p.key));
    if (jackWanted && system) useJack();
    quit_ = false;
    thread_ = std::thread([this] { outputLoop(); });
    rescan();
}

bool Hub::useJack() {
    if (jack_) return true;
    if (jackTried_) return false;
    jackTried_ = true;
    auto b = jackBackend();
    if (!b) return false;
    jack_ = b.get();
    addBackend(std::move(b));
    return true;
}

void Hub::stop() {
    for (auto& p : ports_)
        if (p.open) closePort(p, p.present);
    if (thread_.joinable()) {
        quit_ = true;
        out->wake();
        thread_.join();
    }
    std::lock_guard<std::mutex> g(nowLock_);
    slotBackend_.clear();
    backends_.clear();
    jack_ = nullptr;
    running_ = false;
}

bool Hub::tick(double now) {
    bool announced = false;
    for (auto& b : backends_) announced = b->changed() || announced;
    if (!announced && scanned_ && now - lastScan_ < RESCAN_S) return false;
    lastScan_ = now;
    std::vector<std::pair<std::string, bool>> before;
    for (auto& p : ports_) before.emplace_back(p.key, p.present && p.open);
    rescan();
    std::vector<std::pair<std::string, bool>> after;
    for (auto& p : ports_) after.emplace_back(p.key, p.present && p.open);
    return before != after;
}

void Hub::rescan() {
    scanned_ = true;
    std::vector<std::pair<Found, Backend*>> seen;
    for (auto& b : backends_) {
        std::vector<Found> found;
        b->list(found);
        for (auto& f : found) seen.emplace_back(f, b.get());
    }
    for (auto& p : ports_) p.present = false;
    for (auto& [f, b] : seen) {
        bool known = find(f.key, f.input) != nullptr;
        Port& p = entry(f.key, f.input);
        if (!known) {  // never seen before: an input listens at once (not a "Through" port: it echoes), an output waits
            p.on = f.input && f.key.find("Through") == std::string::npos;
            util::debugf("midi: new %s \"%s\" (%s)%s\n", f.input ? "input" : "output", f.key.c_str(), b->name(),
                         p.on ? ", listened to" : "");
        }
        // (the same port again with another address: closed and opened anew)
        if (p.open && (p.found.client != f.client || p.found.port != f.port || p.found.full != f.full)) closePort(p, false);
        p.present = true;
        p.found = f;
        p.backend = b;
    }
    for (auto& p : ports_) {
        if (p.open && !p.present) {  // unplugged while open
            util::debugf("midi: %s \"%s\" went away\n", p.input ? "input" : "output", p.key.c_str());
            closePort(p, false);
            if (p.input) lost_.push_back(p.slot);
        }
        apply(p);
    }
    publish();
}

Port& Hub::entry(const std::string& key, bool input) {
    for (auto& p : ports_)
        if (p.key == key && p.input == input) return p;
    Port p;
    p.key = key;
    p.input = input;
    p.slot = nextSlot_++;
    ports_.push_back(p);
    return ports_.back();
}

const Port* Hub::find(const std::string& key, bool input) const {
    for (auto& p : ports_)
        if (p.key == key && p.input == input) return &p;
    return nullptr;
}

const Port* Hub::bySlot(int slot) const {
    for (auto& p : ports_)
        if (p.slot == slot) return &p;
    return nullptr;
}

void Hub::apply(Port& p) {
    if (p.on && p.present && !p.open && p.backend) {
        p.open = p.backend->open(p.found, p.slot);
        util::debugf("midi: %s \"%s\" %s\n", p.input ? "input" : "output", p.key.c_str(),
                     p.open ? "open" : "can't be opened");
        if (p.open) {
            std::lock_guard<std::mutex> g(nowLock_);
            if ((int)slotBackend_.size() <= p.slot) slotBackend_.resize((size_t)p.slot + 1, nullptr);
            slotBackend_[(size_t)p.slot] = p.backend;
        }
    } else if (p.open && !(p.on && p.present)) {
        closePort(p, p.present);
    }
}

void Hub::closePort(Port& p, bool notesOff) {
    if (!p.open) return;
    std::lock_guard<std::mutex> g(nowLock_);
    if (!p.input && notesOff && p.backend) {  // every note on it stops (All Notes Off on each channel)
        for (int ch = 0; ch < 16; ch++) {
            uint8_t m[3] = {(uint8_t)(0xB0 | ch), 123, 0};
            p.backend->send(p.slot, m, 3, monoNow());
        }
        sent_ += 16;
    }
    if (p.backend) p.backend->close(p.slot);
    if (p.slot < (int)slotBackend_.size()) slotBackend_[(size_t)p.slot] = nullptr;
    p.open = false;
    util::debugf("midi: %s \"%s\" closed\n", p.input ? "input" : "output", p.key.c_str());
}

void Hub::publish() {
    std::vector<midi_out::ClockPort> clocks;
    for (auto& p : ports_)
        if (!p.input && p.open && p.clock) clocks.push_back({p.slot, p.alwaysStart, p.offsetMs / 1000.0});
    out->setClocks(clocks);
}

void Hub::setOn(const std::string& key, bool input, bool on) {
    Port& p = entry(key, input);
    if (p.on == on) return;
    p.on = on;
    if (on && isJack(key)) {  // (JACK's ports listed from now on: `p` may move as they come into the list)
        useJack();
        rescan();
    } else {
        apply(p);
    }
    publish();
    if (changed) changed();
}

void Hub::setClock(const std::string& key, bool input, bool on) {
    Port& p = entry(key, input);
    if (p.clock == on) return;
    p.clock = on;
    publish();
    if (changed) changed();
}

void Hub::setAlwaysStart(const std::string& key, bool on) {
    Port& p = entry(key, false);
    if (p.alwaysStart == on) return;
    p.alwaysStart = on;
    publish();
    if (changed) changed();
}

void Hub::setOffsetMs(const std::string& key, double ms) {
    Port& p = entry(key, false);
    ms = std::clamp(ms, -MAX_OFFSET_MS, MAX_OFFSET_MS);
    if (p.offsetMs == ms) return;
    p.offsetMs = ms;
    publish();
    if (changed) changed();
}

bool Hub::anyInputOpen() const {
    for (auto& p : ports_)
        if (p.input && p.open) return true;
    return false;
}

std::string Hub::openInputs() const {
    std::string s;
    for (auto& p : ports_)
        if (p.input && p.open) s += (s.empty() ? "" : ", ") + p.key;
    return s;
}

Json Hub::settings() const {
    Json in = Json::object(), outs = Json::object();
    for (auto& p : ports_) {
        Json j = Json::object();
        j.set("on", Json(p.on));
        j.set("clock", Json(p.clock));
        if (!p.input) {
            j.set("start", Json(p.alwaysStart));
            j.set("offset_ms", Json(p.offsetMs));
        }
        (p.input ? in : outs).set(p.key, j);
    }
    return Json::object({{"in", in}, {"out", outs}});
}

void Hub::load(const Json& j) {
    for (bool input : {true, false}) {
        const Json& side = j[input ? "in" : "out"];
        for (auto& [key, v] : side.members()) {
            if (key.empty() || !v.isObject()) continue;
            Port& p = entry(key, input);
            p.on = v.flag("on", false);
            p.clock = v.flag("clock", false);
            if (!input) {
                p.alwaysStart = v.flag("start", false);
                double ms = v.num("offset_ms", 0.0);
                p.offsetMs = std::isfinite(ms) ? std::clamp(ms, -MAX_OFFSET_MS, MAX_OFFSET_MS) : 0.0;
            }
            if (p.on && isJack(key) && running_) useJack();
        }
    }
    if (running_) rescan();
}

size_t Hub::poll(std::vector<midi_in::Event>& events, std::vector<int>* lost) {
    size_t from = events.size();
    for (auto& b : backends_) b->poll(events);
    // (what came from a port closed meanwhile isn't played)
    auto openInput = [this](int slot) {
        for (auto& p : ports_)
            if (p.slot == slot) return p.input && p.open;
        return false;
    };
    events.erase(std::remove_if(events.begin() + (std::ptrdiff_t)from, events.end(),
                                [&](const midi_in::Event& e) { return !openInput(e.port); }),
                 events.end());
    if (lost) {
        lost->insert(lost->end(), lost_.begin(), lost_.end());
        lost_.clear();
    }
    return events.size() - from;
}

void Hub::sendNow(int slot, const uint8_t* bytes, int n) {
    if (n < 1 || n > 3) return;
    {
        std::lock_guard<std::mutex> g(nowLock_);
        now_.emplace_back(slot, std::vector<uint8_t>(bytes, bytes + n));
    }
    out->wake();
}

void Hub::outputLoop() {
    // as close to each message's moment as the system allows: no timer slack, real-time priority where permitted
#ifdef _WIN32
    bool rt = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#else
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
    sched_param sp{};
    sp.sched_priority = 60;
    bool rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
#endif
    util::debugf("midi: the output thread runs %s\n", rt ? "at real-time priority" : "at normal priority");
    std::vector<midi_out::Event> pending;
    std::vector<std::pair<int, std::vector<uint8_t>>> now;
    pending.reserve(4096);
    auto backendOf = [this](int slot) -> Backend* {  // (nowLock_ held)
        return slot >= 0 && slot < (int)slotBackend_.size() ? slotBackend_[(size_t)slot] : nullptr;
    };
    while (!quit_) {
        midi_out::Event e;
        {
            std::lock_guard<std::mutex> g(nowLock_);
            now.swap(now_);
            for (auto& [slot, bytes] : now)
                if (Backend* b = backendOf(slot)) b->send(slot, bytes.data(), (int)bytes.size(), monoNow()), sent_++;
            now.clear();
            while (out->take(e)) {
                Backend* b = backendOf(e.slot);
                if (!b) continue;  // (a port closed meanwhile)
                if (b->timed()) {  // (it places it at its moment itself)
                    b->send(e.slot, e.b, e.n, e.when);
                    sent_++;
                } else {
                    pending.push_back(e);
                }
            }
        }
        std::stable_sort(pending.begin(), pending.end(),
                         [](const midi_out::Event& a, const midi_out::Event& b) { return a.when < b.when; });
        double t = monoNow();
        size_t done = 0;
        if (!pending.empty() && pending[0].when <= t + 20e-6) {
            std::lock_guard<std::mutex> g(nowLock_);
            for (; done < pending.size() && pending[done].when <= t + 20e-6; done++)
                if (Backend* b = backendOf(pending[done].slot)) {
                    b->send(pending[done].slot, pending[done].b, pending[done].n, pending[done].when);
                    sent_++;
                }
        }
        pending.erase(pending.begin(), pending.begin() + (std::ptrdiff_t)done);
        // sleep until the next message is due, or something new arrives
        double wait = pending.empty() ? 0.05 : std::min(0.05, pending[0].when - monoNow());
        if (wait > 0) out->wait(wait);
    }
}

}  // namespace midi_ports
