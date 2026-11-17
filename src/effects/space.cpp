// Space: a reverb that plays the sound of a room by convolution — every sample that comes in sets
// off the room's whole response to a click (its impulse response), and they all add up. The rooms
// are made here, by recipe (a small room, a chamber, a hall, a plate, a spring, a cathedral): a few
// early reflections off near walls, then a dense tail of filtered noise dying away, its highs dying
// faster than its lows; the left and right sides each from their own noise, so the room is wide.
// Each is made from a fixed seed: the same room every time, on every machine.
//
// Room, length (the room's decay made shorter or longer), pre-delay (a gap before the room
// answers), low cut and high cut shape the response itself: changing one makes a new response.
// Width (the wet sound's stereo spread) and mix (dry against wet) are applied as it plays.
//
// How it plays (uniformly partitioned convolution, from the textbooks): the response is cut into
// pieces of P = 256 frames. The first piece is played directly (a dot product per frame), so the
// reverb adds no latency at all; the others in the frequency domain: every P frames the newest
// 2P frames of input go through an FFT (audio/fft.h) into a ring of past spectra, each piece's
// spectrum is multiplied with the spectrum from as many blocks ago and summed, and one inverse
// FFT gives the next P frames of the rest of the reverb (overlap-save). A 3 s room at 48 kHz is
// 562 pieces: about 290 000 complex multiply-adds every 256 frames, whatever the host's buffer.
//
// New responses are made off the audio thread: a worker thread of its own watches the settings
// (every 25 ms) and builds the new response's pieces into one of three kernels it keeps, then hands
// it over by an atomic swap; the audio thread takes it at the next piece's boundary and fades from
// the old one to it over 20 ms (both play meanwhile, sharing the ring of input spectra, so the
// swap neither clicks nor loses the tail). The kernels are reused: the audio thread never allocates,
// frees or waits. Loading a state (setState: a project, undo) and preparing build at once, on the
// calling thread, so playback starts with the right room. (Automation of the shaping settings
// while exporting is heard as soon as the worker has built it: tens of milliseconds of real time.)
//
// It sleeps (Effect's sleep) only once its whole response has died away after the input went quiet.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "audio/fft.h"
#include "effects/builtin.h"

namespace effects {
namespace {

constexpr int P = 256;               // a piece of the response (frames)
constexpr double LONGEST_S = 8.0;    // the longest response, before its pre-delay
constexpr double LONGEST_PREDELAY_MS = 200.0;
constexpr double FADE_S = 0.02;      // a new response fades in over this long
constexpr int POLL_MS = 25;          // the worker looks at the settings this often
constexpr int KERNELS = 3;           // playing, fading out, being built (or ready)

// --- the rooms ----------------------------------------------------------------------------------

enum class Shape { Room, Plate, Spring };

struct Room {
    const char* name;
    Shape shape;
    double rt60;        // seconds for the tail to die by 60 dB (at length 100 %)
    double erFrom, erTo;  // the early reflections' span (s)
    int erCount;
    double erShare;     // of the energy
    double tailFrom;    // when the tail starts (s)
    double buildUp;     // how fast it grows dense (s)
    double brightHz;    // the tail's highs at first…
    double darkHz;      // …falling towards this,
    double darkenS;     // …this fast
};

const Room ROOMS[] = {
    {"Small room", Shape::Room, 0.6, 0.002, 0.025, 14, 0.40, 0.005, 0.010, 9000, 900, 0.30},
    {"Chamber", Shape::Room, 1.2, 0.004, 0.045, 18, 0.30, 0.010, 0.030, 8000, 700, 0.70},
    {"Hall", Shape::Room, 2.3, 0.012, 0.080, 22, 0.22, 0.018, 0.070, 7000, 500, 1.20},
    {"Plate", Shape::Plate, 1.8, 0, 0, 0, 0.0, 0.0, 0.002, 12000, 2500, 3.00},
    {"Spring", Shape::Spring, 1.6, 0, 0, 0, 0.0, 0.0, 0.004, 4500, 1500, 1.00},
    {"Cathedral", Shape::Room, 4.8, 0.020, 0.140, 26, 0.15, 0.030, 0.150, 5500, 350, 1.80},
};
constexpr int ROOM_COUNT = (int)(sizeof ROOMS / sizeof ROOMS[0]);

std::vector<std::string> roomNames() {
    std::vector<std::string> names;
    for (const auto& r : ROOMS) names.push_back(r.name);
    return names;
}

// xorshift64*: the same numbers everywhere (std's distributions differ between libraries)
struct Noise {
    uint64_t s;
    explicit Noise(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL) {}
    double next() {  // -1…1
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (double)((s * 2685821657736338717ULL) >> 11) * (2.0 / 9007199254740992.0) - 1.0;
    }
};

// a one-pole low-pass's coefficient for a cutoff
double onePole(double hz, double rate) { return std::exp(-2 * PI * std::min(hz, rate * 0.49) / rate); }

double energy(const std::vector<double>& x) {
    double e = 0;
    for (double v : x) e += v * v;
    return e;
}

// a second-order section run over a signal (transposed direct form II)
void filter(std::vector<double>& x, const Sos& s) {
    double z0 = 0, z1 = 0;
    for (double& v : x) {
        double in = v, out = s[0] * in + z0;
        z0 = s[1] * in - s[4] * out + z1;
        z1 = s[2] * in - s[5] * out;
        v = out;
    }
}

// The room's response, each side, scaled so its energy is 1 (the reverb about as loud as the
// sound for broadband sound). `scale`: its decay's length (1: as made).
void makeRoom(int index, double scale, double rate, std::vector<float> out[2]) {
    const Room& room = ROOMS[std::clamp(index, 0, ROOM_COUNT - 1)];
    double rt = room.rt60 * scale;
    double seconds = std::min(rt + room.tailFrom, LONGEST_S);
    size_t n = (size_t)std::max<double>(P, std::ceil(seconds * rate));
    std::vector<double> side[2];
    for (int c = 0; c < 2; c++) {
        Noise noise((uint64_t)(index * 7919 + 17) * 2 + (uint64_t)c);
        std::vector<double> tail(n, 0.0), early(n, 0.0);
        // the tail: noise through a low-pass whose cutoff falls as it dies (highs fade first),
        // growing dense over buildUp, dying by 60 dB over rt
        double lp = 0, a = 0;
        size_t from = (size_t)(room.tailFrom * rate);
        for (size_t t = from; t < n; t++) {
            double u = (double)(t - from) / rate;
            if ((t - from) % 16 == 0)
                a = onePole(room.darkHz + (room.brightHz - room.darkHz) * std::exp(-u / room.darkenS), rate);
            lp = (1 - a) * noise.next() + a * lp;
            tail[t] = lp * std::exp(-6.907755 * u / rt) * (1 - std::exp(-u / room.buildUp));
        }
        if (room.shape == Shape::Spring) {
            // a spring: its echoes come back every round trip (a little apart on the two sides),
            // each smeared into a falling chirp by the spring's dispersion (a chain of all-passes)
            std::vector<double> train(n, 0.0);
            double trip = c == 0 ? 0.0312 : 0.0371;
            for (int k = 0;; k++) {
                double t = k * trip;
                size_t at = (size_t)(t * rate);
                if (at >= n) break;
                train[at] = (k % 2 ? -0.85 : 1.0) * std::exp(-6.907755 * t / rt);
            }
            for (int stage = 0; stage < 48; stage++) {  // (first-order all-passes, coefficient 0.6)
                double x1 = 0, y1 = 0;
                for (double& v : train) {
                    double y = -0.6 * v + x1 + 0.6 * y1;
                    x1 = v;
                    y1 = y;
                    v = y;
                }
            }
            double et = energy(tail), es = energy(train);
            for (size_t t = 0; t < n; t++)
                tail[t] = train[t] / std::sqrt(std::max(es, 1e-30)) * std::sqrt(0.85) +
                          tail[t] / std::sqrt(std::max(et, 1e-30)) * std::sqrt(0.15);
            filter(tail, biquad(Kind::Highpass, 120, rate));
            filter(tail, biquad(Kind::Lowpass, std::min(4500.0, rate * 0.45), rate));
        }
        // the early reflections: single echoes off the near walls, sparser and softer as they come
        // later, each a short soft blip
        for (int i = 0; i < room.erCount; i++) {
            double place = std::pow((i + 0.5 + 0.4 * noise.next()) / room.erCount, 1.4);
            double t = room.erFrom + (room.erTo - room.erFrom) * std::clamp(place, 0.0, 1.0);
            size_t at = std::min(n - 1, (size_t)(t * rate));
            double amp = (0.6 + 0.4 * noise.next()) * std::exp(-2.5 * t / std::max(room.erTo, 1e-3));
            early[at] += noise.next() < 0 ? -amp : amp;
        }
        if (room.erCount) {
            double b = onePole(room.brightHz, rate), held = 0;
            for (double& v : early) v = held = (1 - b) * v + b * held;
        }
        double et = energy(tail), ee = energy(early);
        side[c].assign(n, 0.0);
        for (size_t t = 0; t < n; t++) {
            double v = tail[t] / std::sqrt(std::max(et, 1e-30)) * std::sqrt(1 - room.erShare);
            if (ee > 0) v += early[t] / std::sqrt(ee) * std::sqrt(room.erShare);
            side[c][t] = v;
        }
        filter(side[c], biquad(Kind::Highpass, 30, rate));  // (no rumble, no offset)
    }
    // its last tenth fades out (it ends quietly, wherever it was cut)
    size_t fade = std::max<size_t>(1, n / 10);
    double total = 0;
    for (int c = 0; c < 2; c++) {
        for (size_t i = 0; i < fade; i++)
            side[c][n - fade + i] *= 0.5 + 0.5 * std::cos(PI * (double)(i + 1) / (double)fade);
        total += energy(side[c]);
    }
    double gain = 1 / std::sqrt(std::max(total / 2, 1e-30));
    for (int c = 0; c < 2; c++) {
        out[c].resize(n);
        for (size_t t = 0; t < n; t++) out[c][t] = (float)(side[c][t] * gain);
    }
}

// --- the kernel: a response cut into pieces --------------------------------------------------------

struct Kernel {
    size_t frames = 0;            // the response's length
    std::vector<float> head[2];   // its first P frames, reversed (played directly)
    bool headSilent = true;
    int parts = 0, first = 0;     // the pieces after the head; the first that isn't silent
    // each piece's spectrum (bins 0…P-1 of a 2P FFT; bin 0's imaginary part holds bin P), piece-major
    std::vector<float> re[2], im[2];
};

// --- the settings a response is made from ------------------------------------------------------------

struct Settings {
    int room = 0;
    double length = 100, predelay = 0, lowCut = 20, highCut = 20000, rate = 0;
    uint64_t custom = 0;  // which set impulse (0: the room's)
    bool operator==(const Settings& o) const {
        return room == o.room && length == o.length && predelay == o.predelay && lowCut == o.lowCut &&
               highCut == o.highCut && rate == o.rate && custom == o.custom;
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};

// --- the effect --------------------------------------------------------------------------------------

class Space : public Effect {
public:
    Space()
        : Effect("space", "Space",
                 {choice("room", "Room", roomNames(), 2),
                  param("length", "Length", 25, 200, 100, "%"),
                  param("predelay", "Pre-delay", 0, LONGEST_PREDELAY_MS, 10, "ms"),
                  param("low_cut", "Low cut", 20, 1000, 100, "Hz", Scale::Log),
                  param("high_cut", "High cut", 1000, 20000, 20000, "Hz", Scale::Log),
                  param("width", "Width", 0, 100, 100, "%"),
                  param("mix", "Mix", 0, 100, 30, "%")}) {}

    ~Space() override {
        {
            std::lock_guard<std::mutex> lock(wakeMutex_);
            quit_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    int room = key("room"), length = key("length"), predelay = key("predelay"), lowCut = key("low_cut"),
        highCut = key("high_cut"), width = key("width"), mix = key("mix");

    // ---- the builder's side (the worker, or the main thread: under buildMutex_) ----

    std::mutex buildMutex_;
    Kernel kernels_[KERNELS];
    // which kernels the audio thread holds (bits 0…2) and the one ready for it (bits 4…: its index + 1)
    std::atomic<uint32_t> slots_{0};
    static int readyOf(uint32_t s) { return (int)(s >> 4) - 1; }
    double rate_ = 48000;                 // (sr, as the builder knows it)
    Settings built_;                      // what the newest kernel was made from
    std::vector<float> shaped_[2];        // its response
    std::vector<float> raw_[2];           // the room's response before shaping, and what it was made from
    Settings rawFrom_;
    std::vector<float> custom_[2];        // a response set directly
    uint64_t customId_ = 0;
    int customRoom_ = -1;                 // (the room chosen when it was set: choosing another drops it)

    std::thread worker_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    bool quit_ = false;

    // the settings now (custom_ dropped once another room is chosen) (under buildMutex_)
    Settings wanted() {
        Settings s;
        s.room = std::clamp((int)std::lround(v(room)), 0, ROOM_COUNT - 1);
        s.length = v(length);
        s.predelay = v(predelay);
        s.lowCut = v(lowCut);
        s.highCut = v(highCut);
        s.rate = rate_;
        if (customId_ && s.room != customRoom_) {
            customId_ = 0;
            custom_[0].clear();
            custom_[1].clear();
        }
        s.custom = customId_;
        return s;
    }

    size_t longest() const { return (size_t)std::ceil((LONGEST_S + LONGEST_PREDELAY_MS * 0.001) * rate_); }

    // the response for these settings: the room (or the impulse set), its length, pre-delay and cuts
    void shape(const Settings& s, std::vector<float> out[2]) {
        const std::vector<float>* src = raw_;
        if (s.custom) {
            src = custom_;
        } else {
            Settings key;
            key.room = s.room;
            key.length = s.length;
            key.rate = s.rate;
            if (raw_[0].empty() || key != rawFrom_) {
                makeRoom(s.room, s.length / 100, s.rate, raw_);
                rawFrom_ = key;
            }
        }
        size_t pre = (size_t)std::lround(std::clamp(s.predelay, 0.0, LONGEST_PREDELAY_MS) * 0.001 * s.rate);
        size_t n = std::max(src[0].size(), src[1].size());
        size_t keep = n;
        if (s.custom && s.length < 100) keep = (size_t)std::ceil(n * s.length / 100);  // (a set impulse: trimmed)
        keep = std::min(keep, longest() - std::min(longest(), pre));
        bool lowOn = s.lowCut > 20.5, highOn = s.highCut < std::min(19500.0, s.rate * 0.45);
        for (int c = 0; c < 2; c++) {
            std::vector<double> x(keep, 0.0);
            for (size_t i = 0; i < keep && i < src[c].size(); i++) x[i] = src[c][i];
            if (keep < n && keep > 0) {  // (cut short: its last tenth fades out)
                size_t fade = std::max<size_t>(1, keep / 10);
                for (size_t i = 0; i < fade; i++)
                    x[keep - fade + i] *= 0.5 + 0.5 * std::cos(PI * (double)(i + 1) / (double)fade);
            }
            if (lowOn) filter(x, biquad(Kind::Highpass, s.lowCut, s.rate));
            if (highOn) filter(x, biquad(Kind::Lowpass, s.highCut, s.rate));
            out[c].assign(pre + keep, 0.0f);
            for (size_t i = 0; i < keep; i++) out[c][pre + i] = (float)x[i];
        }
    }

    // a kernel from a response (any length up to longest())
    void cut(const std::vector<float> ir[2], Kernel& k) {
        size_t n = std::max(ir[0].size(), ir[1].size());
        auto at = [&](int c, size_t i) { return i < ir[c].size() ? (double)ir[c][i] : 0.0; };
        k.frames = n;
        k.headSilent = true;
        for (int c = 0; c < 2; c++) {
            k.head[c].assign(P, 0.0f);
            for (int i = 0; i < P; i++) {
                k.head[c][(size_t)(P - 1 - i)] = (float)at(c, (size_t)i);
                if (at(c, (size_t)i) != 0) k.headSilent = false;
            }
        }
        k.parts = n > (size_t)P ? (int)((n - P + P - 1) / P) : 0;
        k.first = k.parts;
        fft::Plan plan(2 * P);
        std::vector<double> a(2 * P), b(2 * P);
        std::vector<fft::cd> A(P + 1), B(P + 1), work(2 * P);
        for (int c = 0; c < 2; c++) {
            k.re[c].assign((size_t)k.parts * P, 0.0f);
            k.im[c].assign((size_t)k.parts * P, 0.0f);
        }
        for (int j = 0; j < k.parts; j++) {
            bool silent = true;
            for (int i = 0; i < 2 * P; i++) {
                size_t t = (size_t)P + (size_t)j * P + (size_t)i;
                a[(size_t)i] = i < P ? at(0, t) : 0.0;
                b[(size_t)i] = i < P ? at(1, t) : 0.0;
                if (a[(size_t)i] != 0 || b[(size_t)i] != 0) silent = false;
            }
            if (silent) continue;
            k.first = std::min(k.first, j);
            fft::realPair(plan, a.data(), b.data(), A.data(), B.data(), work);
            for (int c = 0; c < 2; c++) {
                const fft::cd* S = c == 0 ? A.data() : B.data();
                float* re = k.re[c].data() + (size_t)j * P;
                float* im = k.im[c].data() + (size_t)j * P;
                re[0] = (float)S[0].real();
                im[0] = (float)S[P].real();
                for (int i = 1; i < P; i++) {
                    re[i] = (float)S[i].real();
                    im[i] = (float)S[i].imag();
                }
            }
        }
    }

    // make the response for `s` and hand it to the audio thread (under buildMutex_)
    void build(const Settings& s) {
        // a kernel the audio thread doesn't hold: the one waiting for it (taken back), or a free one
        uint32_t state = slots_.load(std::memory_order_acquire);
        int target = -1;
        while (target < 0) {
            int ready = readyOf(state);
            if (ready >= 0) {
                if (slots_.compare_exchange_weak(state, state & 7u, std::memory_order_acq_rel)) target = ready;
            } else {
                for (int i = 0; i < KERNELS && target < 0; i++)
                    if (!(state & (1u << i))) target = i;
            }
        }
        shape(s, shaped_);
        cut(shaped_, kernels_[target]);
        state = slots_.load(std::memory_order_acquire);
        while (!slots_.compare_exchange_weak(state, (state & 7u) | ((uint32_t)(target + 1) << 4),
                                             std::memory_order_acq_rel)) {
        }
        built_ = s;
    }

    void work() {
        std::unique_lock<std::mutex> lock(wakeMutex_);
        while (!quit_) {
            wake_.wait_for(lock, std::chrono::milliseconds(POLL_MS));
            if (quit_) break;
            lock.unlock();
            {
                std::lock_guard<std::mutex> b(buildMutex_);
                Settings s = wanted();
                if (s != built_) build(s);
            }
            lock.lock();
        }
    }

    // ---- the audio thread's side ----

    int cur_ = -1, old_ = -1;  // the kernel playing; the one fading out (-1: none)
    int fadeLeft_ = 0, fadeLen_ = P;
    int maxParts_ = 1, fdlAt_ = 0, pos_ = 0, hpos_ = 0;
    int quietParts_ = 0;  // pieces of input since the last that wasn't silent
    bool heard_ = false;  // anything but silence since start()
    std::atomic<size_t> playing_{0};  // the frames of the responses playing (for sleepAfter)
    std::vector<float> fdlRe_, fdlIm_;          // the ring of input spectra: maxParts_ × P
    std::vector<float> hist_;                    // the newest P inputs, twice (2P): the head's window
    std::vector<float> fresh_;                   // this piece's input so far
    std::vector<double> frame_;                  // the last two pieces' input (2P), for the FFT
    std::vector<float> tail_[2][2];              // the rest of the reverb for this piece: [new, old][side]
    std::vector<float> accRe_[2], accIm_[2];
    std::vector<fft::cd> X_, A_, B_, work_;
    std::vector<double> outA_, outB_;
    std::unique_ptr<fft::Plan> plan_;

    void allocate(double rate, int) override {
        std::lock_guard<std::mutex> b(buildMutex_);
        rate_ = rate;
        maxParts_ = (int)((longest() + P - 1) / P) + 1;
        fdlRe_.assign((size_t)maxParts_ * P, 0.0f);
        fdlIm_.assign((size_t)maxParts_ * P, 0.0f);
        hist_.assign(2 * P, 0.0f);
        fresh_.assign(P, 0.0f);
        frame_.assign(2 * P, 0.0);
        for (auto& k : tail_)
            for (auto& s : k) s.assign(P, 0.0f);
        for (int c = 0; c < 2; c++) {
            accRe_[c].assign(P, 0.0f);
            accIm_[c].assign(P, 0.0f);
        }
        X_.assign(P + 1, {});
        A_.assign(P + 1, {});
        B_.assign(P + 1, {});
        work_.assign(2 * P, {});
        outA_.assign(2 * P, 0.0);
        outB_.assign(2 * P, 0.0);
        if (!plan_) plan_ = std::make_unique<fft::Plan>(2 * P);
        fadeLen_ = std::max(1, (int)std::lround(FADE_S * rate / P)) * P;
        // (nothing plays: every kernel is free) the response for the settings now, playing at once
        slots_.store(0);
        cur_ = old_ = -1;
        build(wanted());
        takeReady(false);
        quietParts_ = maxParts_ + 1;
        if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
    }

    // a state loaded (a project, undo): its response made at once (on this thread, not the audio's)
    void setState(const std::string& component, const std::string& controller) override {
        Effect::setState(component, controller);
        std::lock_guard<std::mutex> b(buildMutex_);
        Settings s = wanted();
        if (s != built_) build(s);
    }

    // take the kernel waiting, if any: fading to it, or at once (nothing heard since start())
    bool takeReady(bool fade) {
        uint32_t state = slots_.load(std::memory_order_acquire);
        int ready = readyOf(state);
        while (ready >= 0) {
            if (slots_.compare_exchange_weak(state, (state & 7u) | (1u << ready), std::memory_order_acq_rel)) break;
            ready = readyOf(state);
        }
        if (ready < 0) return false;
        if (cur_ >= 0) {
            if (fade) {
                old_ = cur_;
                fadeLeft_ = fadeLen_;
            } else {
                release(cur_);
            }
        }
        cur_ = ready;
        size_t frames = kernels_[cur_].frames;
        if (old_ >= 0) frames = std::max(frames, kernels_[old_].frames);
        playing_.store(frames, std::memory_order_relaxed);
        return true;
    }
    void release(int k) { slots_.fetch_and(~(1u << k), std::memory_order_acq_rel); }

    double sleepAfter() const override {
        return std::max(SLEEP_AFTER_S * sr, (double)playing_.load(std::memory_order_relaxed) + 2.0 * P);
    }

    void start() override {
        if (old_ >= 0) {
            release(old_);
            old_ = -1;
            fadeLeft_ = 0;
        }
        if (quietParts_ <= maxParts_)  // (only silence in the ring already: nothing to clear)
            std::fill(fdlRe_.begin(), fdlRe_.end(), 0.0f), std::fill(fdlIm_.begin(), fdlIm_.end(), 0.0f);
        quietParts_ = maxParts_ + 1;
        std::fill(hist_.begin(), hist_.end(), 0.0f);
        std::fill(fresh_.begin(), fresh_.end(), 0.0f);
        std::fill(frame_.begin(), frame_.end(), 0.0);
        for (auto& k : tail_)
            for (auto& s : k) std::fill(s.begin(), s.end(), 0.0f);
        pos_ = hpos_ = 0;
        heard_ = false;
        if (cur_ >= 0) playing_.store(kernels_[cur_].frames, std::memory_order_relaxed);
    }

    // the head: a dot product of P (eight sums, so it vectorizes)
    static float dot(const float* __restrict h, const float* __restrict w) {
        float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int k = 0; k < P; k += 8)
            for (int u = 0; u < 8; u++) s[u] += h[k + u] * w[k + u];
        return ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
    }

    // one piece's spectrum times the input's, added (both sides)
    static void multiplyAdd(float* __restrict r0, float* __restrict i0, float* __restrict r1, float* __restrict i1,
                            const float* __restrict xr, const float* __restrict xi, const float* __restrict hr0,
                            const float* __restrict hi0, const float* __restrict hr1, const float* __restrict hi1) {
        for (int k = 0; k < P; k++) {
            r0[k] += xr[k] * hr0[k] - xi[k] * hi0[k];
            i0[k] += xr[k] * hi0[k] + xi[k] * hr0[k];
            r1[k] += xr[k] * hr1[k] - xi[k] * hi1[k];
            i1[k] += xr[k] * hi1[k] + xi[k] * hr1[k];
        }
        // (bin 0 holds two real bins, DC and P: the loop multiplied them as one complex number)
        r0[0] += xi[0] * hi0[0];
        i0[0] += xi[0] * hi0[0] - xr[0] * hi0[0] - xi[0] * hr0[0];
        r1[0] += xi[0] * hi1[0];
        i1[0] += xi[0] * hi1[0] - xr[0] * hi1[0] - xi[0] * hr1[0];
    }

    // the rest of the reverb for the next P frames, through kernel k, into out[2]
    void convolve(const Kernel& k, std::vector<float> out[2]) {
        if (k.first >= k.parts || quietParts_ > maxParts_) {
            for (int c = 0; c < 2; c++) std::fill(out[c].begin(), out[c].end(), 0.0f);
            return;
        }
        for (int c = 0; c < 2; c++) {
            std::fill(accRe_[c].begin(), accRe_[c].end(), 0.0f);
            std::fill(accIm_[c].begin(), accIm_[c].end(), 0.0f);
        }
        for (int j = k.first; j < k.parts; j++) {
            int slot = fdlAt_ - j;
            if (slot < 0) slot += maxParts_;
            size_t x = (size_t)slot * P, h = (size_t)j * P;
            multiplyAdd(accRe_[0].data(), accIm_[0].data(), accRe_[1].data(), accIm_[1].data(), fdlRe_.data() + x,
                        fdlIm_.data() + x, k.re[0].data() + h, k.im[0].data() + h, k.re[1].data() + h,
                        k.im[1].data() + h);
        }
        A_[0] = fft::cd(accRe_[0][0], 0);
        A_[P] = fft::cd(accIm_[0][0], 0);
        B_[0] = fft::cd(accRe_[1][0], 0);
        B_[P] = fft::cd(accIm_[1][0], 0);
        for (int i = 1; i < P; i++) {
            A_[(size_t)i] = fft::cd(accRe_[0][(size_t)i], accIm_[0][(size_t)i]);
            B_[(size_t)i] = fft::cd(accRe_[1][(size_t)i], accIm_[1][(size_t)i]);
        }
        fft::realPairInverse(*plan_, A_.data(), B_.data(), outA_.data(), outB_.data(), work_);
        for (int i = 0; i < P; i++) {  // (overlap-save: the second half is the clean one)
            out[0][(size_t)i] = (float)outA_[(size_t)(P + i)];
            out[1][(size_t)i] = (float)outB_[(size_t)(P + i)];
        }
    }

    // a piece of input complete: its spectrum into the ring, the kernels swapped if one is ready,
    // and the next piece of the reverb worked out
    void piece() {
        bool loud = false;
        for (int i = 0; i < P; i++) {
            frame_[(size_t)i] = frame_[(size_t)(P + i)];
            frame_[(size_t)(P + i)] = fresh_[(size_t)i];
            loud |= fresh_[(size_t)i] != 0.0f;
        }
        quietParts_ = loud ? 0 : std::min(quietParts_ + 1, maxParts_ + 1);
        fdlAt_ = fdlAt_ + 1 >= maxParts_ ? 0 : fdlAt_ + 1;
        float* re = fdlRe_.data() + (size_t)fdlAt_ * P;
        float* im = fdlIm_.data() + (size_t)fdlAt_ * P;
        if (quietParts_ < 2) {  // (both halves silent: its spectrum is silence)
            fft::realPair(*plan_, frame_.data(), nullptr, X_.data(), nullptr, work_);
            re[0] = (float)X_[0].real();
            im[0] = (float)X_[P].real();
            for (int i = 1; i < P; i++) {
                re[i] = (float)X_[(size_t)i].real();
                im[i] = (float)X_[(size_t)i].imag();
            }
        } else {
            std::fill(re, re + P, 0.0f);
            std::fill(im, im + P, 0.0f);
        }
        if (old_ >= 0 && fadeLeft_ <= 0) {
            release(old_);
            old_ = -1;
            playing_.store(kernels_[cur_].frames, std::memory_order_relaxed);
        }
        if (old_ < 0) takeReady(heard_);
        convolve(kernels_[cur_], tail_[0]);
        if (old_ >= 0) convolve(kernels_[old_], tail_[1]);
    }

    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        if (!heard_) takeReady(false);  // (only silence held: the new one plays at once)
        double w = v(width) / 100, m = v(mix) / 100;
        for (int i = 0; i < n; i++) {
            float mono = 0.5f * (x[0][i] + x[1][i]);
            heard_ = heard_ || mono != 0.0f;
            hpos_ = (hpos_ + 1) & (P - 1);
            hist_[(size_t)hpos_] = hist_[(size_t)(hpos_ + P)] = mono;
            fresh_[(size_t)pos_] = mono;
            const float* window = hist_.data() + hpos_ + 1;
            const Kernel& k = kernels_[cur_];
            double wet[2];
            for (int c = 0; c < 2; c++)
                wet[c] = tail_[0][c][(size_t)pos_] + (k.headSilent ? 0.0f : dot(k.head[c].data(), window));
            if (old_ >= 0) {
                const Kernel& o = kernels_[old_];
                double g = 1.0 - (double)fadeLeft_ / fadeLen_;
                for (int c = 0; c < 2; c++) {
                    double before = tail_[1][c][(size_t)pos_] + (o.headSilent ? 0.0f : dot(o.head[c].data(), window));
                    wet[c] = before + g * (wet[c] - before);
                }
                fadeLeft_--;
            }
            double mid = 0.5 * (wet[0] + wet[1]), side = 0.5 * (wet[0] - wet[1]) * w;
            y[0][i] = (float)(x[0][i] * (1 - m) + (mid + side) * m);
            y[1][i] = (float)(x[1][i] * (1 - m) + (mid - side) * m);
            if (++pos_ == P) {
                pos_ = 0;
                piece();
            }
        }
    }
};

Space& asSpace(Effect& e) {
    auto* s = dynamic_cast<Space*>(&e);
    if (!s) throw std::invalid_argument("not a Space effect");
    return *s;
}

}  // namespace

std::unique_ptr<Effect> makeSpace() { return std::make_unique<Space>(); }

void setSpaceImpulse(Effect& e, std::vector<float> left, std::vector<float> right) {
    Space& s = asSpace(e);
    std::lock_guard<std::mutex> b(s.buildMutex_);
    s.custom_[0] = std::move(left);
    s.custom_[1] = std::move(right);
    static std::atomic<uint64_t> ids{0};
    s.customId_ = ++ids;
    s.customRoom_ = std::clamp((int)std::lround(s.value(s.room)), 0, ROOM_COUNT - 1);
    s.build(s.wanted());
}

std::vector<float> spaceImpulse(Effect& e, int side) {
    Space& s = asSpace(e);
    std::lock_guard<std::mutex> b(s.buildMutex_);
    return s.shaped_[std::clamp(side, 0, 1)];
}

bool spaceSettle(Effect& e, double seconds) {
    Space& s = asSpace(e);
    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    for (;;) {
        {
            std::lock_guard<std::mutex> b(s.buildMutex_);
            if (s.wanted() == s.built_) return true;
        }
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int spacePartitions(Effect& e) {
    Space& s = asSpace(e);
    std::lock_guard<std::mutex> b(s.buildMutex_);
    size_t frames = s.shaped_[0].size();
    return frames > (size_t)P ? (int)((frames - 1) / P) : 0;
}

}  // namespace effects
