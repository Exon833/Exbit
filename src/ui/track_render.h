// Freezing and bouncing tracks (right-click a track's row), with a progress window (track_render.py).
//
// Freeze: the track is rendered (audio/render.h) and from then on plays that rendering instead of
// its clips and plugins, which saves the CPU its plugins would use. Its clips and plugins are locked
// until it is unfrozen; the rendering is saved with the project.
//
// Bounce: the rendering becomes an ordinary audio clip, either on a new audio track right below
// (the original is kept and muted), or in place: the track's clips, instrument and effects are
// replaced by it (a MIDI track becomes an audio track; its MIDI takes go; Edit → Undo brings it all
// back). A bounce onto a new track keeps the mix: the rendering is the sound before the fader, so the
// new track gets the original's volume, pan, output, delay, sends and volume / pan automation.
//
// Rendering calls the track's plugins from the main thread, so playback stops and the engine leaves
// plugins alone meanwhile (Engine::PluginsPaused). A frozen track bounces its rendering as is.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "audio/render.h"
#include "model/model.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"

namespace audio { class Engine; }

// What TrackRenderer needs from the main window, the timeline and the plugin host.
struct TrackRenderHost {
    ui::Window* window = nullptr;                              // (messages, the progress window's parent)
    std::function<void()> stop;                                // MainWindow::stop
    std::function<void(const std::string&, int)> showMessage;  // statusBar().showMessage(text, ms)
    // timeline.tracks_regrouped: locked clips deselected; lanes and panel redrawn (tracks_changed)
    std::function<void()> tracksRegrouped;
    // timeline.place_clip; when unset, recording::placeClip
    std::function<void(const ClipPtr&, int64_t, const TrackPtr&)> placeClip;
    // PluginHost::close_editor: close the plugin windows of these slots (before they go)
    std::function<void(const std::vector<SlotPtr>&)> closeEditors;
};

class TrackRenderer {
public:
    TrackRenderer(TrackRenderHost host, audio::Engine& engine);
    // the track's right-click menu entries (a separator, Freeze / Unfreeze, the two bounces)
    void fillMenu(ui::Menu* menu, const TrackPtr& track);
    // (start frame, stereo audio) of the track, or nothing (cancelled, failed, or nothing to render)
    std::optional<render::Rendered> render(const TrackPtr& track);
    void freeze(const TrackPtr& track);
    void unfreeze(const TrackPtr& track);
    // in place: asks first ("(Edit → Undo brings them back.)")
    void bounce(const TrackPtr& track, bool inPlace);
    ClipPtr audioClip(const std::string& name, const std::vector<float>& stereo, int64_t start) const;
    // the track has something to render: a rendering, or clips that play
    bool hasAudio(const TrackPtr& track) const;
    TrackRenderHost host;
    // (tests) answer the "Bounce in place" question without asking: 1 yes, 0 no, -1 ask
    int answerInPlace = -1;
private:
    audio::Engine& engine_;
    Project& project() const;
};
