// The metronome's settings window (right-click the Metronome button), and remembering them
// (metronome_dialog.py).
//
// Two click editors side by side: the click at the start of each bar, and the click on the other
// beats. Each has an envelope graph to drag (the attack, sustain-end and release-end points; up and
// down for the level) with the same values as numbers below it, plus pitch, waveform and a Test
// button. Changes apply at once (so you can adjust while it plays) and are remembered between runs.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "audio/metronome.h"
#include "core/json.h"
#include "ui/panel_style.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

namespace audio { class Engine; }

namespace metronome_dialog {
extern const char* const SETTINGS_KEY;  // "metronome/sounds"
constexpr int GRAPH_MS = (int)(metronome::MAX_ATTACK_MS + metronome::MAX_SUSTAIN_MS + metronome::MAX_RELEASE_MS);
constexpr int HANDLE_R = 5;  // px

// the remembered MetronomeSettings (defaults for anything missing or invalid)
metronome::MetronomeSettings loadSettings();
void saveSettings(const metronome::MetronomeSettings& settings);
// keep a click's values in their ranges
void limit(metronome::Click& click);
// as stored: {"bar": {"pitch": …, "level_db": …, "attack_ms": …, "sustain_ms": …, "release_ms": …,
// "waveform": "sine"}, "beat": {…}} (the Python program's field names)
Json toJson(const metronome::MetronomeSettings& settings);
metronome::MetronomeSettings fromJson(const Json& stored);
// main_window.test_click: play a click once (through the engine's preview, on outputs 1/2)
void testClick(audio::Engine& engine, const metronome::Click& click);
}  // namespace metronome_dialog

// Attack-sustain-release of one click: time across (GRAPH_MS), level up (MIN_DB..MAX_DB).
// Drag a point: the attack end (also the level), the sustain end (also the level), the release end.
// Styled as EnvelopeGraph (default.css), with the class of its click (.bar, .beat): its background-color and
// color (the envelope's); ::grid (color, stroke-width), ::envelope (stroke-width; fill-opacity: inside it),
// ::label (color, font), ::handle (background-color, border-top-color, border-top-width) and ::handle:dragging.
class EnvelopeGraph : public ui::Widget {
public:
    // role: its class ("bar", "beat"), which gives it its colour
    EnvelopeGraph(metronome::Click* click, const std::string& role);
    metronome::Click* click;
    Color color() const;           // the envelope's, from the stylesheets
    int dragging = 0;              // the point being dragged (1..3), 0 none
    ui::Signal<> changed;
    RectF area() const;
    double xOf(double ms) const;
    double yOf(double db) const;
    std::vector<PointF> points() const;
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    Point sizeHint() const override { return Point(240, 120); }

private:
    struct Look {
        const css::Style* self = nullptr;  // (its box; kept until parts_.refresh() answers true)
        Color color, grid, label, handle, handleEdge, handleDragging;
        float gridW = 1, lineW = 2, handleW = 2;
        int fillAlpha = 60;
        Font font;
    };
    const Look& look() const;
    mutable panel_style::Parts parts_{*this};
    mutable Look look_;
};

// One click: its envelope graph, its values as numbers, pitch, waveform, and Test.
class ClickEditor : public ui::GroupBox {
public:
    // test(click): play it once; role: the graph's class ("bar", "beat")
    ClickEditor(const std::string& title, metronome::Click* click, const std::string& role,
                std::function<void(const metronome::Click&)> test);
    metronome::Click* click;
    EnvelopeGraph* graph;
    std::map<std::string, ui::SpinBox*> boxes;  // "attack_ms", "sustain_ms", "release_ms", "level_db", "pitch"
    ui::ComboBox* waveform;
    ui::Button* testButton;
    ui::Signal<> changed;
    void showValues();
    void setValue(const std::string& attr, double value);
    void setWaveform(const std::string& w);
private:
    void fromGraph();
};

class MetronomeDialog : public ui::Dialog {
public:
    using Apply = std::function<void(const metronome::MetronomeSettings&)>;
    // Edits `settings` in place (it must outlive the dialog); apply(settings) is called after every
    // change (and the settings are saved); test(click) plays a click once.
    MetronomeDialog(ui::Window* parent, metronome::MetronomeSettings& settings, Apply apply,
                    std::function<void(const metronome::Click&)> test);
    metronome::MetronomeSettings& settings;
    std::vector<ClickEditor*> editors;
    void changed();
    void useDefaults();
private:
    Apply apply_;
};
