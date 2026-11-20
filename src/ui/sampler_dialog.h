// The built-in sampler's window (a MIDI track's instrument "Built-in sampler"; audio/sampler): its
// sample (Choose… another) and waveform (the part played between start and end shown), the key it
// plays as recorded, tuning, level, velocity, the envelope, classic or one-shot, looping, and the part
// of the sample played (sampler_dialog.py). Changes are heard at once: the track's clips are rendered
// again a moment after the last change. Like the FX windows it doesn't block anything; one per track.
//
// THE MAIN WINDOW: once at start-up
//   sampler_dialog::setHost({this, &project, &engine, [this] { timeline->relayout(); }});
// (the MIDI effects window and the instrument menu's "Built-in sampler…" use it too). After undo / redo:
// sampler_dialog::reloadAll(); when tracks go away: sampler_dialog::closeGone(project).
#pragma once
#include <functional>
#include <string>

#include "audio/engine.h"
#include "model/model.h"
#include "ui/sample_waveform.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

namespace sampler_dialog {

constexpr int RENDER_DELAY_MS = 250;  // after the last change, the track's clips are rendered again

// what the MIDI dialogs need from the main window
struct Host {
    ui::Window* window = nullptr;   // their parent
    Project* project = nullptr;
    audio::Engine* engine = nullptr;
    std::function<void()> relayout;  // timeline.relayout(): the clips' new renderings shown
    std::function<void()> tracksChanged;  // tracks added (a Pad Kit's pads on their own tracks): the rows shown again
    std::function<void(const std::string&)> status;  // a message in the main window's status bar
};
void setHost(const Host& host);
const Host& host();

// Render a MIDI track's clips again (its built-in instrument or MIDI effects changed), and its takes
// (their lanes show, and comping places, what they would play now)
void renderTrackClips(const TrackPtr& track);

// the key a sample's file name says it was recorded at ("Piano C4.wav", "flute_A#3"), else 60
int rootFromName(const std::string& name);
std::string noteName(int pitch);  // 60 -> "C4"
// ask for an audio file: an audio clip holding it (name, source, sourceSr), or null
ClipPtr chooseSample(ui::Window* parent, const std::string& startDir = "");
// settings for a sample just chosen: its root from its file name (a key in it, like "C3", else C4)
SamplerPtr newSettings(const ClipPtr& sample);

// The sampler's settings as controls in a form (its window's, and a Pad Kit pad's: pad_kit_dialog.h): its
// root key (unless left out), tuning, level, velocity, envelope, classic or one-shot, looping, start and end.
// show() puts settings in them without it counting as a change; read() writes what they show into settings
// (the sample and anything they don't show kept); changed is emitted on each change made in them.
struct SoundControls {
    ui::SpinBox *root = nullptr, *transpose = nullptr, *gain = nullptr, *velocity = nullptr, *attack = nullptr,
                *decay = nullptr, *sustain = nullptr, *release = nullptr, *start = nullptr, *end = nullptr;
    ui::ComboBox* mode = nullptr;
    ui::CheckBox* loop = nullptr;
    // the controls made as children of `owner`, each a row of `form`; transposeLabel: its row's name
    void build(ui::Widget* owner, ui::FormLayout* form, bool withRoot, const std::string& transposeLabel = "Transpose");
    void show(const SamplerSettings& s);
    void read(SamplerSettings& s) const;
    void connect(std::function<void()> changed);  // called on each change made in them
    // a number box as the sampler's window has them (changed once a value is typed in, not on every key)
    static ui::SpinBox* number(ui::Widget* owner, double low, double high, int decimals, const std::string& suffix,
                               const std::string& tip = "");
};

// the sample's waveform, the parts before its start and after its end shaded (they don't play). Styled as
// SamplerWaveform (a SampleWaveform): ::outside (the shade: background-color), ::marker (the start's and end's
// lines: background-color, width); the lines go from SampleWaveform::caption's height down.
class SamplerWaveform : public SampleWaveform {
public:
    explicit SamplerWaveform(ui::Widget* parent = nullptr);
    double start = 0, end = 1;
    void paint(Painter& p) override;

private:
    struct Look {
        const css::Style* outside = nullptr;  // (the shades' boxes; kept until ownParts_.refresh() answers true)
        Color marker;
        int top = 14, markerW = 1;
    };
    panel_style::Parts ownParts_{*this};  // (apart from SampleWaveform's: each refreshes its own)
    Look look_;
};

class SamplerDialog : public ui::Dialog {
public:
    SamplerDialog(ui::Window* window, TrackPtr track);
    ~SamplerDialog() override;
    TrackPtr track;
    void showSettings(const SamplerSettings& s);
    SamplerPtr settings() const;   // the settings as the window shows them
    void reload();                 // the track's sampler as it is now (after undo or redo), not a change
    void changed();
    void choose();                 // Choose…: another sample, the settings kept
    void renderClips();
    void playRoot();
    void done(int result) override;

    ui::Label* sampleName;
    SamplerWaveform* waveform;
    SoundControls controls;
    ui::SpinBox *root, *transpose, *gain, *velocity, *attack, *decay, *sustain, *release, *start, *end;  // (controls')
    ui::ComboBox* mode;
    ui::CheckBox* loop;

private:
    int renderTimer_ = -1;
    bool reloading_ = false;
};

// the track's sampler window (one per track), brought forward if it is open
SamplerDialog* openSampler(const TrackPtr& track);
SamplerDialog* openFor(const Track* track);  // the open one, or null
void reloadAll();                            // every open window shows its track's sampler again (undo / redo)
void closeGone(const Project& project);      // close the windows of tracks that are gone (or play no sampler)

}  // namespace sampler_dialog
