// The built-in synth's window ("Synth…" in a MIDI track's instrument menu, or 🎹 while it plays the built-in
// synth): its patch (audio/synth_patch.h) on one page, a view of its modulation, and its presets. Changes are
// heard at once (live notes use the new patch; the track's clips are rendered again). Like the sampler's window
// it doesn't block anything; one per track. It uses sampler_dialog's Host (the main window sets it).
//
// THE PAGE (SynthPage): every control as a knob (a choice as a box: click it for its list; an on/off: click it),
// in sections: Osc 1, Osc 2, Noise, Unison, Filter, Env 1, Env 2, LFO, Voice. Drag a knob up or down (Shift:
// finer), the wheel steps it, double-click puts it back to its default. MODULATION BY DRAGGING: the ⊕ handle on
// Env 1's, Env 2's and the LFO's title dragged onto a knob that can be modulated routes that source to it (at
// MOD_START depth); a ring around the knob, one per source in its colour, shows how far it moves it; drag the
// ring to change the depth (right-click the knob to remove a routing, or Modulate…). The keyboard (and exbit
// --tui, where the page is a list: section, control, value, its modulation): Up / Down a control, Left / Right
// a step (Page Up / Down ten), Home / End its ends, Backspace its default, Enter a choice's next, M Modulate….
//
// THE MODULATION VIEW (the window's "Modulation" button; "Synth" goes back): every routing as a row (source →
// target, depth); Add… and Edit… open MODULATE… (a source, a target and a depth: what the terminal uses
// instead of dragging), Remove takes the routing away.
//
// PRESETS ▾: "Init (classic)" (the classic voice of projects made before this synth: no patch), "Init" (one
// saw), the shipped presets by category, the user's own (Save preset… asks for a name and category).
//
// A track playing the classic voice shows the Init patch's values and a line saying so; changing anything (or
// choosing a preset) gives it that patch.
//
// A DRAWN SYNTH (its patch has Patch::drawn: Instrument ▾ → Drawn synth…): the window shows only its wave, on a
// canvas to draw it on (ui/wave_canvas.h), and Close: no presets, knobs or modulation.
//
// THE MAIN WINDOW: after undo / redo synth_dialog::reloadAll(); when tracks go away synth_dialog::closeGone(project).
//
// Styles (default.css): SynthDialog (padding, row-gap). SynthPage: background-color, color and font (names and
// values); ::section (a section's box), ::title (its title: color, font), ::knob (background-color),
// ::pointer and ::arc (the value: color, stroke-width), ::choice (a choice's box, color), ::current (the
// control with the keyboard: color), ::handle (a source's ⊕: background-color, color), ::target (a knob that
// can take the dragged source: color), ::env1, ::env2, ::lfo (each source's ring: color, stroke-width).
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "audio/synth_patch.h"
#include "gfx/geom.h"
#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"
#include "ui/wave_canvas.h"

namespace synth_dialog {

constexpr int RENDER_DELAY_MS = 150;  // while a knob is dragged: the clips rendered again this long after a move
constexpr double MOD_START = 0.25;    // a routing made by dragging starts at this depth
constexpr int DRAG_PIXELS = 200;      // of dragging: a knob's whole range (Shift: five times finer)

class SynthPage : public ui::Widget {
public:
    SynthPage();
    subsynth::Patch patch;  // as shown
    // called on every change made here (final: the mouse let go, or not a drag: render now)
    std::function<void(const subsynth::Patch&, bool final)> changed;
    std::function<void(int target)> modulate;  // M on a control: Modulate… for it

    int current = 0;  // the control with the keyboard (an index into order())
    static const std::vector<int>& order();  // every parameter, as the page shows them
    // where things are (the page's coordinates)
    Rect cellOf(int param) const;
    Point knobCentre(int param) const;
    int knobRadius() const { return 15; }
    int ringRadius(int source) const { return knobRadius() + 3 + 3 * source; }  // (a source's ring around a knob)
    Rect handleOf(int source) const;
    int paramAt(Point p) const;  // -1: none
    // a ring at p: its source (and the knob's parameter in `param`); -1: none
    int ringAt(Point p, int& param) const;
    bool dragging() const { return drag_ != Drag::None; }

    void set(int param, double value, bool final);
    void setDepth(int source, int target, double depth, bool final);

    Point sizeHint() const override;
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override;
    bool wheel(ui::WheelEvent& e) override;
    bool contextMenu(ui::MouseEvent& e) override;
    bool keyPress(ui::KeyEvent& e) override;
    bool listRows(std::vector<std::string>& rows, int& current) const override;
    std::string toolTipAt(Point p) const override;

private:
    enum class Drag { None, Knob, Ring, Handle };
    Drag drag_ = Drag::None;
    int dragParam_ = -1, dragSource_ = -1;
    Point pressAt_, mouse_;
    double startValue_ = 0;
    bool moved_ = false;
    struct Section {
        const char* title;
        int source;  // its ⊕ (-1: none)
        std::vector<int> params;
        Rect box;
    };
    std::vector<Section> sections_;
    std::map<int, Rect> cells_;
    void layoutCells();
    void chooseFrom(int param);  // a choice's list
};

class SynthDialog : public ui::Dialog {
public:
    SynthDialog(ui::Window* window, TrackPtr track);
    ~SynthDialog() override;
    TrackPtr track;
    SynthPage* page;
    ui::Label *presetName, *classicNote;
    ui::Button *presetsButton, *saveButton, *modulateButton, *synthView, *modView;
    ui::Widget* modPanel;
    ui::ListWidget* modList;
    ui::Button *addMod, *editMod, *removeMod, *playButton;
    WaveCanvas* wave;  // a Drawn synth's (Patch::drawn): all its window shows
    ui::Menu presetsMenu;

    void apply(const subsynth::Patch& p, bool final);  // the track plays it now (its clips rendered: final, or soon)
    void classic();                                   // back to the classic voice (no patch)
    bool choosePreset(const std::string& path);       // load a preset file onto the track
    void reload();                                    // show the track's patch as it is (after undo or redo)
    void showModulation(bool on);                     // the Modulation view (else the page)
    bool modulateFor(int target, int source = -1);    // Modulate… (target -1: any); true if a routing changed
    void savePreset();
    void playNote();
    void done(int result) override;

private:
    int renderTimer_ = -1;
    bool drawnShown_ = false;  // the canvas shown (a Drawn synth), not the page
    void fillPresets(ui::Menu* menu);
    void showPatch();
    void renderClips();
};

// Modulate…: a source, a target and a depth for `patch` (target / source -1: the first); true if OK changed it
bool modulateDialog(ui::Window* parent, subsynth::Patch& patch, int target = -1, int source = -1);

SynthDialog* openSynth(const TrackPtr& track);  // the track's window (one per track), brought forward if open
SynthDialog* openFor(const Track* track);       // the open one, or null
void reloadAll();
void closeGone(const Project& project);  // windows of tracks gone, or playing something else

}  // namespace synth_dialog
