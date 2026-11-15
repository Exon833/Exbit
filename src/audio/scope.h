// What the master scope shows (ui/scope_view.h), worked out from the master's newest audio
// (Metering::scopeLatest): no drawing here, so it is tested on its own.
//
// Spectrum: the newest fftSize samples through a window function and an FFT, as levels in dB (a
// full-scale sine reads 0 dB) on `count` bands spaced by frequency's logarithm from 20 Hz to the top
// of the audio range, tilted by `slope` dB per octave about 1 kHz (music then looks flat: 3 dB is
// pink noise's), floored at `floorDb`. Rises at once, falls at `fall` dB per second (0: at once).
//
// Waveform: the lowest and highest sample under each column of the picture. With pitch follow, the
// note's period is found (YIN: the difference function, from an FFT, normalised by its running mean;
// the first dip under a threshold), whole periods are shown, and each picture starts at the rising
// zero crossing of the newest period that best matches where the last picture started: a held note
// stands still. When the period isn't clear (noise, chords, silence) the capture window runs freely.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fft {
class Plan;
}

namespace scope {

enum class Window { Hann, Hamming, BlackmanHarris, Rectangular };
const char* windowName(Window w);
// the window's n coefficients
std::vector<double> windowShape(Window w, int n);

struct SpectrumSettings {
    int fftSize = 4096;  // a power of two, 256…32768
    Window window = Window::Hann;
    double fall = 48;     // dB per second the levels fall back (0: at once)
    double slope = 3;     // dB per octave, about 1 kHz
    double floorDb = -90;
};

class Spectrum {
public:
    Spectrum();
    ~Spectrum();
    // the newest samples (`n` of them, the newest last; fewer than fftSize: padded with silence before),
    // `dt` seconds after the last analysis (for the fall)
    void analyse(const float* samples, size_t n, int sr, const SpectrumSettings& s, int count, double dt);
    const std::vector<float>& levels() const { return levels_; }  // dB, floorDb…0 (and above, if louder)
    double bandHz(int band) const;  // the middle of a band (its frequency on the picture)
    double peakHz() const { return peakHz_; }  // the loudest frequency of the last analysis (interpolated)
    void reset() { levels_.clear(); }

private:
    std::map<int, std::unique_ptr<fft::Plan>> plans_;
    std::vector<float> levels_;
    double lowHz_ = 20, highHz_ = 20000, peakHz_ = 0;
};

// the lowest and highest sample under each of `columns` columns
struct Column {
    float lo = 0, hi = 0;
};
void columns(const float* samples, size_t n, int columns, std::vector<Column>& out);

struct Pitch {
    double hz = 0;      // 0: no clear pitch
    double period = 0;  // in samples
    double clarity = 0; // 0…1 (1 - the dip of the normalised difference)
};
// the pitch of the newest samples (between minHz and maxHz)
Pitch detectPitch(const float* samples, size_t n, int sr, double minHz = 30, double maxHz = 2000);
// "A3", "C#4" (C4 is MIDI note 60, as Tiny DAW's keyboard)
std::string noteName(double hz);

// Pitch follow: where a picture of `length` samples should start in samples[0…n) so a periodic sound
// stands still: at the rising zero crossing within the newest period before n - length whose next
// `period` samples best match `previous` (the last picture's start; empty: the latest crossing).
// No crossing there: n - length.
size_t followStart(const float* samples, size_t n, size_t length, double period, const std::vector<float>& previous);

}  // namespace scope
