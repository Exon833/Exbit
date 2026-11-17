#include "audio/midi_cc.h"

#include <algorithm>
#include <atomic>

namespace midi_cc {
namespace {

struct Table {
    std::atomic<int32_t> raw[COUNT];
    std::atomic<int32_t> last{-1};
    std::atomic<uint32_t> moves{0};
    Table() {
        for (auto& r : raw) r.store(0, std::memory_order_relaxed);
    }
};

Table& table() {
    static Table t;  // (made on first use, thread-safely)
    return t;
}

void note(int controller, int value) {
    Table& t = table();
    t.raw[controller].store(value, std::memory_order_relaxed);
    t.last.store(controller, std::memory_order_relaxed);
    t.moves.fetch_add(1, std::memory_order_release);
}

}  // namespace

void control(int number, int value) {
    if (number < 0 || number > 127) return;
    note(number, std::max(0, std::min(127, value)));
}

void pitchBend(int value) { note(PITCH_BEND, std::max(-8192, std::min(8191, value))); }

int raw(int controller) {
    if (controller < 0 || controller >= COUNT) return 0;
    return table().raw[controller].load(std::memory_order_relaxed);
}

double bend() {
    int v = raw(PITCH_BEND);
    return v >= 0 ? v / 8191.0 : v / 8192.0;
}

double value(int controller) {
    if (controller == PITCH_BEND) return (bend() + 1) / 2;
    if (controller < 0 || controller >= COUNT) return 0;
    return raw(controller) / 127.0;
}

int last() { return table().last.load(std::memory_order_relaxed); }

uint32_t moves() { return table().moves.load(std::memory_order_acquire); }

void reset() {
    Table& t = table();
    for (auto& r : t.raw) r.store(0, std::memory_order_relaxed);
    t.last.store(-1, std::memory_order_relaxed);
}

}  // namespace midi_cc
