// Sound devices: the driver types and device lists, and short streams that write only silence
// (the callback leaves `out` as it arrives: zeroed). Never makes a sound.
//
// ALSA: tries "default", "pipewire", "pulse" (or $TINYDAW_TEST_ALSA_DEVICE); when none opens it
// uses "null" (no clock there, so only the frame counts are checked).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "audio/devices.h"
#include "check.h"

using namespace audio;

namespace {

struct Counts {
    std::atomic<int> calls{0};
    std::atomic<int> badFrames{0};
    std::atomic<int> notZeroed{0};
    std::atomic<int> frames{0};
};

// runs a stream for `seconds`; returns false if it couldn't open
bool runStream(const StreamConfig& cfg, double seconds, Counts& c, std::unique_ptr<Stream>& keep, std::string& error) {
    int expect = cfg.blocksize;
    auto cb = [&c, expect](const float* in, float* out, int frames) {
        (void)in;
        c.calls++;
        c.frames += frames;
        if (expect && frames != expect) c.badFrames++;
        if (out && out[0] != 0.f) c.notZeroed++;
        // writes nothing: silence
    };
    keep = Stream::open(cfg, cb, error);
    if (!keep) return false;
    if (cfg.blocksize) {
        CHECK(keep->blocksize() > 0);
        if (keep->blocksize() != cfg.blocksize)
            printf("   (the device chose %d frames per block)\n", keep->blocksize());
    }
    CHECK(keep->start());
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    keep->stop();
    return true;
}

void listDriver(const std::string& key) {
    auto outs = outputDevices(key);
    auto ins = inputDevices(key);
    printf("%s (%s): %zu outputs, %zu inputs; default output \"%s\", default input \"%s\"\n", key.c_str(),
           driverLabel(key).c_str(), outs.size(), ins.size(), defaultOutput(key).c_str(), defaultInput(key).c_str());
    for (auto& d : outs)
        printf("   out %-34s %-50s %d ch %d Hz\n", d.id.c_str(), d.name.c_str(), d.maxOutputs, d.defaultSampleRate);
    for (auto& d : ins)
        printf("   in  %-34s %-50s %d ch %d Hz\n", d.id.c_str(), d.name.c_str(), d.maxInputs, d.defaultSampleRate);
    for (auto& d : outs) {
        CHECK(!d.id.empty());
        CHECK(d.maxOutputs > 0);
        CHECK(d.defaultSampleRate > 0);
    }
    if (!outs.empty()) {
        DeviceInfo info;
        CHECK(deviceInfo(key, outs[0].id, info));
        CHECK(info.id == outs[0].id);
        CHECK(info.maxOutputs > 0);
    }
}

}  // namespace

int main() {
    // --- driver types ---
    auto types = driverTypes();
    printf("driver types:");
    for (auto& t : types) printf(" %s", t.key.c_str());
    printf("\n");
    for (auto& t : types) {
        CHECK(!t.label.empty());
        CHECK(!t.description.empty());
        CHECK(t.key == "ALSA" || t.key == "PulseAudio" || t.key == "JACK");
    }
    CHECK(driverLabel("ALSA") == "ALSA");
    CHECK(driverLabel("PulseAudio") == "PulseAudio");
    CHECK(driverLabel("Something") == "Something");
    CHECK(minLatency("ALSA") == 0);
    CHECK(outputDevices("Nonsense").empty());
    std::string err;
    StreamConfig bad;
    bad.driverType = "Nonsense";
    bad.outputDevice = "x";
    CHECK(!Stream::open(bad, [](const float*, float*, int) {}, err));
    CHECK(!err.empty());

    listDriver("ALSA");
    bool havePulse = false;
    for (auto& t : types) havePulse |= t.key == "PulseAudio";
    if (havePulse) listDriver("PulseAudio");

    // --- ALSA: 48 kHz, 256 frames, 0.5 s of silence ---
    {
        std::vector<std::string> candidates = {"default", "pipewire", "pulse"};
        if (const char* e = getenv("TINYDAW_TEST_ALSA_DEVICE")) candidates = {e};
        StreamConfig cfg;
        cfg.driverType = "ALSA";
        cfg.sampleRate = 48000;
        cfg.blocksize = 256;
        cfg.outChannels = 2;
        Counts c;
        std::unique_ptr<Stream> s;
        bool opened = false, real = false;
        for (auto& dev : candidates) {
            cfg.outputDevice = dev;
            if (runStream(cfg, 0.5, c, s, err)) {
                opened = real = true;
                break;
            }
            printf("   ALSA \"%s\": %s\n", dev.c_str(), err.c_str());
        }
        if (!opened) {
            cfg.outputDevice = "null";
            opened = runStream(cfg, 0.1, c, s, err);
            if (!opened) printf("   ALSA \"null\": %s\n", err.c_str());
        }
        CHECK(opened);
        if (opened) {
            printf("ALSA \"%s\" (%s): %d callbacks, %d Hz, %d ch, %d frames, latency %.1f ms, underflows %d\n",
                   cfg.outputDevice.c_str(), s->deviceName().c_str(), c.calls.load(), s->sampleRate(), s->outChannels(),
                   s->blocksize(), s->outputLatency() * 1000, s->underflows.load());
            CHECK(c.calls > 0);
            CHECK(s->sampleRate() == 48000);
            CHECK(s->outChannels() == 2);
            CHECK(c.notZeroed == 0);
            CHECK(!s->lost);
            if (s->blocksize() == 256) CHECK(c.badFrames == 0);
            if (real) {
                // 0.5 s at 48 kHz in blocks of 256: ~94 (a block or two more for the start)
                double expected = 0.5 * 48000 / s->blocksize();
                CHECK(c.calls >= expected * 0.85 && c.calls <= expected * 1.2 + 3);
                CHECK(s->outputLatency() > 0 && s->outputLatency() < 0.2);
            }
            int after = c.calls;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK(c.calls == after);  // stopped: no more callbacks
            // and again (stop / start)
            CHECK(s->start());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            s->stop();
            CHECK(c.calls > after);
            s.reset();
        }
    }

    // --- PulseAudio: 48 kHz, 256 frames, 0.5 s of silence ---
    if (havePulse) {
        StreamConfig cfg;
        cfg.driverType = "PulseAudio";
        cfg.outputDevice = defaultOutput("PulseAudio");
        cfg.sampleRate = 48000;
        cfg.blocksize = 256;
        Counts c;
        std::unique_ptr<Stream> s;
        bool opened = runStream(cfg, 0.5, c, s, err);
        if (!opened) printf("   PulseAudio: %s\n", err.c_str());
        CHECK(opened);
        if (opened) {
            printf("PulseAudio \"%s\" (%s): %d callbacks, %d Hz, %d ch, %d frames, latency %.1f ms\n",
                   cfg.outputDevice.c_str(), s->deviceName().c_str(), c.calls.load(), s->sampleRate(), s->outChannels(),
                   s->blocksize(), s->outputLatency() * 1000);
            CHECK(s->sampleRate() == 48000);
            CHECK(s->blocksize() == 256);
            CHECK(c.badFrames == 0);
            CHECK(c.notZeroed == 0);
            CHECK(!s->lost);
            CHECK(c.calls >= 94 * 0.85 && c.calls <= 94 * 1.2 + 4);
            CHECK(s->outputLatency() > 0 && s->outputLatency() < 0.3);
            int after = c.calls;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK(c.calls == after);
            s.reset();
        }
        // a device that isn't there
        StreamConfig no = cfg;
        no.outputDevice = "no-such-sink";
        CHECK(!Stream::open(no, [](const float*, float*, int) {}, err));
        CHECK(!err.empty());
    }
    TEST_MAIN_END();
}
