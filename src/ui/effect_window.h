// A built-in effect's window (effect_window.py), made from its parameters (effects/effect.h): a
// slider for each value (its text next to it; double-click for its default), a checkbox for an
// Off / On, a list for a choice; above them its curve (an EQ's response, a compressor's transfer
// curve) and meters (gain reduction…) when it has them; a Defaults button. An effect with an
// analyzer (the EQ) shows the live spectrum of its sound behind its curve, with a list to choose
// what it listens to (Off, Pre, Post); the Tuner's meters are drawn as a tuner (the note large, a
// needle for the cents). It follows its
// parameters as automation moves them (every REFRESH_MS). Changes made here count as the effect's
// own window's for automation ("recently tweaked": Plugin::windowEdit / edited()).
//
// It behaves as EditorWindow (plugin_editor.h) does for PluginHost: attach() (always true: it is
// drawn by the program), place(), position(), editorClosed, and the fields PluginHost keeps on an
// open window (watcher, picture, photographed, tickTimer). Its picture for the patch panel is
// simply what it shows (snapshot(), no need to show it: PluginHost::photograph can make one
// invisibly, take the snapshot and delete it).
//
// The slot's instance must be a built-in effect (effects::asEffect(instance) not null; PluginHost:
// effects::isBuiltin(slot->path)); otherwise the window is empty and attach() returns false.
//
// Styles (default.css, "Panels, meters and editors"):
//   EffectWindow  background-color (its picture's), padding and row-gap (around and between its parts), column-gap
//                 (between a parameter's controls); ::slider and ::value (min-width: a value's slider and text).
//   CurveView     its box; ::grid (color, stroke-width; the labels' font), ::analyzer (the spectrum behind: color,
//                 fill-opacity, stroke-opacity, stroke-width), ::curve (color, stroke-width).
//   MeterView     color, font; line-height (a meter's row); ::label (width, padding-right), ::track (the bar's box: height),
//                 ::bar (background-color), ::value (width, padding-left).
//   Tuner         (a MeterView) its box; ::note (font, color) :in-tune / :silent; ::scale (background-color,
//                 height; margin-left / margin-right: its ends from the sides; top: its place), ::tick (color)
//                 and ::tick:centre; ::needle (background-color, border-radius) and :in-tune; ::cents and ::hz
//                 (color, font), ::end-label (−50 / +50: color, font).
#pragma once
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "effects/effect.h"
#include "gfx/image.h"
#include "model/model.h"
#include "ui/panel_style.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/widget.h"

namespace automation { class TweakWatcher; }
namespace ui { class HBox; }

namespace effect_window {
constexpr int REFRESH_MS = 50;  // meters, and the controls following automation
constexpr int SLIDER_STEPS = 1000;
}  // namespace effect_window

// The effect's curve: points (x 0…1 left to right, y -1…1 bottom to top), with its grid lines.
class CurveView : public ui::Widget {
public:
    explicit CurveView(std::shared_ptr<effects::Effect> effect);
    void refresh();
    void paint(Painter& p) override;
    Point sizeHint() const override { return Point(300, 130); }
    std::shared_ptr<effects::Effect> effect;
    std::vector<effects::CurvePoint> points;
    // the analyzer's spectrum behind the curve (empty: none), worked out again by refreshSpectrum()
    // (every REFRESH_MS while the window is open; `dt`: seconds since the last)
    void refreshSpectrum(double dt);
    std::vector<effects::CurvePoint> spectrum;

private:
    struct Look {  // (kept: the analyzer repaints it every REFRESH_MS)
        const css::Style* self = nullptr;
        Color grid, analyzerFill, analyzerLine, curve;
        float gridW = 1, analyzerW = 1, curveW = 2;
        Font font;
    };
    const Look& look();
    panel_style::Parts parts_{*this};
    Look look_;
};

// The effect's meters: a bar each (0…1) with its label and text.
class MeterView : public ui::Widget {
public:
    explicit MeterView(std::shared_ptr<effects::Effect> effect);
    void refresh();
    void paint(Painter& p) override;
    Point sizeHint() const override { return Point(300, rowH() * std::max<int>(1, (int)values.size())); }
    std::shared_ptr<effects::Effect> effect;
    std::vector<effects::Meter> values;

protected:
    struct Look {  // (kept: the meters repaint every REFRESH_MS)
        const css::Style* track = nullptr;
        Color text, bar;
        int rowH = 20, labelW = 110, labelPad = 6, trackH = 12, valueW = 80, valuePad = 6;
        Font font;
    };
    const Look& look() const;
    int rowH() const { return look().rowH; }
    mutable panel_style::Parts parts_{*this};
    mutable Look look_;
};

// The Tuner's meters ([note, cents, frequency]) as a tuner: the note large in the middle, a scale
// of −50…+50 cents under it with a needle (green when within 5 cents), the cents and the frequency.
class TunerView : public MeterView {
public:
    explicit TunerView(std::shared_ptr<effects::Effect> effect);
    void paint(Painter& p) override;
    Point sizeHint() const override { return Point(300, 120); }

private:
    struct TunerLook {
        const css::Style* self = nullptr;
        const css::Style* note[3] = {};  // heard, in tune, silent
        Color scale, tick, tickCentre, needle, needleInTune, cents, hz, endLabel;
        float needleR = 2;  // (before Options → Rounded corners)
        float needleW = 4, needleH = 30, needleTop = -14;  // (its top: from the scale's)
        float tickW = 1, tickH = 12, tickCentreH = 24;     // (centred on the scale, 1 px below its middle)
        int scaleH = 2, scaleLeft = 24, scaleRight = 24, scaleTop = 66;
        Font noteFont[3], centsFont, hzFont, endFont;
    };
    const TunerLook& tunerLook();
    panel_style::Parts tparts_{*this};
    TunerLook tunerLook_;
};

class EffectWindow : public ui::Window {
public:
    // parent: the main window (it stays above it); title: "<slot name> — <track>"; invisible: mapped
    // offscreen, for a picture
    EffectWindow(ui::Window* parent, SlotPtr slot, const std::string& title, bool invisible = false);
    ~EffectWindow() override;

    SlotPtr slot;
    std::shared_ptr<effects::Effect> effect;  // the slot's instance as a built-in effect (null: none)
    bool invisible;

    // its window is ready (it always has one, for a built-in effect); starts following the effect
    bool attach();
    // over the main window (a third of the way down), or where it was last, fully on the screen
    void place(ui::Window* main, std::optional<Point> last = {});
    Point position() const;
    // show the effect's settings (automation may move them), its meters and its curve
    void follow();
    // every setting back to its default (as edits in its window)
    void defaults();
    // what it shows, for the patch panel (plugin_pictures)
    Image snapshot();
    void close() override;

    ui::Signal<SlotPtr> editorClosed;  // the slot, once the window is closed (Python: closed)

    // kept by PluginHost while it is open (as on EditorWindow)
    std::unique_ptr<automation::TweakWatcher> watcher;
    Image picture;
    double photographed = 0;
    int tickTimer = -1;

    // its controls (tests)
    struct Control {
        int index;              // the parameter's
        ui::Widget* widget;     // Slider, CheckBox or ComboBox
        ui::Label* text;        // the slider's value text, or null
    };
    std::vector<Control> controls;
    CurveView* curveView = nullptr;
    MeterView* meterView = nullptr;  // (a TunerView for the Tuner)
    ui::ComboBox* analyzerBox = nullptr;  // what the analyzer listens to (an effect with one)
    ui::Button* defaultsButton = nullptr;

    // --- presets and MIDI learn (effect_window_presets.cpp; presets_ui.h, midi_learn.h) ---
    // "Presets ▾": save, load, rename or delete its presets, and compare (Keep this, Now / Kept); while
    // settings are kept, the names of the parameters that differ between Now and Kept are marked.
    // Right-click a control (or its name): link it to a MIDI controller.
    ui::Button* presetsButton = nullptr;
    std::vector<ui::Label*> nameLabels;  // each parameter's name (by index; null if not found)
    std::vector<int> marked;             // the parameters marked now
    void addPresetsButton(ui::HBox* row);
    void showDifferences();              // (every follow())
    int controlAt(Point pos);            // the parameter whose control or name is at pos (window coordinates), or -1
    bool contextMenu(ui::MouseEvent& e) override;

private:
    void edit(int index, double normal);
    bool updating_ = false;  // (the controls being set from the effect: not edits)
    std::vector<double> shown_;
    int timer_ = -1;
    bool closing_ = false;
    double lastSpectrum_ = -1;  // (seconds, steady clock: the analyzer's dt)
    static plat::WindowOptions options(ui::Window* parent, const std::string& title, bool invisible);
};
