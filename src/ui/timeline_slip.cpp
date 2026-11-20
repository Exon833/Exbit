// timeline.py's successor, part: editing every selected clip at once (model/clip_edits.h). Ctrl+Alt+drag a clip
// slips the selected clips' contents (on the grid's steps; Shift as well: freely); the clip menu's Slip
// contents…, Crop to the selection and Fades at every boundary…. (Dragging an edge or a fade handle of a
// selected clip changes every selected clip: timeline.cpp's dragEdge, timeline_edits.cpp's dragFade.)
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/util.h"
#include "ui/clip_edit_dialogs.h"
#include "ui/timeline.h"

std::vector<ClipPtr> Timeline::editableSelected() const {
    std::vector<ClipPtr> out;
    for (auto& c : project.clips)
        if (selection.contains(c) && !(c->track && c->track->frozen)) out.push_back(c);
    return out;
}

double Timeline::gridStepAt(int64_t frame, bool beats) const {
    const Timing& t = project.timing;
    double seconds = std::max(0.0, (double)frame / engine.sr);
    double step = t.gridStep(pps, seconds).value();  // (beats)
    return beats ? step : step * t.beatSAt(seconds) * engine.sr;
}

std::string Timeline::slipText(int64_t frames, int64_t at) const {
    char buf[64];
    double seconds = (double)frames / engine.sr;
    if (project.timing.beats)
        std::snprintf(buf, sizeof buf, "%+.3g beats", seconds / project.timing.beatSAt(std::max(0.0, (double)at / engine.sr)));
    else
        std::snprintf(buf, sizeof buf, "%+.3f s", seconds);
    return buf;
}

// --- slip ---

void Timeline::slipSelected(int64_t frames) {
    auto clips = editableSelected();
    if (clips.empty()) return;
    clip_edits::slip(project, clips, frames, engine.sr);
    update();
    std::string text = "Slip " + slipText(frames, clips[0]->start) + ": the contents of " + std::to_string(clips.size()) +
                       (clips.size() == 1 ? " clip" : " clips");
    util::debugf("timeline: %s\n", text.c_str());
    if (status) status(text, 4000);
}

void Timeline::askSlip() {
    auto clips = editableSelected();
    if (clips.empty()) return;
    bool beats = project.timing.beats;
    int64_t at = clips[0]->start;
    double step = beats ? gridStepAt(at, true) : gridStepAt(at) / engine.sr;
    double amount = 0;
    if (!clip_edit_dialogs::askSlip(dialogParent(), (int)clips.size(), beats ? "beats" : "s", step, amount)) return;
    double seconds = beats ? amount * project.timing.beatSAt(std::max(0.0, (double)at / engine.sr)) : amount;
    int64_t frames = (int64_t)std::llround(seconds * engine.sr);
    if (frames) slipSelected(frames);
}

void Timeline::startSlip(PointF pos, const ClipPtr& clip) {
    if (!selection.contains(clip)) selection.selectOnly(clip);
    SlipDrag d;
    d.clip = clip;
    d.x = pos.x;
    d.from = clip_edits::slipStart(project, editableSelected());
    drag = d;
    setCursor(ui::Cursor::SizeHor);  // (⇔: the contents move, the clip stays)
    util::debugf("timeline: slip begins on %zu clips\n", d.from.size());
    if (status) status("Slip: drag to move the contents inside the clips (Shift as well: off the grid)", 0);
}

void Timeline::dragSlip(PointF pos, int mods) {
    auto& d = std::get<SlipDrag>(drag);
    int64_t frames = frameAt(pos.x) - frameAt(d.x);
    if (project.timing.snaps((mods & plat::ModShift) != 0)) {  // whole grid steps (Shift: freely)
        double step = std::max(1.0, gridStepAt(d.clip->start));
        frames = (int64_t)std::llround(std::nearbyint((double)frames / step) * step);
    }
    if (frames == d.frames) return;
    d.frames = frames;
    clip_edits::slipFrom(project, d.from, frames, engine.sr);
    if (status) status("Slip " + slipText(frames, d.clip->start), 0);
    update();
}

// --- crop ---

void Timeline::cropSelected() {
    if (!selection.range) return;
    const TimeRange r = *selection.range;
    std::vector<ClipPtr> clips = editableSelected();
    for (auto& c : project.clipsIn(r.tracks, r.start, r.end))
        if (std::find(clips.begin(), clips.end(), c) == clips.end() && !c->track->frozen) clips.push_back(c);
    auto made = clip_edits::crop(project, clips, r.start, r.end);
    replaceClips(made);
    std::string text = "Cropped " + std::to_string(made.size()) + (made.size() == 1 ? " clip" : " clips") + " to the selection";
    util::debugf("timeline: %s\n", text.c_str());
    if (status) status(text, 4000);
}

// --- fades ---

void Timeline::askFades() {
    std::vector<ClipPtr> clips;
    for (auto& c : editableSelected())
        if (c->kind == Kind::Audio) clips.push_back(c);
    if (clips.empty()) return;
    clip_edit_dialogs::FadeChoice choice;
    int64_t in, out;
    clips[0]->fades(in, out);  // (the first one's, to start from)
    if (in || out) {
        choice.inMs = 1000.0 * in / engine.sr;
        choice.outMs = 1000.0 * out / engine.sr;
    }
    choice.inShape = clips[0]->fadeInShape;
    choice.outShape = clips[0]->fadeOutShape;
    if (!clip_edit_dialogs::askFades(dialogParent(), (int)clips.size(), choice)) return;
    clip_edits::Fades f;
    f.in = (int64_t)std::llround(std::max(0.0, choice.inMs) / 1000 * engine.sr);
    f.out = (int64_t)std::llround(std::max(0.0, choice.outMs) / 1000 * engine.sr);
    f.inShape = choice.inShape;
    f.outShape = choice.outShape;
    int n = clip_edits::setFades(clips, f);
    update();
    if (status) status("Fades on " + std::to_string(n) + (n == 1 ? " audio clip" : " audio clips"), 4000);
}

// --- the clip menu's entries ---

void Timeline::addClipEditActions(ui::Menu& menu) {
    auto clips = editableSelected();
    int audio = 0;
    for (auto& c : clips) audio += c->kind == Kind::Audio;
    ui::Action* slip = menu.addAction("Slip contents…", [this] { askSlip(); });
    slip->tooltip = "Move the audio (or notes) inside the selected clips, the clips staying where they are "
                    "(or Ctrl+Alt+drag a clip; Shift as well: off the grid)";
    slip->setEnabled(!clips.empty());
    ui::Action* crop = menu.addAction("Crop to the selection", [this] { cropSelected(); });
    crop->tooltip = "Cut the selected clips, and every clip on the selected range's tracks, to the range";
    crop->setEnabled(selection.range.has_value() && selection.range->end > selection.range->start);
    ui::Action* fades = menu.addAction("Fades at every boundary…", [this] { askFades(); });
    fades->tooltip = "One fade-in and fade-out (length and shape) for every selected audio clip";
    fades->setEnabled(audio > 0);
}
