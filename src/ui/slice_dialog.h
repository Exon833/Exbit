// The slice window: an audio clip cut into a new Pad Kit track (the timeline's right-click "Slice to a Pad
// Kit…"; audio/padkit.h SLICING). Part of pad_kit_dialog (pad_kit_dialog.h includes it).
//
// MODES. Transients: the hits dsp::Onsets finds, with a sensitivity slider. Grid: a line every 1/4, 1/8, 1/16
// or 1/32 note, on the song's grid where the clip is (its tempo changes, its time signatures), or at the clip's
// own tempo when it has one (its "Recorded at" BPM: padkit::gridStarts). Manual: the lines as they are, edited
// by hand; any edit switches to it (keeping the lines).
//
// THE WAVEFORM (SliceView) shows the clip's sound with a line where each slice starts. Click inside a slice to
// hear it (Engine::preview, as the Explorer plays a sample: the slice heard before stops). Drag a line to move
// it (it snaps to a hit within SNAP_PX unless Alt is held). Double-click to add a line there, or to remove the
// one under the pointer. The line at the clip's very start can't be moved or removed (the sound before the
// first hit is a slice of its own). Keys (also the terminal's, where it is a list: number, start, length):
// Up / Down choose a slice, Left / Right move its start NUDGE_S (Shift: NUDGE_BIG_S), Space hears it, Insert
// adds a line in its middle, Delete removes its start.
//
// THE COUNT shows how many slices there are; a Pad Kit has PadKit::MAX_PADS pads, so lines past the 32nd are
// drawn in the warning colour (the rest of the sound stays in the last pad's slice). While it is open the
// timeline draws the lines on the clip (hitsChanged → the timeline's slicePreview), following every change.
// "Slice" accepts (the timeline then makes the track: Timeline::makeSlicedKit with `hits`); Cancel leaves
// everything as it was.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "audio/dsp.h"
#include "audio/engine.h"
#include "model/model.h"
#include "ui/sample_waveform.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

namespace pad_kit_dialog {

constexpr double NUDGE_S = 0.005;     // Left / Right move a slice's start this much…
constexpr double NUDGE_BIG_S = 0.05;  // …and with Shift this much
constexpr int SNAP_PX = 8;            // a dragged line snaps to a hit this near (Alt: never)
constexpr int GRAB_PX = 4;            // a line this near the pointer is grabbed (dragged, or removed by a double-click)
extern const int GRID_DIVISIONS[4];   // 4, 8, 16, 32: the Grid's 1/4 … 1/32

class SliceDialog;

// the clip's sound and its slice lines (see the top)
class SliceView : public SampleWaveform {
public:
    explicit SliceView(SliceDialog* dialog);
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override;
    bool keyPress(ui::KeyEvent& e) override;
    bool listRows(std::vector<std::string>& rows, int& current) const override;  // (as text: a row per slice)
    int64_t frameAt(int x) const;  // the frame under x (px)
    int xOf(int64_t frame) const;
    int lineAt(int x) const;       // the line within GRAB_PX of x (an index into hits), or -1
    int dragging = -1;             // the line being dragged (an index into hits)

private:
    SliceDialog* dialog_;
};

class SliceDialog : public ui::Dialog {
public:
    // sr: the rate the clip was prepared at; timing: the song's (the Grid)
    SliceDialog(ui::Window* parent, ClipPtr clip, int sr, const Timing& timing = Timing());
    enum Mode { Transients = 0, Grid = 1, Manual = 2 };
    ClipPtr clip;
    int sr;
    Timing timing;
    AudioPtr sound;                // the clip's sound as it plays (padkit::clipSound), at sr
    std::vector<int64_t> hits;     // where the slices start: frames into the clip's window (all of them, sorted)
    std::vector<int64_t> found;    // the hits Transients finds at this sensitivity (a dragged line snaps to them)
    int chosen = 0;                // the slice chosen (keys, the highlight)
    ui::ComboBox* mode;            // Mode
    ui::Slider* sensitivity;       // 0…100 % (Transients)
    ui::ComboBox* grid;            // GRID_DIVISIONS (Grid)
    SliceView* view;
    ui::Label* count;
    ui::Label* hint;
    ui::Button* sliceButton;
    ui::Signal<> hitsChanged;      // the lines changed (found again, or edited)

    void findHits();               // the lines of the mode chosen (Manual: kept)
    void setMode(Mode m);
    // edits (each switches to Manual); a line at frame 0 stays where it is
    int moveLine(int index, int64_t frame, bool snap, int64_t reach);  // the line's index after
    void addLine(int64_t frame);
    void removeLine(int index);
    void nudge(int direction, bool big);  // the chosen slice's start, earlier (-1) or later (+1)
    void choose(int slice);
    void audition(int slice);      // hear it (the slice heard before stops)
    int sliceAt(int64_t frame) const;
    int64_t frames() const { return sound ? sound->frames() : 0; }
    int64_t sliceEnd(int slice) const;
    void done(int result) override;  // (the slice playing stops)

private:
    std::unique_ptr<dsp::Onsets> onsets_;
    float top_ = 0;  // the sound's loudest sample
    std::shared_ptr<audio::PreviewVoice> voice_;  // the slice playing (ours while the engine still plays it)
    void setHits(std::vector<int64_t> made);  // sorted, in range (and a line at 0 when there's sound before), shown
    void edited();                            // switched to Manual
};

}  // namespace pad_kit_dialog
