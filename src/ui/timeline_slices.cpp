// The timeline's "Slice to a Pad Kit…" (timeline.h): the slice window, the lines it shows on the clip, and
// the Pad Kit track it makes (audio/padkit.h SLICING).
#include <algorithm>

#include "audio/padkit.h"
#include "core/util.h"
#include "ui/pad_kit_dialog.h"
#include "ui/timeline.h"

pad_kit_dialog::SliceDialog* Timeline::sliceToPadKit(const ClipPtr& clip) {
    if (!clip || clip->kind != Kind::Audio || !clip->data) return nullptr;
    if (sliceWindow && ui::Widget::alive(sliceWindow)) sliceWindow->reject();  // (one at a time: its lines go)
    sliceWindow = nullptr;
    auto* dialog = new pad_kit_dialog::SliceDialog(dialogParent(), clip, engine.sr);
    sliceWindow = dialog;
    slicePreview = SlicePreview{clip, dialog->hits};
    std::shared_ptr<bool> alive = alive_;
    dialog->hitsChanged.connect([this, dialog, alive] {
        if (!*alive || sliceWindow != dialog) return;
        slicePreview = SlicePreview{dialog->clip, dialog->hits};
        update();
    });
    dialog->finished.connect([this, dialog, alive](int result) {
        if (!*alive || sliceWindow != dialog) return;
        sliceWindow = nullptr;
        ClipPtr clip = dialog->clip;
        std::vector<int64_t> starts = dialog->hits;
        slicePreview.reset();
        update();
        if (result == ui::Dialog::Accepted && project.clips.contains(clip)) makeSlicedKit(clip, starts);
    });
    dialog->open();
    update();
    return dialog;
}

TrackPtr Timeline::makeSlicedKit(const ClipPtr& clip, const std::vector<int64_t>& starts) {
    if (!clip || clip->kind != Kind::Audio) return nullptr;
    std::vector<float> sound = padkit::clipSound(*clip);
    int64_t frames = (int64_t)sound.size() / 2;
    if (frames <= 0) return nullptr;
    std::string name = util::stem(clip->name);
    if (name.empty()) name = "Slices";
    padkit::Sliced sliced = padkit::slice(sound.data(), frames, engine.sr, starts, name);
    TrackPtr owner = clip->track;
    TrackPtr track = addTrack(Kind::Midi, owner ? owner->group : nullptr, owner);
    track->name = name + " kit";
    track->needsInstrument = false;
    std::atomic_store(&track->padKit, sliced.kit);
    auto made = std::make_shared<Clip>();
    made->kind = Kind::Midi;
    made->name = name;
    made->notes = std::make_shared<const std::vector<Note>>(sliced.notes);
    made->lengthS = sliced.lengthS;
    made->length = frames;  // (exactly as long as the audio clip: what it played, where it played it)
    // the loop now plays from the kit: the audio clip switched off (kept, drawn as an outline: right-click →
    // Enable clips brings it back; so does undo, with the rest)
    clip->disabled = true;
    placeClip(made, clip->start, track);  // (prepared on its track: through the kit)
    tracksChanged.emit();                 // (its row shows its instrument now)
    return track;
}
