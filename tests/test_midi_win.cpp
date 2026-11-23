// Windows' MIDI ports (WinMM, audio/midi_win.cpp): the port lists (unique, stable names), what a driver hands over
// parsed into events with their timestamps, every output opened and played into (a note, MIDI clock, Start/Stop),
// the Hub listing and opening them (several at once), opening a port that isn't there, the output thread's wait
// precision (high-resolution timer) and wake, and a loopback when a pair of virtual ports exists (loopMIDI:
// "x" in and "x" out). Windows only (elsewhere: nothing to do).
#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <thread>

#include "audio/midi_in.h"
#include "audio/midi_out.h"
#include "audio/midi_ports.h"
#include "check.h"

namespace midi_ports {
void winmmFake(Backend* b, int slot, uint32_t packed, uint32_t ms, double start);
}

namespace {
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint32_t pack(int a, int b = 0, int c = 0) { return (uint32_t)(a | b << 8 | c << 16); }
}  // namespace

int main() {
    auto ins = midi_in::ports(), outs = midi_in::outputs();
    printf("WinMM: %zu inputs, %zu outputs\n", ins.size(), outs.size());
    for (auto& p : ins) printf("   in  %d \"%s\"\n", p.client, p.name.c_str());
    for (auto& p : outs) printf("   out %d \"%s\"\n", p.client, p.name.c_str());
    CHECK(outs.size() == midiOutGetNumDevs() && ins.size() == midiInGetNumDevs());
    for (auto* list : {&ins, &outs}) {  // (unique names, as remembered)
        std::set<std::string> names;
        for (auto& p : *list) {
            names.insert(p.stable);
            CHECK(!p.stable.empty() && p.stable == p.name);
        }
        CHECK(names.size() == list->size());
    }
    CHECK(midi_in::stableName("Microsoft GS Wavetable Synth") == "Microsoft GS Wavetable Synth");

    // what a driver hands over, parsed (the backend's own callback)
    {
        auto b = midi_ports::winmmBackend();
        CHECK(b != nullptr);
        double start = now() - 0.5;
        midi_ports::winmmFake(b.get(), 3, pack(0x91, 60, 100), 400, start);  // note on, ch 2, 100 ms ago
        midi_ports::winmmFake(b.get(), 3, pack(0x91, 60, 0), 450, start);    // note on vel 0: off
        midi_ports::winmmFake(b.get(), 3, pack(0xB0, 64, 127), 460, start);
        midi_ports::winmmFake(b.get(), 3, pack(0xE0, 0, 64), 470, start);    // bend: centre
        midi_ports::winmmFake(b.get(), 3, pack(0xF8), 480, start);           // clock
        midi_ports::winmmFake(b.get(), 3, pack(0xF2, 5, 1), 490, start);     // song position 133
        midi_ports::winmmFake(b.get(), 3, pack(0xFA), 99999, start);         // start, stamped in the future
        midi_ports::winmmFake(b.get(), 3, pack(0x45, 1, 1), 495, start);     // (no status byte: dropped)
        std::vector<midi_in::Event> ev;
        b->poll(ev);
        CHECK(ev.size() == 7);
        if (ev.size() == 7) {
            using E = midi_in::Event;
            CHECK(ev[0].type == E::NoteOn && ev[0].channel == 1 && ev[0].data1 == 60 && ev[0].data2 == 100 && ev[0].port == 3);
            CHECK_NEAR(ev[0].time, start + 0.4, 1e-6);
            CHECK(ev[1].type == E::NoteOff && ev[1].data1 == 60);
            CHECK(ev[2].type == E::Control && ev[2].data1 == 64 && ev[2].data2 == 127);
            CHECK(ev[3].type == E::PitchBend && ev[3].bend == 0);
            CHECK(ev[4].type == E::Clock);
            CHECK(ev[5].type == E::SongPos && ev[5].bend == 5 + (1 << 7));
            CHECK(ev[6].type == E::Start && ev[6].time <= now() && ev[6].time > now() - 0.1);
        }
        // a port that isn't there can't be opened; closing what isn't open does nothing
        midi_ports::Found f;
        f.key = "nothing";
        f.client = 4000;
        CHECK(!b->open(f, 9));
        f.input = false;
        CHECK(!b->open(f, 9));
        b->close(9);
        uint8_t m[3] = {0x90, 60, 1};
        b->send(9, m, 3, now());  // (not open: nothing)
    }

    // every output played into: a note, clock pulses, Start/Stop (the GS synth: quiet)
    {
        auto b = midi_ports::winmmBackend();
        std::vector<midi_ports::Found> found;
        b->list(found);
        int slot = 0, opened = 0;
        for (auto& f : found) {
            if (f.input) continue;
            if (!b->open(f, slot)) {
                printf("   out \"%s\" can't be opened (in use?)\n", f.key.c_str());
                continue;
            }
            opened++;
            uint8_t on[3] = {0x90, 72, 20}, off[3] = {0x80, 72, 0}, clock = 0xF8, st = 0xFA, sp = 0xFC;
            b->send(slot, &st, 1, now());
            b->send(slot, on, 3, now());
            for (int i = 0; i < 24; i++) b->send(slot, &clock, 1, now()), std::this_thread::sleep_for(std::chrono::milliseconds(5));
            b->send(slot, off, 3, now());
            b->send(slot, &sp, 1, now());
            b->close(slot);
            slot++;
        }
        printf("   %d outputs opened and played into\n", opened);
        CHECK(opened == (int)outs.size() || outs.empty());
    }

    // the output thread's wait: precise (high-resolution timer) and woken at once
    {
        midi_out::Out out;
        std::vector<double> late;
        for (int i = 0; i < 100; i++) {
            double want = 0.001 + (i % 5) * 0.0007, t0 = now();
            out.wait(want);
            late.push_back((now() - t0 - want) * 1000);
        }
        std::sort(late.begin(), late.end());
        printf("   wait() late by: median %.3f ms, p99 %.3f ms, max %.3f ms\n", late[50], late[98], late.back());
        CHECK(late.front() > -0.05);  // (never early)
        CHECK(late[50] < 1.0);        // (not the 15.6 ms system tick)
        double woken = 0;
        std::thread t([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            woken = now();
            out.wake();
        });
        double t0 = now();
        out.wait(1.0);
        double took = now() - t0, lag = (now() - woken) * 1000;
        t.join();
        printf("   woken after %.1f ms (%.3f ms after wake())\n", took * 1000, lag);
        CHECK(took < 0.5);
        out.wake();
        t0 = now();
        out.wait(1.0);  // (a wake before the wait counts)
        CHECK(now() - t0 < 0.1);
    }

    // the Hub: Windows' backend, every port listed; the outputs switched on (several at once) and played into
    {
        midi_ports::Hub hub;
        hub.start(true);
        CHECK(hub.ports().size() == ins.size() + outs.size());
        int on = 0;
        for (auto& p : outs) hub.setOn(p.stable, false, true), on++;
        int open = 0;
        for (auto& p : hub.ports()) {
            printf("   hub: %s %s%s\n", p.input ? "in " : "out", p.label().c_str(), p.open ? " (open)" : "");
            open += p.open;
        }
        CHECK(open >= on);  // (every input is on from the start; every output switched on)
        for (auto& p : hub.ports())
            if (!p.input && p.open) {
                uint8_t m[3] = {0x90, 64, 10}, o[3] = {0x80, 64, 0};
                hub.sendNow(p.slot, m, 3);
                hub.sendNow(p.slot, o, 3);
            }
        for (int i = 0; i < 50 && hub.sent() < 2 * on; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(hub.sent() >= 2 * on);
        hub.tick(1e9);  // (looked at again)
        hub.stop();
        CHECK(!hub.anyInputOpen());
    }

    // loopback through a pair of virtual ports of one name, if there are any (loopMIDI)
    for (auto& i : ins)
        for (auto& o : outs)
            if (i.name == o.name) {
                auto b = midi_ports::winmmBackend();
                midi_ports::Found fi, fo;
                fi.key = i.stable, fi.client = i.client, fi.input = true;
                fo.key = o.stable, fo.client = o.client, fo.input = false;
                if (!b->open(fi, 0) || !b->open(fo, 1)) continue;
                uint8_t m[3] = {0x92, 61, 77};
                double sent = now();
                b->send(1, m, 3, sent);
                std::vector<midi_in::Event> ev;
                for (int k = 0; k < 100 && ev.empty(); k++) b->poll(ev), std::this_thread::sleep_for(std::chrono::milliseconds(2));
                printf("   loopback \"%s\": %s\n", i.name.c_str(), ev.empty() ? "nothing came" : "came back");
                CHECK(!ev.empty() && ev[0].data1 == 61 && ev[0].channel == 2 && ev[0].port == 0);
                if (!ev.empty()) printf("   round trip %.2f ms\n", (ev[0].time - sent) * 1000);
            }
    TEST_MAIN_END();
}
#else
#include <cstdio>
int main() {
    printf("test_midi_win: Windows only\n");
    return 0;
}
#endif
