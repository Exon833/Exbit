#include "ui/track_render.h"

#include <exception>

#include "audio/engine.h"
#include "audio/patch.h"
#include "model/pad_tracks.h"
#include "ui/recording.h"

using namespace ui;

TrackRenderer::TrackRenderer(TrackRenderHost h, audio::Engine& engine) : host(std::move(h)), engine_(engine) {}

Project& TrackRenderer::project() const { return engine_.project; }

bool TrackRenderer::hasAudio(const TrackPtr& track) const {
    if (std::atomic_load(&track->frozen)) return true;
    for (auto& c : project().clipsOn(track))
        if (!c->disabled) return true;
    if (TrackPtr kit = pad_tracks::kitOf(project(), *track))  // (a pad track: its kit's clips play its pad)
        for (auto& c : project().clipsOn(kit))
            if (!c->disabled) return true;
    return false;
}

void TrackRenderer::fillMenu(Menu* menu, const TrackPtr& track) {
    bool has = hasAudio(track);
    menu->addSeparator();
    if (std::atomic_load(&track->frozen)) {
        menu->addAction("Unfreeze track", [this, track] { unfreeze(track); });
    } else {
        Action* freezeAction = menu->addAction("Freeze track", [this, track] { freeze(track); });
        freezeAction->setEnabled(has);
    }
    Action* bounceAction = menu->addAction("Bounce to new audio track", [this, track] { bounce(track, false); });
    bounceAction->setEnabled(has);
    Action* inPlace = menu->addAction("Bounce in place…", [this, track] { bounce(track, true); });
    inPlace->setEnabled(has);
    // (pads on their own tracks, model/pad_tracks.h: a pad track's pad would still play into it, a kit's routed pads
    // would be lost)
    if (pad_tracks::kitOf(project(), *track) || pad_tracks::routedMask(*track)) {
        inPlace->setEnabled(false);
        inPlace->tooltip = pad_tracks::kitOf(project(), *track)
                               ? "A pad's own track: bounce it to a new track, or freeze it"
                               : "Its pads play on tracks of their own: bounce it to a new track, or freeze it";
    }
}

std::optional<render::Rendered> TrackRenderer::render(const TrackPtr& track) {
    ClipPtr frozen = std::atomic_load(&track->frozen);
    if (frozen) {  // already rendered
        render::Rendered r;
        r.start = frozen->start;
        if (frozen->data) r.audio = frozen->data->samples;
        return r;
    }
    if (host.stop) host.stop();
    ProgressDialog dialog(host.window, "Rendering", "Rendering " + track->name + "…", true);
    dialog.setRange(0, 100);
    double shownFrom = App::get().now() + 0.3;  // (QProgressDialog's minimum duration: 300 ms)
    auto progress = [&](double done) {
        if (dialog.isWindowShown() || App::get().now() >= shownFrom) dialog.setValue((int)(done * 100));
        else App::get().processEvents();
        return !dialog.wasCanceled();
    };
    std::optional<render::Rendered> result;
    try {
        audio::Engine::PluginsPaused paused(engine_);  // this thread calls the track's plugins now
        result = render::renderTrack(project(), track, engine_.sr, progress);
    } catch (const std::exception& e) {
        if (dialog.isWindowShown()) dialog.hideWindow();
        msg::warning(host.window, "Cannot render track", track->name + "\n\n" + e.what());
        return std::nullopt;
    }
    if (dialog.isWindowShown()) dialog.hideWindow();
    return result;
}

void TrackRenderer::freeze(const TrackPtr& track) {
    auto result = render(track);
    if (!result) return;
    ClipPtr clip = audioClip(track->name + " (frozen)", result->audio, result->start);
    std::atomic_store(&track->frozen, clip);  // (the audio thread reads it)
    if (host.tracksRegrouped) host.tracksRegrouped();  // its clips are locked now: deselect them; redraw
    if (host.showMessage)
        host.showMessage(track->name + " is frozen: its plugins rest until you unfreeze it", 5000);
}

void TrackRenderer::unfreeze(const TrackPtr& track) {
    std::atomic_store(&track->frozen, ClipPtr());
    if (host.tracksRegrouped) host.tracksRegrouped();
}

void TrackRenderer::bounce(const TrackPtr& track, bool inPlace) {
    if (inPlace) {
        bool plugins = !track->instruments().empty() || !track->effects.empty();
        int answer = answerInPlace;
        if (answer < 0)
            answer = msg::question(host.window, "Bounce in place",
                                   "Replace the clips of " + track->name +
                                       (plugins ? ", its instrument and its effects" : "") +
                                       " with one audio clip of how it sounds now?\n(Edit → Undo brings them back.)") ==
                     msg::Yes;
        if (!answer) return;
    }
    auto result = render(track);
    if (!result) return;
    ClipPtr clip = audioClip(track->name + " (bounced)", result->audio, result->start);
    Project& p = project();
    TrackPtr target;
    if (inPlace) {
        std::vector<SlotPtr> slots;
        if (track->instrument) slots.push_back(track->instrument);
        for (auto& s : track->rack)
            if (s) slots.push_back(s);
        for (auto& s : track->effects)
            if (s) slots.push_back(s);
        if (host.closeEditors) host.closeEditors(slots);
        p.removeClips(p.clipsOn(track));
        track->cables = straight();  // (first: the audio thread never finds cables to effects that are gone)
        track->rackCables = std::vector<Cable>{};
        track->kind = Kind::Audio;
        std::atomic_store(&track->instrument, SlotPtr());
        track->effects = std::vector<SlotPtr>{};
        std::atomic_store(&track->frozen, ClipPtr());
        track->needsInstrument = false;
        track->usesRack = false;
        track->rack = std::vector<SlotPtr>{};
        track->sampler = nullptr;
        track->padKit = nullptr;
        std::vector<ClipPtr> audioTakes;  // (a MIDI track's MIDI takes can't be used now)
        for (auto& t : track->takes)
            if (t->kind == Kind::Audio) audioTakes.push_back(t);
        track->takes = audioTakes;
        track->showTakes = track->showTakes && audioTakes.size() > 1;
        for (auto& s : slots)  // (kept alive a moment: the audio thread may still hold them)
            if (s->instance) p.retire(s->instance);
        target = track;
    } else {
        target = p.addTrack(Kind::Audio, track->group, track);
        target->name = track->name + " bounce";
        // (the rendering is the sound before the fader: the new track mixes it as the original did)
        target->volumeDb = track->volumeDb;
        target->pan = track->pan;
        target->output = track->output;
        target->delayMs = track->delayMs;
        target->sends = track->sends.copy();
        std::vector<AutomationPtr> automation;
        for (auto& a : track->automation)
            if (a->slot == track->controls) {
                auto copy = std::make_shared<Automation>();
                copy->slot = target->controls;
                copy->param = a->param;
                copy->points = a->points.copy();
                copy->height = a->height;
                automation.push_back(copy);
            }
        target->automation = automation;
        target->showAutomation = track->showAutomation && !automation.empty();
        track->muted = true;  // the bounce plays instead (unmute to hear both)
    }
    patch::ensureStates(p);
    if (host.placeClip) host.placeClip(clip, result->start, target);
    else recording::placeClip(p, clip, result->start, target, engine_.sr);
    if (host.tracksRegrouped) host.tracksRegrouped();
}

ClipPtr TrackRenderer::audioClip(const std::string& name, const std::vector<float>& stereo, int64_t start) const {
    auto source = std::make_shared<AudioBuffer>();
    source->samples = stereo;
    ClipPtr clip = makeAudioClip(name, source, engine_.sr, engine_.sr);
    clip->start = start;
    return clip;
}
