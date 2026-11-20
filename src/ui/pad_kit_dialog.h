// The Pad Kit's window (a MIDI track's instrument "Pad Kit…": audio/padkit.h, model.h PAD KIT), and the
// small window slicing an audio clip into a new Pad Kit track (the timeline's right-click "Slice to a Pad
// Kit…").
//
// THE PAD KIT WINDOW. A 4 × 4 grid of pads (Pads 1–16 / 17–32 switch between the two banks), each showing
// its sound's name, its waveform, its key and the colour of its choke group (none: no colour). Click a pad
// to hear it and select it: its settings are shown on the right (its key, choke group, then the built-in
// sampler's own controls: sampler_dialog::SoundControls, without the root key: a pad plays its sample at
// its own pitch, Tune tuning it). Drop sound files on a pad (from the Explorer or the file manager): they
// fill that pad and the ones after it. Right-click a pad: Load sample…, Clear, Choke group ▸ None / 1…8.
// Every change replaces the track's kit as a whole (undo sees it; live notes play the new one at once) and
// renders the track's clips again a moment after the last one, as the sampler's window does. Like the FX
// windows it doesn't block anything; one per track.
//
// THE SLICE WINDOW. Finds the hits in the clip's sound (dsp::Onsets) with a sensitivity slider; while it is
// open the timeline draws a line on the clip where each slice would start (hitsChanged → the timeline's
// slicePreview). "Slice" accepts it (the timeline then makes the track: Timeline::makeSlicedKit), Cancel
// leaves everything as it was.
//
// THE MAIN WINDOW: uses sampler_dialog's Host (set once at start-up); after undo / redo:
// pad_kit_dialog::reloadAll(); when tracks go away: pad_kit_dialog::closeGone(project).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "audio/dsp.h"
#include "model/model.h"
#include "ui/sample_waveform.h"
#include "ui/sampler_dialog.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"

namespace pad_kit_dialog {

constexpr int PAD_W = 116;  // px: a pad's least width (its height: SampleWaveform::HEIGHT)
constexpr int COLUMNS = 4;  // pads per row (a bank: PadKit::BANK pads)

Color chokeColor(int group);           // the colour of a choke group (1…8); group 0: none (transparent)
std::string chokeName(int group);      // "None", "1"… "8"

class PadKitDialog;

// One pad of the grid: its waveform (SampleWaveform's peaks), name, key and choke group.
class PadView : public SampleWaveform {
public:
    PadView(PadKitDialog* dialog, int slot);
    int slot;  // its place in the bank (the pad: bank * PadKit::BANK + slot)
    int pad() const;
    void refresh();  // show the pad as the track's kit has it now
    void paint(Painter& p) override;
    void styleStates(std::vector<std::string>& out) const override;  // and :selected, :dropping
    bool mousePress(ui::MouseEvent& e) override;
    bool contextMenu(ui::MouseEvent& e) override;
    // keys (the keyboard alone, tinydaw --tui): the arrows to the pad beside it (the grid's 4 × 4), Enter or
    // Space plays it, L loads a sample onto it, Delete clears it, F2 its menu
    bool keyPress(ui::KeyEvent& e) override;
    bool listRows(std::vector<std::string>& rows, int& current) const override;  // (as text: its number, sample, key)
    void dragEnter(ui::DragEvent& e) override;
    void dragLeave() override;
    void drop(ui::DragEvent& e) override;

private:
    PadKitDialog* dialog_;
    bool dropping_ = false;  // files being dragged over it
};

class PadKitDialog : public ui::Dialog {
public:
    PadKitDialog(ui::Window* window, TrackPtr track);
    ~PadKitDialog() override;
    TrackPtr track;
    int selected = 0;  // the pad whose settings are shown (0 … MAX_PADS - 1)
    int bank = 0;      // 0: pads 1–16; 1: pads 17–32

    PadKitPtr kit() const;  // the track's (an empty one if it has none)
    // replace the track's kit (a change: its clips are rendered again a moment later)
    void setKit(const PadKitPtr& kit);
    void select(int pad, bool play);  // show its settings (its bank too); and play it
    void play(int pad);               // hear it on the track (also: the MIDI editor's keys, a MIDI keyboard)
    void showBank(int bank);
    void reload();                    // the track's kit as it is now (after undo or redo), not a change
    // sound files onto pad `pad` and the ones after it; the messages of those that couldn't be loaded shown
    void dropFiles(int pad, const std::vector<std::string>& paths);
    void loadSample(int pad);         // Load sample…: asks for one
    void clearPad(int pad);           // its sample gone (its key and choke group stay)
    void setChoke(int pad, int group);
    void fillPadMenu(ui::Menu& menu, int pad);  // a pad's right-click menu
    void renderClips();
    void done(int result) override;

    std::vector<PadView*> pads;       // the grid (one bank)
    ui::Button *bankA, *bankB;
    ui::Label* hint;                  // what to do, or why files couldn't be loaded
    ui::Label* padTitle;
    ui::Button* loadButton;
    sampler_dialog::SamplerWaveform* waveform;
    ui::SpinBox* key;
    ui::ComboBox* choke;
    sampler_dialog::SoundControls controls;

private:
    int renderTimer_ = -1;
    bool reloading_ = false;
    void showPad();                   // the selected pad's settings in the controls
    void changed();                   // the controls changed the selected pad
    void scheduleRender();
};

// the track's Pad Kit window (one per track), brought forward if it is open
PadKitDialog* openPadKit(const TrackPtr& track);
PadKitDialog* openFor(const Track* track);  // the open one, or null
void reloadAll();                            // every open window shows its track's kit again (undo / redo; none: closed)
void closeGone(const Project& project);      // close the windows of tracks that are gone (or play no kit)

// --- slicing an audio clip ---
class SliceDialog : public ui::Dialog {
public:
    SliceDialog(ui::Window* parent, ClipPtr clip, int sr);  // sr: the rate the clip was prepared at
    ClipPtr clip;
    int sr;
    std::vector<float> sound;      // the clip's sound as it plays (padkit::clipSound)
    std::vector<int64_t> hits;     // where the slices start: frames into the clip's window (padkit::sliceStarts)
    ui::Slider* sensitivity;       // 0…100 %
    ui::Label* count;
    ui::Button* sliceButton;
    ui::Signal<> hitsChanged;      // the hits were found again (the sensitivity changed)
    void findHits();

private:
    std::unique_ptr<dsp::Onsets> onsets_;
};

}  // namespace pad_kit_dialog
