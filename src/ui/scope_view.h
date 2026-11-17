// The master scope: under the Master row (the right-hand column), what the master plays now, as a
// waveform (an oscilloscope) or a spectrum. audio/scope.h works it out from the master's newest audio
// (Metering::scopeLatest; it asks for it with Metering::scopeOn while shown); this draws it.
//
// Right-click it for its options (kept with the program's settings, "options/scope"):
//   Waveform or Spectrum; Channels: combined (left and right mixed) or separate (side by side);
//   for the waveform: the capture window (how much time it shows), pitch follow (whole cycles of the
//   note heard, standing still, its name shown) and how many cycles;
//   for the spectrum: the FFT size, the window function, how fast it falls back, the slope (dB per
//   octave) and the range shown.
// Options → Master scope shows or hides it (the master's row is SCOPE_H shorter without it).
//
// Styled as Scope (default.css): its box (background-color, border, padding; margin-top: the gap above it);
// ::divider between separate channels (color, width; margin-left and margin-right: the gap around it); ::trace,
// the waveform and the spectrum's line (color, stroke-width; fill-opacity: under the spectrum's line); ::grid
// (color); ::caption (color, font, padding, height). Its height is :root's --scope-height.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "audio/scope.h"
#include "core/json.h"
#include "ui/panel_style.h"
#include "ui/toolkit/widget.h"

namespace audio {
class Engine;
}

struct ScopeSettings {
    bool shown = true;
    bool spectrum = false;  // (the waveform first)
    bool separate = false;  // left and right side by side
    int captureMs = 50;
    bool pitchFollow = true;
    int cycles = 2;
    scope::SpectrumSettings fft;
    Json toJson() const;
    static ScopeSettings fromJson(const Json& j);  // (missing or odd values: the defaults)
};

class ScopeView : public ui::Widget {
public:
    explicit ScopeView(audio::Engine& engine);
    ~ScopeView() override;
    ScopeSettings settings;
    std::function<void()> settingsChanged;  // an option changed (the main window keeps it)
    std::function<void()> hideRequested;    // "Hide the scope" in its menu

    void paint(Painter& p) override;
    bool contextMenu(ui::MouseEvent& e) override;
    void fillMenu(ui::Menu& menu);  // (also for tests)
    // takes the master's newest audio and works out the picture (every frame while shown)
    void frame();
    // (tests) the picture from this audio instead of the engine's
    void showAudio(const std::vector<float>& left, const std::vector<float>& right, int sr, double dt);
    // what the waveform's label says now ("A3 220 Hz", "50 ms"), and the pitch found (0: none)
    std::string label(int channel = 0) const;
    double pitchHz(int channel = 0) const;

private:
    struct Pane {
        scope::Spectrum spectrum;
        std::vector<float> shown;     // the waveform's samples on show
        std::vector<float> previous;  // (pitch follow: the last picture's start)
        scope::Pitch pitch;
    };
    void analyse(const std::vector<float>& left, const std::vector<float>& right, int sr, double dt);
    void analysePane(Pane& pane, const std::vector<float>& samples, int sr, double dt, int width);
    void paintPane(Painter& p, const Pane& pane, const RectF& r, const std::string& name);
    void changed();  // an option changed: start afresh, tell the main window
    audio::Engine& engine_;
    Pane panes_[2];
    int sr_ = 48000;
    int frameId_ = -1;
    double lastTime_ = -1;
    uint64_t lastHeard_ = 0;
    bool wasOn_ = false;
    std::vector<float> left_, right_, mixed_;
    // what it is drawn with (kept: it repaints every frame)
    struct Look {
        const css::Style* self = nullptr;  // (its box)
        ui::style::Insets inside, caption;  // (inside: its border and padding)
        int gap = 3, dividerW = 1, paneGap = 3, captionH = 12;
        Color divider, trace, fill, grid, text;
        float traceW = 1.2f;
        Font font;
    };
    const Look& look();
    panel_style::Parts parts_{*this};
    Look look_;
};
