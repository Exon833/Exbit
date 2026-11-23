// WASAPI (Windows' sound devices, audio/devices_win.cpp): the device lists, the default output playing a quiet tone
// for a second (callback timing measured: the period, its jitter, frames against the clock), the buffer sizes asked
// for, exclusive mode (or its fallback to shared), the default input, opening and closing over and over, the worst
// case of reopening as fast as possible, and a device lost (another stream takes it exclusively) and found again.
// Without a device it checks that everything says so instead of failing. Windows only (elsewhere: nothing to do).
#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "audio/devices.h"
#include "check.h"

using namespace audio;
using clock_ = std::chrono::steady_clock;

namespace {

const char* kType = "WASAPI";

struct Timing {
    std::vector<double> at;  // when each callback came (s)
    std::atomic<int> calls{0}, frames{0}, badFrames{0};
    int expect = 0;
    double phase = 0;
};

double now() { return std::chrono::duration<double>(clock_::now().time_since_epoch()).count(); }

std::unique_ptr<Stream> openOut(Timing& t, int blocksize, bool exclusive, std::string& err, bool tone = true) {
    StreamConfig cfg;
    cfg.driverType = kType;
    cfg.outputDevice = defaultOutput(kType);
    cfg.blocksize = blocksize;
    cfg.exclusive = exclusive;
    t.at.reserve(20000);
    auto self = std::make_shared<std::atomic<Stream*>>(nullptr);  // (the callback learns its stream's channels and rate)
    auto cb = [&t, self, tone](const float* in, float* out, int frames) {
        (void)in;
        int i = t.calls++;
        if (i < (int)t.at.capacity()) t.at.push_back(now());
        t.frames += frames;
        if (t.expect && frames != t.expect) t.badFrames++;
        Stream* me = self->load();
        if (!tone || !me) return;
        int ch = me->outChannels(), sr = me->sampleRate();
        for (int f = 0; f < frames; f++) {
            float v = 0.05f * (float)std::sin(t.phase);  // (-26 dB: quiet)
            t.phase += 2 * M_PI * 440.0 / sr;
            for (int c = 0; c < ch; c++) out[f * ch + c] = v;
        }
    };
    auto s = Stream::open(cfg, cb, err);
    *self = s.get();
    if (s) t.expect = s->blocksize();
    return s;
}

void report(const char* what, const Timing& t, const Stream& s, double seconds) {
    std::vector<double> d;
    for (size_t i = 1; i < t.at.size(); i++) d.push_back((t.at[i] - t.at[i - 1]) * 1000);
    std::sort(d.begin(), d.end());
    double period = 1000.0 * s.blocksize() / s.sampleRate();
    if (d.empty()) {
        printf("   %s: no callbacks\n", what);
        return;
    }
    double avg = 0;
    for (double x : d) avg += x;
    avg /= d.size();
    printf("   %s: %d callbacks of %d frames (%.2f ms) in %.2f s; interval min %.2f / median %.2f / p99 %.2f / max %.2f "
           "ms (avg %.2f); %d frames = %.3f s; %d underflows; latency %.2f ms out, %.2f ms in\n",
           what, t.calls.load(), s.blocksize(), period, seconds, d.front(), d[d.size() / 2], d[d.size() * 99 / 100],
           d.back(), avg, t.frames.load(), (double)t.frames / s.sampleRate(), s.underflows.load(),
           s.outputLatency() * 1000, s.inputLatency() * 1000);
}

void listing() {
    auto types = driverTypes();
    printf("driver types: %zu\n", types.size());
    for (auto& t : types) printf("   %s: %s\n", t.key.c_str(), t.description.c_str());
    auto outs = outputDevices(kType), ins = inputDevices(kType);
    printf("WASAPI: %zu outputs, %zu inputs; default output \"%s\", input \"%s\"\n", outs.size(), ins.size(),
           defaultOutput(kType).c_str(), defaultInput(kType).c_str());
    for (auto& d : outs) printf("   out %-60s %-40s %d ch %d Hz\n", d.id.c_str(), d.name.c_str(), d.maxOutputs, d.defaultSampleRate);
    for (auto& d : ins) printf("   in  %-60s %-40s %d ch %d Hz\n", d.id.c_str(), d.name.c_str(), d.maxInputs, d.defaultSampleRate);
    for (auto& d : outs) {
        DeviceInfo i;
        CHECK(deviceInfo(kType, d.id, i));
        CHECK(i.maxOutputs > 0 && i.defaultSampleRate > 0);
    }
    if (!outs.empty()) CHECK(outs[0].id == "default" && defaultOutput(kType) == "default");
    CHECK(types.empty() == outs.empty());
    DeviceInfo none;
    CHECK(!deviceInfo(kType, "{no-such-device}", none));
    CHECK(!deviceInfo("ALSA", "default", none));
}

}  // namespace

int main() {
    listing();
    std::string err;
    if (defaultOutput(kType).empty()) {  // (no device: opening says so, nothing breaks)
        StreamConfig cfg;
        cfg.driverType = kType;
        cfg.outputDevice = "default";
        CHECK(!Stream::open(cfg, [](const float*, float*, int) {}, err) && !err.empty());
        printf("no output device: \"%s\"\n", err.c_str());
        TEST_MAIN_END();
    }

    // the default output at the driver's own period, a tone for a second
    {
        Timing t;
        auto s = openOut(t, 0, false, err);
        CHECK(s != nullptr);
        if (!s) printf("can't open: %s\n", err.c_str());
        if (s) {
            printf("default output: %s, %d Hz, %d ch, %d-frame blocks, %.2f ms output latency, exclusive %d\n",
                   s->deviceName().c_str(), s->sampleRate(), s->outChannels(), s->blocksize(), s->outputLatency() * 1000,
                   (int)s->exclusive);
            double t0 = now();
            CHECK(s->start());
            std::this_thread::sleep_for(std::chrono::seconds(1));
            s->stop();
            double secs = now() - t0;
            report("tone", t, *s, secs);
            CHECK(t.badFrames == 0);
            CHECK(t.calls > 10);
            double heard = (double)t.frames / s->sampleRate();
            CHECK(heard > secs * 0.85 && heard < secs * 1.15 + s->outputLatency());  // (frames keep pace with the clock)
            CHECK(!s->lost);
            int after = t.calls;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CHECK(t.calls == after);  // (stopped: no more callbacks)
        }
    }

    // buffer sizes asked for
    for (int bs : {32, 64, 128, 256, 480, 1024, 2048}) {  // (32: the lowest the driver allows)
        Timing t;
        auto s = openOut(t, bs, false, err, false);
        CHECK(s != nullptr);
        if (!s) continue;
        CHECK(s->start());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        s->stop();
        char what[64];
        snprintf(what, sizeof what, "asked %d", bs);
        report(what, t, *s, 0.3);
        CHECK(t.badFrames == 0 && t.calls > 0);
        if (bs >= 1024) CHECK(s->blocksize() == bs);  // (big buffers, for heavy projects, are had as asked)
        CHECK(s->blocksize() <= std::max(bs, 480));
    }

    // exclusive mode (or shared, if the device won't)
    {
        Timing t;
        auto s = openOut(t, 0, true, err, false);
        CHECK(s != nullptr);
        if (s) {
            printf("exclusive asked: %s\n", s->exclusive ? "exclusive" : "fell back to shared");
            CHECK(s->start());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            s->stop();
            report(s->exclusive ? "exclusive" : "shared (fallback)", t, *s, 0.5);
            CHECK(t.calls > 0 && t.badFrames == 0);
        }
    }

    // the default input
    if (!defaultInput(kType).empty()) {
        StreamConfig cfg;
        cfg.driverType = kType;
        cfg.inputDevice = defaultInput(kType);
        std::atomic<int> calls{0}, frames{0}, nulls{0};
        auto s = Stream::open(cfg, [&](const float* in, float*, int n) {
            calls++, frames += n;
            if (!in) nulls++;
        }, err);
        CHECK(s != nullptr);
        if (!s) printf("input can't open: %s\n", err.c_str());
        if (s) {
            CHECK(s->inChannels() > 0 && s->outChannels() == 0);
            CHECK(s->start());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            s->stop();
            printf("   input: %s, %d ch, %d Hz, %d-frame blocks: %d callbacks, %.3f s, %.2f ms latency, %d glitches\n",
                   s->deviceName().c_str(), s->inChannels(), s->sampleRate(), s->blocksize(), calls.load(),
                   (double)frames / s->sampleRate(), s->inputLatency() * 1000, s->underflows.load());
            CHECK(calls > 0 && nulls == 0);
            CHECK((double)frames / s->sampleRate() > 0.3);
        }
    }

    // opened and closed over and over (started, a few buffers, stopped)
    {
        int ok = 0;
        double t0 = now();
        for (int i = 0; i < 30; i++) {
            Timing t;
            auto s = openOut(t, 0, false, err, false);
            if (!s) continue;
            s->start();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            s->stop();
            ok += t.calls > 0;
        }
        printf("   30 open/start/stop/close: %d played, %.1f ms each\n", ok, (now() - t0) * 1000 / 30);
        CHECK(ok == 30);
    }
    // the worst case: reopened as fast as it goes (no wait), started and stopped at once, exclusive in between
    {
        int opened = 0;
        double t0 = now();
        for (int i = 0; i < 200; i++) {
            Timing t;
            auto s = openOut(t, i % 3 == 0 ? 0 : 64 << (i % 4), i % 7 == 0, err, false);
            if (!s) continue;
            opened++;
            if (i % 2) {
                s->start();
                s->stop();
                s->start();  // (again after a stop)
            }
        }  // (destroyed while running half the time)
        printf("   200 rapid reopens: %d opened, %.2f ms each\n", opened, (now() - t0) * 1000 / 200);
        CHECK(opened == 200);
        Timing t;  // still works afterwards
        auto s = openOut(t, 0, false, err, false);
        CHECK(s && s->start());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (s) s->stop();
        CHECK(t.calls > 0);
    }

    // a device lost: another stream takes it exclusively (shared streams are cut off), then lets it go
    {
        Timing t;
        auto s = openOut(t, 0, false, err, false);
        CHECK(s && s->start());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Timing tx;
        auto x = openOut(tx, 0, true, err, false);
        bool took = x && x->exclusive && x->start();
        if (!took) {
            printf("   device-lost case skipped: exclusive mode not allowed here\n");
        } else {
            for (int i = 0; i < 40 && !s->lost; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            printf("   lost while another program has it exclusively: %s\n", s->lost ? "yes" : "no");
            CHECK(s->lost);
            int before = t.calls;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            printf("   callbacks while lost (silent, at the stream's pace): %d in 0.5 s\n", t.calls - before);
            CHECK(t.calls - before > 0);  // (the transport goes on)
            x->stop();
            x.reset();
            for (int i = 0; i < 60 && s->lost; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            printf("   found again: %s\n", s->lost ? "no" : "yes");
            CHECK(!s->lost);
            before = t.calls;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(t.calls - before > 0 && t.badFrames == 0);
        }
        if (s) s->stop();
    }
    TEST_MAIN_END();
}
#else
#include <cstdio>
int main() {
    printf("test_wasapi_win: Windows only\n");
    return 0;
}
#endif
