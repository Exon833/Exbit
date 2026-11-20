// Right-click audio clips → Warp… (was "Stretch and pitch…"): warping (audio/warp.h, model/warping.h), and the
// old stretch and pitch inside it.
//
//   Warp            on: the clip's audio follows the song's tempo, its changes too (its beats on the song's)
//   Mode            Beats (drums: cut at transients, placed, not stretched), Tones (one instrument, a voice),
//                   Complex (full mixes: transients kept); Beats keeps its transients, or every 1/4, 1/8, 1/16
//   Tempo           the tempo the audio was made at (its markers are straight at it until moved); "Detected:"
//                   what its file says (acid chunk, file name) or its sound (analysis), and Use: that tempo,
//                   markers straight at it, warp on
//   Length          (not warped) how long it plays compared with how it was recorded; Match: the song's tempo
//   Pitch           semitones and cents, warped or not
//   Markers         its warp markers: each one's beat (bar.beat of the song where it plays) and time; ←/→ move
//                   the chosen one by the grid (between its neighbours), Delete removes it; "Warp from here":
//                   a marker at the clicked place (or the cursor) on the nearest bar, straight from there
// The terminal UI gets it as every dialog: the list takes ←/→ and Delete there too.
#pragma once
#include <functional>
#include <memory>

#include "audio/tempo_detect.h"
#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

// the markers' list: ←/→ move the chosen marker (moved(row, -1 / +1)), Delete removes it
class WarpMarkerList : public ui::ListWidget {
public:
    using ui::ListWidget::ListWidget;
    ui::Signal<int, int> moved;
    ui::Signal<int> removed;
    bool keyPress(ui::KeyEvent& e) override;
};

// After run() (or exec() and accept): the settings chosen. `markers` is set when they were changed here (moved,
// removed, Use, Warp from here); null: each clip keeps its own.
class WarpDialog : public ui::Dialog {
public:
    // `at`: the timeline frame clicked (or the cursor) for Warp from here; `gridBeats`: what ←/→ move a marker by
    WarpDialog(ui::Window* parent, const Clip& clip, TempoMapPtr map, int count, int sr, int64_t at, double gridBeats);
    bool warp = false;
    warp::Mode mode = warp::Mode::Beats;
    int preserve = warp::TRANSIENTS;
    double stretch = 1.0, pitch = 0.0, sourceBpm = 0.0;
    std::shared_ptr<const warp::Markers> markers;
    tempo_detect::Found detected;  // (worked out when it opens)

    ui::CheckBox* warpBox;
    ui::ComboBox* modeBox;
    ui::ComboBox* preserveBox;
    ui::SpinBox* bpm;          // "not known" at 0
    ui::Label* detectedLabel;
    ui::Button* useButton;
    ui::SpinBox* length;       // %
    ui::Button* matchButton;
    ui::SpinBox* semitones;
    ui::SpinBox* cents;
    ui::Button* resetButton;   // "As recorded"
    WarpMarkerList* markerList;
    ui::Button* fromHereButton;
    ui::Button* removeButton;

    void use();                    // the detected tempo: markers straight at it, warp on
    void match();                  // (not warped) the length that plays its tempo at the song's
    void fromHere();               // Warp from here, at `at`
    void moveMarker(int row, int steps);  // by the grid, between its neighbours
    void removeMarker(int row);
    const warp::Markers& markerRows() const { return rows_; }
    void done(int result) override;
    bool run();
    static std::function<bool(WarpDialog&)> testRun;  // tests: instead of showing it
private:
    Clip clip_;
    TempoMapPtr map_;
    double songBpm() const;  // (the song's average tempo over the clip)
    int sr_;
    int64_t at_;
    double grid_;
    warp::Markers rows_;
    bool edited_ = false;
    void read();
    void fill(int select);
    void enable();
};
