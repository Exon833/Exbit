// The timeline, headless: snapping in beats and in seconds, hit testing of a clip's handle, body and
// ends, moving clips (with and without Alt, between lanes), trimming limits, duplicating a range
// again and again, splitting at the cursor, cutting a range out, drops on the grid and onto new
// tracks, new MIDI clips, automation points, the loop band, keys, the tile cache, the overview, and
// stylesheets restyling all of it (sizes, colours, clip parts).
// Paints the timeline, the master lane and the overview into PNGs in build/ui-samples/timeline/.
#include <cmath>
#include <map>
#include <cstdio>
#include <string>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/file_io.h"
#include "audio/sound_file.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/edits.h"
#include "ui/clip_painter.h"
#include "ui/master_lane.h"
#include "ui/overview.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/waveform_cache.h"
#include "ui/clipboard.h"
#include "ui/warp_dialog.h"
#include "ui/tempo_dialog.h"
#include "ui/track_dialogs.h"
#include "ui/toolkit/menu.h"

namespace {

const int SR = 48000;

std::string outDir() {
    std::string d = "build/ui-samples/timeline";
    util::makeDirs(d);
    return d;
}

std::string outDir2() {  // (the successor's features)
    std::string d = "build/ui-samples/timeline2";
    util::makeDirs(d);
    return d;
}

ui::Action* actionStarting(const ui::Menu& m, const std::string& prefix) {
    for (auto& a : m.actions())
        if (a->text.compare(0, prefix.size(), prefix) == 0) return a.get();
    return nullptr;
}

// the dialogs answered by the test: title -> the value typed (and OK)
void answerWith(std::map<std::string, std::string> answers) {
    track_dialogs::answer = [answers](const std::string& title, std::string& value) {
        auto it = answers.find(title);
        if (it == answers.end()) return false;
        value = it->second;
        return true;
    };
}

AudioPtr sine(double seconds, double hz, double amp = 0.8) {
    auto a = std::make_shared<AudioBuffer>();
    int64_t n = (int64_t)(seconds * SR);
    a->samples.resize(n * 2);
    for (int64_t i = 0; i < n; ++i) {
        double env = 0.4 + 0.6 * std::fabs(std::sin(M_PI * i / (SR * 0.75)));
        float v = (float)(amp * env * std::sin(2 * M_PI * hz * i / SR));
        a->samples[i * 2] = a->samples[i * 2 + 1] = v;
    }
    return a;
}

ClipPtr audioClip(Project& p, const TrackPtr& t, double start, double seconds, const std::string& name = "Sine") {
    auto c = makeAudioClip(name, sine(seconds, 3), SR, SR);
    c->track = t;
    c->start = (int64_t)(start * SR);
    p.addClip(c);
    return c;
}

ClipPtr midiClip(Project& p, const TrackPtr& t, double start, double seconds) {
    std::vector<Note> notes;
    for (int i = 0; i < 8; ++i) notes.push_back({i * 0.25, i * 0.25 + 0.2, 60.0 + (i * 5) % 14, 100});
    notes.push_back({0, 2, 48, 90});
    auto c = makeMidiClip("Melody", notes, seconds, SR);
    c->track = t;
    c->start = (int64_t)(start * SR);
    p.addClip(c);
    return c;
}

ui::MouseEvent mouse(double x, double y, int button = plat::LeftButton, int mods = 0) {
    ui::MouseEvent e;
    e.pos = Point((int)x, (int)y);
    e.globalPos = e.pos;
    e.button = button;
    e.buttons = button;
    e.mods = mods;
    return e;
}

struct Rig {
    Project project;
    audio::Engine engine{project};
    ui::ScrollArea scroll;
    Timeline* tl;
    Rig(int w = 900, int h = 420) {
        scroll.setHorizontalPolicy(ui::ScrollArea::Policy::AlwaysOn);
        scroll.resize(w, h);
        tl = scroll.makeContent<Timeline>(project, engine, &scroll);
        tl->relayout();
    }
    int64_t sec(double s) const { return (int64_t)std::llround(s * SR); }
};

Image paintScroll(ui::Widget& w) {
    ui::App::get().doLayouts();
    Image img(std::max(1, w.width()), std::max(1, w.height()), theme::c("window"));
    Painter p(img);
    w.paintTree(p, w.rect());
    return img;
}

void testSnapping() {
    Rig r;
    Timeline& tl = *r.tl;
    Timing& t = r.project.timing;
    t.beats = false;
    CHECK(tl.snappedFrameAt(150.7, 0) == tl.frameAt(150.7));
    CHECK(tl.frameAt(150.7) == (int64_t)(1.507 * SR));
    CHECK(tl.snappedFrameAt(-20, 0) == 0);
    t.beats = true;  // 120 BPM: a beat is 50 px at 100 px/s; the grid is in 8ths (25 px)
    CHECK(tl.snappedFrameAt(52, 0) == r.sec(0.5));
    CHECK(tl.snappedFrameAt(52, plat::ModAlt) == tl.frameAt(52));
    CHECK(tl.snappedFrameAt(63, 0) == r.sec(0.75));
    CHECK(tl.snappedFrameAt(52, 0) == (int64_t)std::nearbyint(t.snap(0.52, tl.pps) * SR));
    tl.zoom(4, 0);  // 400 px/s: finer grid
    CHECK_NEAR(tl.pps, 400, 1e-9);
    CHECK(tl.snappedFrameAt(52, 0) == (int64_t)std::nearbyint(t.snap(52 / 400.0, 400) * SR));
    tl.zoom(1e9, 0);
    CHECK_NEAR(tl.pps, config::MAX_PPS, 1e-9);
    tl.zoom(1e-9, 0);
    CHECK_NEAR(tl.pps, config::MIN_PPS, 1e-9);
}

void testHitTesting() {
    Rig r;
    Timeline& tl = *r.tl;
    auto track = r.project.addTrack(Kind::Audio);
    auto clip = audioClip(r.project, track, 1, 2);  // x 100…300, lane y 24…96, box 26…94
    tl.relayout();
    CHECK(tl.hit(PointF(150, 30)).first == clip);
    CHECK(tl.hit(PointF(150, 30)).second == ClipZone::Handle);
    CHECK(tl.hit(PointF(150, 41)).second == ClipZone::Handle);
    CHECK(tl.hit(PointF(150, 42)).second == ClipZone::Body);
    CHECK(tl.hit(PointF(150, 60)).second == ClipZone::Body);
    CHECK(tl.hit(PointF(150, 95)).first == clip);  // (2 px of slack below the box)
    CHECK(!tl.hit(PointF(50, 60)).first);
    CHECK(!tl.hit(PointF(150, 10)).first);  // the ruler
    CHECK(tl.edgeAt(PointF(103, 60)).second == ClipSide::Left);
    CHECK(tl.edgeAt(PointF(297, 60)).second == ClipSide::Right);
    CHECK(!tl.edgeAt(PointF(150, 60)).first);
    CHECK(tl.trackAt(60) == track);
    CHECK(!tl.trackAt(10));
    CHECK(tl.laneEdgeAt(96).first == track);
    CHECK(tl.laneEdgeAt(93).first == track);
    CHECK(!tl.laneEdgeAt(90).first);
    // frozen tracks are locked
    track->frozen = std::make_shared<Clip>();
    CHECK(!tl.hit(PointF(150, 30)).first);
    track->frozen = nullptr;
    // a clip too narrow to have ends
    clip->length = r.sec(0.1);  // 10 px < 3 * CLIP_EDGE_PX
    CHECK(!tl.edgeAt(PointF(101, 60)).first);
    // the later clip is on top
    auto other = audioClip(r.project, track, 1.05, 1);
    CHECK(tl.hit(PointF(106, 30)).first == other);
}

void testMove() {
    Rig r;
    Timeline& tl = *r.tl;
    auto a1 = r.project.addTrack(Kind::Audio);
    auto m1 = r.project.addTrack(Kind::Midi);
    auto a2 = r.project.addTrack(Kind::Audio);  // lanes: 24, 96, 168
    auto clip = audioClip(r.project, a1, 1, 1);
    auto under = audioClip(r.project, a1, 2.5, 1);
    tl.relayout();
    // with Alt: freely, by the mouse's distance
    auto e = mouse(150, 30);
    tl.mousePress(e);
    CHECK(std::holds_alternative<MoveDrag>(tl.drag));
    CHECK(tl.selection.contains(clip));
    e = mouse(163, 30, plat::LeftButton, plat::ModAlt);
    tl.mouseMove(e);
    CHECK(clip->start == r.sec(1) + tl.frameAt(163) - tl.frameAt(150));
    // in bars and beats: the clip grabbed starts on the grid
    e = mouse(163, 30);
    tl.mouseMove(e);
    CHECK(clip->start == (int64_t)std::nearbyint(r.project.timing.snap((r.sec(1) + tl.frameAt(163) - tl.frameAt(150)) / (double)SR, tl.pps) * SR));
    CHECK(clip->start % (SR / 4) == 0);
    // never before 0 s
    e = mouse(-400, 30);
    tl.mouseMove(e);
    CHECK(clip->start == 0);
    // onto the MIDI lane: the kind doesn't match, so it stays on its lane; the next audio lane is fine
    e = mouse(150, 120);
    tl.mouseMove(e);
    CHECK(clip->track == a1);
    e = mouse(150, 190);
    tl.mouseMove(e);
    CHECK(clip->track == a2);
    e = mouse(150, 30);
    tl.mouseMove(e);
    CHECK(clip->track == a1);
    // dropped over the other clip: what it covers is cut away on release
    e = mouse(300, 30, plat::LeftButton, plat::ModAlt);  // +1.5 s: 2.5…3.5 s, right over `under`
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(std::holds_alternative<std::monostate>(tl.drag));
    CHECK(clip->start == r.sec(2.5));
    bool underGone = !r.project.clips.contains(under);
    CHECK(underGone);
    CHECK(r.project.clips.size() == 1);
    (void)m1;
}

void testTrim() {
    Rig r;
    Timeline& tl = *r.tl;
    r.project.timing.beats = false;
    auto track = r.project.addTrack(Kind::Audio);
    auto clip = audioClip(r.project, track, 1, 2);  // x 100…300
    tl.relayout();
    // the right end, outwards: no further than its audio
    auto e = mouse(299, 60);
    tl.mousePress(e);
    CHECK(std::holds_alternative<EdgeDrag>(tl.drag));
    e = mouse(500, 60);
    tl.mouseMove(e);
    CHECK(clip->frames() == r.sec(2));
    // inwards: at least CLIP_MIN_S long
    e = mouse(50, 60);
    tl.mouseMove(e);
    CHECK(clip->frames() == (int64_t)std::nearbyint(config::CLIP_MIN_S * SR));
    e = mouse(200, 60);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(clip->end() == tl.frameAt(200));
    // the left end, inwards and back out: no further than its audio's start
    e = mouse(101, 60);
    tl.mousePress(e);
    e = mouse(150, 60);
    tl.mouseMove(e);
    CHECK(clip->start == tl.frameAt(150));
    CHECK(clip->offset == tl.frameAt(150) - r.sec(1));
    CHECK(clip->end() == tl.frameAt(200));
    e = mouse(20, 60);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(clip->start == r.sec(1));
    CHECK(clip->offset == 0);
    CHECK(clip->end() == tl.frameAt(200));
    // a MIDI clip can be made as long as you like: rendered that long on release
    auto midiTrack = r.project.addTrack(Kind::Midi);
    auto m = midiClip(r.project, midiTrack, 0, 2);  // x 0…200 on the lane at y 96
    tl.relayout();
    int64_t before = m->data->frames();
    int rightX = (int)tl.xOf(m->end()) - 1;  // (its data has the synth's release tail after 2 s)
    e = mouse(rightX, 130);
    tl.mousePress(e);
    CHECK(std::holds_alternative<EdgeDrag>(tl.drag));
    e = mouse(600, 130);
    tl.mouseMove(e);
    CHECK(m->frames() == tl.frameAt(600));
    tl.mouseRelease(e);
    CHECK(m->data->frames() > before);
    CHECK(m->data->frames() >= m->offset + m->frames());
    CHECK_NEAR(m->lengthS, 6.0, 0.01);
}

void testRangeEdits() {
    Rig r;
    Timeline& tl = *r.tl;
    auto track = r.project.addTrack(Kind::Audio);
    auto clip = audioClip(r.project, track, 1, 2);
    tl.relayout();
    // drag across the clip's body: a range (its edges on the grid), the clip selected, the cursor at its start
    auto e = mouse(110, 60);
    tl.mousePress(e);
    CHECK(std::holds_alternative<RangeDrag>(tl.drag));
    e = mouse(112, 60);
    tl.mouseMove(e);
    CHECK(!tl.selection.range);  // (not past DRAG_THRESHOLD_PX yet)
    e = mouse(200, 60);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(tl.selection.range.has_value());
    CHECK(tl.selection.range->start == r.sec(1));
    CHECK(tl.selection.range->end == r.sec(2));
    CHECK(tl.selection.contains(clip));
    CHECK(tl.cursor == r.sec(1));
    // Ctrl+D: the copy goes right after; the range moves on, so again repeats it once more
    tl.duplicateSelected();
    CHECK(tl.selection.range->start == r.sec(2));
    CHECK(tl.selection.range->end == r.sec(3));
    tl.duplicateSelected();
    CHECK(tl.selection.range->start == r.sec(3));
    CHECK(tl.selection.range->end == r.sec(4));
    int64_t total = 0, last = 0;
    for (auto& c : r.project.clips) {
        total += c->frames();
        last = std::max(last, c->end());
    }
    CHECK(last == r.sec(4));
    CHECK(total == r.sec(3));  // 1…2 (orig), 2…3 (copy, replacing), 3…4 (copy); original's 2…3 part replaced
    // cutting the range out
    tl.deleteSelected();
    last = 0;
    for (auto& c : r.project.clips) last = std::max(last, c->end());
    CHECK(last == r.sec(3));
    CHECK(!tl.selection.range);
    // a click on the body without dragging selects the clip
    auto first = tl.hit(PointF(150, 60)).first;
    e = mouse(150, 60);
    tl.mousePress(e);
    tl.mouseRelease(e);
    CHECK(tl.selection.clips.size() == 1 && tl.selection.contains(first));
    // split at the cursor
    tl.setCursor(r.sec(1.5));
    tl.splitSelected();
    CHECK(tl.selection.clips.size() == 2);
    bool cut = false;
    for (auto& c : tl.selection.clips) cut |= c->start == r.sec(1.5) || c->end() == r.sec(1.5);
    CHECK(cut);
    // split a range at both edges: the middle selected
    tl.selection.clear();
    tl.selection.setRange(TimeRange{r.sec(2.2), r.sec(2.6), {track}}, {});
    tl.splitSelected();
    CHECK(tl.selection.clips.size() == 1);
    CHECK((*tl.selection.clips.begin())->start == r.sec(2.2));
    CHECK((*tl.selection.clips.begin())->end() == r.sec(2.6));
    // delete clips (no range): gone
    tl.selection.range.reset();
    size_t n = r.project.clips.size();
    tl.deleteSelected();
    CHECK(r.project.clips.size() == n - 1);
    // keys: Ctrl+A selects all, Esc clears, 0 disables, S splits
    ui::KeyEvent k;
    k.key = 'A';
    k.mods = plat::ModCtrl;
    CHECK(tl.keyPress(k));
    CHECK(tl.selection.clips.size() == r.project.clips.size());
    k.key = '0';
    k.mods = 0;
    tl.keyPress(k);
    bool allOff = true;
    for (auto& c : r.project.clips) allOff &= c->disabled;
    CHECK(allOff);
    tl.keyPress(k);
    for (auto& c : r.project.clips) CHECK(!c->disabled);
    k.key = plat::Key_Escape;
    tl.keyPress(k);
    CHECK(tl.selection.clips.empty());
    k.key = 'Q';
    CHECK(!tl.keyPress(k));
}

void testDrops() {
    Rig r;
    Timeline& tl = *r.tl;
    std::string dir = outDir();
    std::string wav = dir + "/drop.wav";
    auto audio = sine(1.0, 220);
    std::string err;
    CHECK(writeWav(wav, audio->samples.data(), audio->frames(), 2, SR, SampleFormat::Pcm16, err));
    auto midi = r.project.addTrack(Kind::Midi);
    auto audioTrack = r.project.addTrack(Kind::Audio);  // lanes 24 (MIDI), 96 (audio)
    tl.relayout();
    // on the audio lane, on the grid (bars and beats)
    ui::DragData data;
    data.files = {wav};
    ui::DragEvent d;
    d.pos = Point(163, 120);
    d.data = &data;
    tl.dragEnter(d);
    CHECK(d.accepted);
    tl.drop(d);
    CHECK(r.project.clips.size() == 1);
    ClipPtr placed = r.project.clips[0];
    CHECK(placed->track == audioTrack);
    CHECK(placed->start == tl.snappedFrameAt(163, 0));
    CHECK(placed->start % (SR / 4) == 0);
    CHECK(tl.selection.contains(placed));
    // with Alt: freely
    d.pos = Point(613, 120);
    d.mods = plat::ModAlt;
    tl.drop(d);
    CHECK(r.project.clips[1]->start == tl.frameAt(613));
    // on the MIDI lane: a new audio track
    d.pos = Point(50, 40);
    d.mods = 0;
    tl.drop(d);
    CHECK(r.project.tracks.size() == 3);
    CHECK(r.project.clips[2]->track != midi && r.project.clips[2]->track->kind == Kind::Audio);
    // on the master lane (no lane): a new track too
    tl.dropPaths({wav}, 0, std::nullopt);
    CHECK(r.project.tracks.size() == 4);
    // a group's header (folded): unfolded, new tracks in the group
    auto g = tl.groupTracks({audioTrack});
    g->folded = true;
    tl.tracksRegrouped();
    auto lanes = tl.lanes();
    std::optional<Lane> header;
    for (auto& l : lanes)
        if (l.group) header = l;
    CHECK(header.has_value());
    tl.dropPaths({wav}, 0, header);
    CHECK(!g->folded);
    CHECK(r.project.members(g).size() == 2);
    // folders are skipped
    size_t n = r.project.clips.size();
    tl.dropPaths({dir}, 0, std::nullopt);
    CHECK(r.project.clips.size() == n);
    // addFile: onto the first empty track of its kind, or a new one
    tl.addFile(wav, r.sec(3));
    CHECK(r.project.clips->back()->start == r.sec(3));
}

void testMidiAndAutomation() {
    Rig r;
    Timeline& tl = *r.tl;
    auto midi = r.project.addTrack(Kind::Midi);
    tl.relayout();
    ClipPtr edited;
    tl.editNotes.connect([&](ClipPtr c) { edited = c; });
    // double-click empty space on a MIDI lane: a new clip of a bar, to edit
    auto e = mouse(120, 60);
    tl.mouseDoubleClick(e);
    CHECK(edited != nullptr);
    CHECK(edited->kind == Kind::Midi);
    CHECK(edited->start == tl.snappedFrameAt(120, 0));
    CHECK_NEAR(edited->lengthS, 2.0, 1e-9);  // 4 beats at 120 BPM (its data: plus the synth's release)
    // double-click the clip's body: edit it
    edited = nullptr;
    e = mouse(150, 70);
    tl.mouseDoubleClick(e);
    CHECK(edited == r.project.clips[0]);
    // automation lane under the track
    auto lane = std::make_shared<Automation>();
    lane->slot = std::make_shared<PluginSlot>();
    lane->param = "Cutoff";
    midi->automation = std::vector<AutomationPtr>{lane};
    midi->showAutomation = true;
    tl.lanesChanged();
    auto lanes = tl.lanes();
    CHECK(lanes.size() == 3);
    const Lane& al = lanes[1];
    CHECK(al.automation == lane && al.owner == midi);
    CHECK(al.top == 24 + config::TRACK_H);
    int edits = 0;
    tl.automationEdited.connect([&] { ++edits; });
    double yHalf = tl.automationY(al, 0.5);
    e = mouse(200, yHalf);
    tl.mousePress(e);  // one click only selects (a time range): no point
    CHECK(lane->points.size() == 0);
    CHECK(std::holds_alternative<RangeDrag>(tl.drag));
    tl.mouseRelease(e);
    tl.mouseDoubleClick(e);  // a double-click adds one, grabbed
    CHECK(lane->points.size() == 1);
    CHECK_NEAR(lane->points[0].t, 2.0, 1e-9);
    CHECK_NEAR(lane->points[0].v, 0.5, 0.02);
    CHECK(std::holds_alternative<PointDrag>(tl.drag));
    e = mouse(258, tl.automationY(al, 1.0));
    tl.mouseMove(e);
    CHECK_NEAR(lane->points[0].t, 2.5, 1e-9);  // snapped to the grid
    CHECK_NEAR(lane->points[0].v, 1.0, 1e-9);
    tl.mouseRelease(e);
    CHECK(edits >= 2);
    // a second point; the first can't pass it
    e = mouse(400, tl.automationY(al, 0.0));
    tl.mouseDoubleClick(e);
    tl.mouseRelease(e);
    CHECK(lane->points.size() == 2);
    e = mouse(250, tl.automationY(al, 1.0));
    tl.mousePress(e);
    CHECK(std::get<PointDrag>(tl.drag).index == 0);
    e = mouse(700, tl.automationY(al, 1.0));
    tl.mouseMove(e);
    CHECK_NEAR(lane->points[0].t, 4.0, 1e-9);
    tl.mouseRelease(e);
    CHECK(tl.pointAt(al, PointF(400, tl.automationY(al, 0.0))) >= 0);
    // double-click a point: deleted
    e = mouse(400, tl.automationY(al, 0.0));
    tl.mouseDoubleClick(e);
    CHECK(lane->points.size() == 1);
    CHECK(tl.valueText(*lane, 0.5) == "50 %");
    auto labels = tl.rangeLabels(*lane);
    CHECK(labels.first == "max" && labels.second == "min");
    // the lane's bottom edge resizes it, within its range; double-click: the standard height
    e = mouse(100, al.bottom());
    tl.mousePress(e);
    CHECK(std::holds_alternative<HeightDrag>(tl.drag));
    e = mouse(100, al.bottom() + 1000);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(lane->height == config::AUTOMATION_MAX_H);
    e = mouse(100, tl.lanes()[1].bottom());
    tl.mouseDoubleClick(e);
    CHECK(lane->height == config::AUTOMATION_H);
    tl.removeAutomation(midi, lane);
    CHECK(midi->automation.empty() && !midi->showAutomation);
}

void testLoop() {
    Rig r;
    Timeline& tl = *r.tl;
    int changes = 0;
    tl.loopChanged.connect([&] { ++changes; });
    tl.setLoop(r.sec(1), r.sec(2));
    CHECK(changes == 1);
    int x0, x1;
    CHECK(tl.loopXs(x0, x1) && x0 == 100 && x1 == 200);
    CHECK(tl.loopEndAt(PointF(198, 18)) == 1);
    CHECK(tl.loopEndAt(PointF(103, 18)) == 0);
    CHECK(tl.loopEndAt(PointF(150, 18)) == -1);
    CHECK(tl.loopEndAt(PointF(100, 40)) == -1);  // (only on the ruler)
    auto e = mouse(200, 18);
    tl.mousePress(e);
    CHECK(std::holds_alternative<LoopDrag>(tl.drag));
    e = mouse(258, 18);
    tl.mouseMove(e);
    CHECK_NEAR(r.project.timing.loopEnd, 2.5, 1e-9);  // on the grid
    e = mouse(50, 18);
    tl.mouseMove(e);
    CHECK_NEAR(r.project.timing.loopEnd, 1.0 + config::LOOP_MIN_S, 1e-9);  // never shorter than LOOP_MIN_S
    tl.mouseRelease(e);
    tl.setLoop(r.sec(3), r.sec(1));  // (start after end: from the earlier one)
    CHECK_NEAR(r.project.timing.loopStart, 1.0, 1e-9);
    // a click on the ruler moves the cursor (snapped)
    e = mouse(163, 10);
    tl.mousePress(e);
    tl.mouseRelease(e);
    CHECK(tl.cursor == r.sec(1.75));
    CHECK(tl.cursorX == 175);
}

void testTileCache() {
    waveform_cache::TileCache cache(1 << 20);
    Project p;
    auto t = p.addTrack(Kind::Audio);
    auto c = audioClip(p, t, 0, 1);
    const Image& a = cache.tile(*c, 0, 256, 50, 100, SR, Color(200, 220, 255));
    CHECK(a.width() == 256 && a.height() == 50);
    int lit = 0;
    for (int y = 0; y < 50; ++y)
        if (a.pixel(10, y)) ++lit;
    CHECK(lit > 0);
    size_t n = cache.count();
    cache.tile(*c, 0, 256, 50, 100, SR, Color(200, 220, 255));
    CHECK(cache.count() == n);  // reused
    cache.tile(*c, 0, 256, 50, 200, SR, Color(200, 220, 255));
    CHECK(cache.count() == n + 1);  // another zoom: another tile
    for (int i = 0; i < 40; ++i) cache.tile(*c, i, 256, 50, 100 + i, SR, Color(200, 220, 255));
    CHECK(cache.bytes() <= cache.budget);
    auto m = midiClip(p, p.addTrack(Kind::Midi), 0, 2);
    auto mask = waveform_cache::renderMidiTile(*m, 0, 200, 50, 100, SR);
    int on = 0;
    for (auto v : mask) on += v;
    CHECK(on > 0);
    CHECK(clip_painter::pixels(RectF(3.7f, 0.2f, 1, 2)) == Rect(4, 0, 1, 2));
}

void paintSamples() {
    Rig r(1000, 560);
    Timeline& tl = *r.tl;
    Project& p = r.project;
    auto vocals = p.addTrack(Kind::Audio);
    vocals->name = "Vocals";
    auto synth = p.addTrack(Kind::Midi);
    auto drums = p.addTrack(Kind::Audio);
    auto bass = p.addTrack(Kind::Audio);
    auto c1 = audioClip(p, vocals, 0.5, 2.5, "Vocal take");
    auto c2 = audioClip(p, vocals, 3.2, 1.8, "Disabled one");
    c2->disabled = true;
    auto m = midiClip(p, synth, 1, 2);
    auto d1 = audioClip(p, drums, 0, 4, "Drum loop");
    d1->color = "#c05050";
    auto b1 = audioClip(p, bass, 2, 3, "Bass");
    tl.groupTracks({drums, bass});
    auto lane = std::make_shared<Automation>();
    lane->slot = std::make_shared<PluginSlot>();
    lane->param = "Cutoff";
    lane->setPoints({{0.5, 0.2}, {1.5, 0.9}, {3, 0.4}, {4.5, 0.7}});
    synth->automation = std::vector<AutomationPtr>{lane};
    synth->showAutomation = true;
    tl.lanesChanged();
    tl.selection.clips = {c1};
    tl.selection.range = TimeRange{r.sec(1.25), r.sec(2.5), {synth}};
    tl.setLoop(r.sec(1), r.sec(3));
    p.timing.looping = true;
    tl.setCursor(r.sec(1.25));
    r.engine.pos = r.sec(2.1);
    tl.setPlayhead(r.sec(2.1));
    // the master lane pinned to the bottom of the view, over the tracks
    auto master = tl.takeMasterView();
    MasterLaneView* mv = master.get();
    r.scroll.viewport()->adopt(std::move(master));
    mv->setGeometry(0, r.scroll.viewport()->height() - config::MASTER_H, r.scroll.viewport()->width(), config::MASTER_H);
    mv->raise();
    savePng(outDir() + "/timeline_beats.png", paintScroll(r.scroll));
    // strip by strip gives the same pixels as the whole (the playhead strip repainted alone)
    Image whole(tl.width(), tl.height());
    {
        Painter pw(whole);
        tl.paintTree(pw, tl.rect());
    }
    Image strip = whole;
    {
        Painter ps(strip);
        Rect s(tl.playheadX - 7, 0, 15, tl.height());
        ps.setClip(s);
        ps.fillRect(s, Color(255, 0, 255));
        tl.paintTree(ps, s);
    }
    int diff = 0;
    for (int y = 0; y < whole.height(); ++y)
        for (int x = 0; x < whole.width(); ++x)
            if (whole.pixel(x, y) != strip.pixel(x, y)) ++diff;
    CHECK(diff == 0);
    // folded group: miniatures in its header; minutes and seconds
    p.groups()[0]->folded = true;
    p.timing.beats = false;
    tl.tracksRegrouped();
    savePng(outDir() + "/timeline_seconds_folded.png", paintScroll(r.scroll));
    // zoomed in, scrolled, frozen track
    p.groups()[0]->folded = false;
    p.timing.beats = true;
    vocals->frozen = std::make_shared<Clip>();
    tl.tracksRegrouped();
    tl.zoom(3, 0);
    r.scroll.horizontalScrollBar()->setValue(200);
    savePng(outDir() + "/timeline_zoomed.png", paintScroll(r.scroll));
    vocals->frozen = nullptr;
    // the overview
    ui::ScrollArea& s = r.scroll;
    s.horizontalScrollBar()->setValue(0);
    Overview ov(p, r.engine, &tl, &s, 60);
    ov.resize(1000, 60);
    ov.refresh();
    savePng(outDir() + "/overview.png", paintScroll(ov));
    RectF v = ov.viewRect(ov.span());
    CHECK(v.x == 0);
    CHECK(v.w > 0 && v.h > 0);
    CHECK_NEAR(ov.span(), config::OVERVIEW_MIN_S, 1e-6);
    // clicking it moves the view there (centred)
    auto e = mouse(500, 30);
    ov.mousePress(e);
    ov.mouseRelease(e);
    CHECK(s.horizontalScrollBar()->value() > 0);
    int heights = 0;
    ov.resizedTo.connect([&](int h) { heights = h; });
    e = mouse(10, 58);
    e.globalPos = Point(10, 58);
    ov.mousePress(e);
    e.globalPos = Point(10, 108);
    ov.mouseMove(e);
    CHECK(heights == 110 && ov.height() == 110);
    ov.mouseRelease(e);
    e = mouse(10, 108);
    ov.mouseDoubleClick(e);
    CHECK(heights == config::OVERVIEW_H);
    (void)m;
    (void)b1;
}

// ============================================================================================
// the successor's features (Exbit-Linux)
// ============================================================================================

void testClipboard() {
    Rig r;
    Timeline& tl = *r.tl;
    Project& p = r.project;
    auto a1 = p.addTrack(Kind::Audio);
    auto a2 = p.addTrack(Kind::Audio);
    auto m1 = p.addTrack(Kind::Midi);
    a1->name = "Audio 1";
    m1->name = "MIDI 1";
    auto c1 = audioClip(p, a1, 1, 1, "One");
    auto c2 = audioClip(p, a2, 1.5, 1, "Two");
    tl.relayout();
    std::string reason;
    clipboard::held.reset();
    CHECK(clipboard::paste(tl, reason).empty() && reason.find("Nothing copied yet") == 0);
    CHECK(clipboard::use(tl, "copy") == "Select clips (or a time range over them) first");
    tl.selection.clips = {c1, c2};
    CHECK(clipboard::copy(tl) == 2);
    CHECK(clipboard::held->span == r.sec(1.5) && clipboard::held->sr == SR);
    // paste at the cursor: the topmost on the selected track, the other on the lane below
    tl.setCursor(r.sec(5));
    tl.setCurrentTrack(a1);
    auto placed = clipboard::paste(tl, reason);
    CHECK(reason.empty() && placed.size() == 2);
    ClipPtr on1, on2;
    for (auto& c : placed) (c->track == a1 ? on1 : on2) = c;
    CHECK(on1 && on2 && on1->start == r.sec(5) && on2->track == a2 && on2->start == r.sec(5.5));
    CHECK(on1 != c1 && on1->data == c1->data);  // (a copy sharing the audio)
    CHECK(tl.cursor == r.sec(6.5));  // (Ctrl+V again pastes right after)
    CHECK(tl.selection.clips.size() == 2 && tl.selection.contains(on1));
    CHECK(p.clips.size() == 4);
    // the lanes don't fit: nothing pasted
    tl.setCurrentTrack(a2);
    CHECK(clipboard::paste(tl, reason).empty() && reason == "MIDI 1 can't take audio clips: select another track to paste onto");
    tl.setCurrentTrack(m1);
    CHECK(clipboard::paste(tl, reason).empty() && reason == "MIDI 1 can't take audio clips: select another track to paste onto");
    clipboard::held->clips.pop_back();  // (only the top one)
    tl.setCurrentTrack(a2);
    CHECK(clipboard::paste(tl, reason).size() == 1 && reason.empty());
    tl.setCurrentTrack(m1);
    clipboard::held->clips[0].lane = 1;
    CHECK(clipboard::paste(tl, reason).empty() && reason == "There aren't enough tracks below the selected one for what was copied");
    // a time range: the parts of the clips in it
    tl.selection.clear();
    tl.selection.range = TimeRange{r.sec(0.5), r.sec(1.5), {a1}};
    CHECK(clipboard::copy(tl) == 1);
    CHECK(clipboard::held->clips[0].offset == r.sec(0.5) && clipboard::held->clips[0].clip->frames() == r.sec(0.5));
    CHECK(clipboard::held->span == r.sec(1));
    // another sample rate: copy again
    clipboard::held->sr = 44100;
    CHECK(clipboard::paste(tl, reason).empty() && reason == "What was copied was at another sample rate: copy it again");
    // cut: copied, then removed
    tl.selection.clear();
    tl.selection.clips = {c2};
    size_t before = p.clips.size();
    CHECK(clipboard::use(tl, "cut") == "Cut 1 clip");
    CHECK(p.clips.size() == before - 1 && !p.clips.contains(c2));
    tl.setCurrentTrack(a1);
    tl.setCursor(r.sec(10));
    CHECK(clipboard::use(tl, "paste") == "Pasted 1 clip");
    CHECK(tl.selection.clips.size() == 1 && (*tl.selection.clips.begin())->track == a1);
    // the right-click menu and the keys ask the main window (clipboardChosen)
    std::vector<std::string> asked;
    tl.clipboardChosen.connect([&](std::string w) { asked.push_back(w); });
    ui::Menu menu;
    tl.fillMenu(menu, Point(50, 120));  // (over a2's lane: paste from there)
    ui::Menu::activate(actionStarting(menu, "Paste at the cursor"));
    CHECK(asked.size() == 1 && asked[0] == "paste" && tl.currentTrack == a2);
    ui::KeyEvent k;
    k.key = 'C';
    k.mods = plat::ModCtrl;
    tl.keyPress(k);
    CHECK(asked.size() == 2 && asked[1] == "copy");
}

void testCopyDrag() {
    Rig r;
    Timeline& tl = *r.tl;
    auto a1 = r.project.addTrack(Kind::Audio);
    auto clip = audioClip(r.project, a1, 1, 1);  // x 100…200
    tl.relayout();
    // a press that barely moves is still a click: nothing moves
    auto e = mouse(150, 30);
    tl.mousePress(e);
    e = mouse(151, 31);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(clip->start == r.sec(1));
    // Ctrl+click without dragging: in or out of the selection
    e = mouse(150, 30, plat::LeftButton, plat::ModCtrl);
    tl.mousePress(e);
    CHECK(std::holds_alternative<CopyDrag>(tl.drag));
    tl.mouseRelease(e);
    CHECK(!tl.selection.contains(clip));
    tl.mousePress(e);
    tl.mouseRelease(e);
    CHECK(tl.selection.contains(clip));
    // Ctrl+drag: a copy moves, the clip stays
    tl.mousePress(e);
    e = mouse(300, 30, plat::LeftButton, plat::ModCtrl);
    tl.mouseMove(e);
    CHECK(std::holds_alternative<MoveDrag>(tl.drag));
    tl.mouseRelease(e);
    CHECK(r.project.clips.size() == 2 && r.project.clips.contains(clip) && clip->start == r.sec(1));
    ClipPtr copy = r.project.clips[1];
    CHECK(copy != clip && copy->start == r.sec(2.5) && copy->track == a1 && copy->data == clip->data);
    CHECK(tl.selection.clips.size() == 1 && tl.selection.contains(copy));
}

void testFadesAndGain() {
    Rig r;
    Timeline& tl = *r.tl;
    auto a1 = r.project.addTrack(Kind::Audio);
    auto clip = audioClip(r.project, a1, 1, 2);  // x 100…300, body y 42…94
    tl.relayout();
    FadeHit h = tl.fadeHandleAt(PointF(110, 46));
    CHECK(h.clip == clip && h.in);
    h = tl.fadeHandleAt(PointF(290, 46));
    CHECK(h.clip == clip && !h.in);
    CHECK(!tl.fadeHandleAt(PointF(150, 46)).clip && !tl.fadeHandleAt(PointF(110, 70)).clip);
    // drag the fade-in handle inwards: a fade from the clip's start to the mouse
    auto e = mouse(110, 46);
    tl.mousePress(e);
    CHECK(std::holds_alternative<FadeDrag>(tl.drag));
    e = mouse(150, 46);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(clip->fadeIn == r.sec(0.5) && clip->start == r.sec(1));
    // the fade-out: no further than the fade-in leaves
    e = mouse(290, 46);
    tl.mousePress(e);
    e = mouse(20, 46);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(clip->fadeOut == r.sec(1.5));
    CHECK(tl.fadeHandleAt(PointF(150 + 3, 46)).clip == clip);  // (the handle sits where the fade ends)
    // double-click a handle: no fade on that side
    RectF in, out;
    RectF body = tl.clipRect(*clip, tl.trackTops())->adjusted(0, config::CLIP_HANDLE_H, 0, 0);
    CHECK(clip_painter::fadeHandles(*clip, body, tl.pps, SR, in, out));
    e = mouse(out.x + 3, out.y + 3);
    tl.mouseDoubleClick(e);
    CHECK(clip->fadeOut == 0 && clip->fadeIn == r.sec(0.5));
    clip->fadeOut = r.sec(0.4);
    // gain
    tl.selection.selectOnly(clip);
    answerWith({{"Clip gain", "-3.04"}});
    tl.setGainSelected();
    CHECK_NEAR(clip->gainDb, -3.0, 1e-9);
    CHECK(clip_painter::clipLabel(*clip) == "Sine   -3.0 dB");
    // the waveform follows the gain and fades: other tiles
    waveform_cache::Mask quiet = waveform_cache::renderAudioTile(*clip, 0, 50, 52, 100, SR);
    clip->gainDb = 0;
    clip->fadeIn = clip->fadeOut = 0;
    waveform_cache::Mask loud = waveform_cache::renderAudioTile(*clip, 0, 50, 52, 100, SR);
    size_t q = 0, l = 0;
    for (auto v : quiet) q += v;
    for (auto v : loud) l += v;
    CHECK(q < l);
    clip->gainDb = -3;
    clip->fadeIn = r.sec(0.5);
    clip->fadeOut = r.sec(0.4);
    // the menu
    ui::Menu menu;
    tl.fillMenu(menu, Point(150, 60));
    CHECK(actionStarting(menu, "Remove fades")->enabled && actionStarting(menu, "Clip gain…")->enabled);
    CHECK(actionStarting(menu, "Crossfade…") && !actionStarting(menu, "Crossfade…")->enabled);
    CHECK(actionStarting(menu, "Export as MIDI file…") && !actionStarting(menu, "Export as MIDI file…")->enabled);
    CHECK(actionStarting(menu, "Reverse (play backwards)") && actionStarting(menu, "Join clips"));
    CHECK(actionStarting(menu, "Warp…") && actionStarting(menu, "Loop (it repeats"));
    tl.update();
    savePng(outDir2() + "/fades_gain.png", paintScroll(r.scroll));
    {  // at a 1.5x interface size: the tiles in device pixels (sharp)
        ui::App::get().doLayouts();
        Image big((int)(r.scroll.width() * 1.5), (int)(r.scroll.height() * 1.5), theme::c("window"));
        Painter ps(big, 1.5f);
        r.scroll.paintTree(ps, r.scroll.rect());
        savePng(outDir2() + "/fades_gain_150.png", big);
        int ink = 0;  // (the waveform is drawn: light pixels inside the clip's body)
        for (int y = (int)(45 * 1.5); y < (int)(92 * 1.5); ++y)
            for (int x = (int)(110 * 1.5); x < (int)(290 * 1.5); ++x)
                { Color c = big.colorAt(x, y); if (c.r > 150 && c.g > 150 && c.b > 150) ++ink; }
        CHECK(ink > 100);
    }
    // fade shapes (model.h FADE SHAPES): two submenus in the clip menu, ticked as the selected clips have them
    ui::Action* inShapes = actionStarting(menu, "Fade-in shape");
    ui::Action* outShapes = actionStarting(menu, "Fade-out shape");
    CHECK(inShapes && outShapes && inShapes->submenu && outShapes->submenu && inShapes->enabled);
    if (inShapes && outShapes && inShapes->submenu && outShapes->submenu) {
        CHECK(inShapes->submenu->actions().size() == 5 && actionStarting(*inShapes->submenu, "Linear")->checked);
        ui::Menu::activate(actionStarting(*inShapes->submenu, "Smooth"));
        ui::Menu::activate(actionStarting(*outShapes->submenu, "Equal power"));
        CHECK(clip->fadeInShape == FadeShape::Smooth && clip->fadeOutShape == FadeShape::EqualPower);
    }
    {  // …ticked now; and a MIDI clip alone has nothing to shape
        ui::Menu again;
        tl.fillMenu(again, Point(150, 60));
        CHECK(actionStarting(*actionStarting(again, "Fade-in shape")->submenu, "Smooth")->checked);
        CHECK(!actionStarting(*actionStarting(again, "Fade-in shape")->submenu, "Linear")->checked);
    }
    // right-clicking a fade handle: that fade's shapes alone
    {
        RectF hin, hout;
        CHECK(clip_painter::fadeHandles(*clip, body, tl.pps, SR, hin, hout));
        ui::Menu handleMenu;
        CHECK(tl.fillFadeMenu(handleMenu, PointF(hout.x + 3, hout.y + 3)));
        CHECK(handleMenu.actions().size() == 6 && handleMenu.actions()[0]->text == "Fade-out shape" &&
              actionStarting(handleMenu, "Equal power")->checked);
        ui::Menu::activate(actionStarting(handleMenu, "Fast"));
        CHECK(clip->fadeOutShape == FadeShape::Fast && clip->fadeInShape == FadeShape::Smooth);
        ui::Menu none;
        CHECK(!tl.fillFadeMenu(none, PointF(200, 70)) && none.actions().empty());
    }
    // drawn as curves: a fast fade-out is shaded where it is quiet (the bottom-right of the fade), a slow
    // one isn't (it stays loud there): the shading (darker) differs at a point under the straight line
    {
        auto shadeAt = [&](FadeShape shape) {
            clip->fadeOutShape = shape;
            tl.update();
            Image pic = paintScroll(r.scroll);
            RectF b2 = tl.clipRect(*clip, tl.trackTops())->adjusted(0, config::CLIP_HANDLE_H, 0, 0);
            double fadeW = (double)clip->fadeOut / SR * tl.pps;
            int x = (int)(b2.right() - fadeW * 0.3), y = (int)(b2.bottom() - b2.h * 0.5);  // (above a fast curve)
            Color c = pic.colorAt(x, y);
            return c.r + c.g + c.b;
        };
        int fast = shadeAt(FadeShape::Fast), slow = shadeAt(FadeShape::Slow);
        CHECK(fast != slow);
        // the waveform's cached tiles follow the shapes too (a tile of the fade-in, for each shape)
        auto tileOf = [&](FadeShape shape) {
            clip->fadeInShape = shape;
            return Image(waveform_cache::tiles().tile(*clip, 0, 50, 52, tl.pps, SR, Color(200, 200, 200)));
        };
        Image smoothTile = tileOf(FadeShape::Smooth), slowTile = tileOf(FadeShape::Slow);
        int differ = 0;
        for (int y = 0; y < smoothTile.height(); ++y)
            for (int x = 0; x < smoothTile.width(); ++x)
                if (smoothTile.colorAt(x, y) != slowTile.colorAt(x, y)) ++differ;
        CHECK(differ > 0);
        clip->fadeInShape = FadeShape::Smooth;
        savePng(outDir2() + "/fade_shapes.png", paintScroll(r.scroll));
    }
    ui::Menu::activate(actionStarting(menu, "Remove fades"));
    CHECK(clip->fadeIn == 0 && clip->fadeOut == 0);
    track_dialogs::answer = nullptr;
}

void testClipEdits() {
    Rig r;
    Timeline& tl = *r.tl;
    auto a1 = r.project.addTrack(Kind::Audio);
    auto clip = audioClip(r.project, a1, 1, 2);
    ClipPtr left, right;
    CHECK(edits::splitClip(clip, r.sec(2), left, right));
    r.project.clips = std::vector<ClipPtr>{left, right};
    tl.relayout();
    // crossfade: they overlap, one fading into the other
    tl.selection.clips = {left, right};
    answerWith({{"Crossfade", "40"}});
    tl.crossfadeSelected();
    CHECK(r.project.clips.size() == 2 && !r.project.clips.contains(left));
    ClipPtr l = r.project.clips[0], rt = r.project.clips[1];
    if (l->start > rt->start) std::swap(l, rt);
    CHECK(l->end() > rt->start && l->fadeOut > 0 && rt->fadeIn > 0);
    CHECK(tl.selection.clips.size() == 2);
    // join (Ctrl+J): one clip of them
    ui::KeyEvent j;
    j.key = 'J';
    j.mods = plat::ModCtrl;
    tl.keyPress(j);
    CHECK(r.project.clips.size() == 1 && r.project.clips[0]->name.find("(joined)") != std::string::npos);
    CHECK(r.project.clips[0]->start == r.sec(1) && tl.selection.contains(r.project.clips[0]));
    // reverse
    ClipPtr joined = r.project.clips[0];
    tl.reverseSelected();
    CHECK(r.project.clips[0] != joined && r.project.clips[0]->reverse && tl.selection.contains(r.project.clips[0]));
    ui::Menu menu;
    tl.fillMenu(menu, Point(150, 60));
    CHECK(actionStarting(menu, "Play forwards again"));
    // Warp… not warped: stretch and pitch (the dialog answered)
    ClipPtr before = r.project.clips[0];
    WarpDialog::testRun = [](WarpDialog& d) {
        CHECK(!d.length->isEnabled() == d.warpBox->isChecked());
        d.length->setValue(150);
        d.semitones->setValue(2);
        d.bpm->setValue(100);
        return true;
    };
    tl.warpSelected();
    WarpDialog::testRun = nullptr;
    ClipPtr stretched = r.project.clips[0];
    CHECK(stretched != before && stretched->stretch == 1.5 && stretched->pitch == 2 && stretched->sourceBpm == 100);
    CHECK(!stretched->warp);
    CHECK(std::llabs(stretched->frames() - (int64_t)(before->frames() * 1.5)) < SR / 50);
    // the dialog's Match: the length that plays the recorded tempo at the project's
    WarpDialog d(nullptr, *stretched, std::make_shared<const TempoMap>(120.0), 1, SR, 0, 0.25);
    d.bpm->setValue(90);
    d.match();
    CHECK_NEAR(d.length->value(), 75, 1e-9);
    track_dialogs::answer = nullptr;
}

void testLoopingClips() {
    Rig r;
    Timeline& tl = *r.tl;
    auto a1 = r.project.addTrack(Kind::Audio);
    auto clip = audioClip(r.project, a1, 1, 1);  // x 100…200
    tl.relayout();
    int64_t audio = clip->data->frames();
    // not looping: its end stops at its audio's end
    auto e = mouse(198, 60);
    tl.mousePress(e);
    CHECK(std::holds_alternative<EdgeDrag>(tl.drag));
    e = mouse(400, 60);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(clip->frames() == audio);
    // looping: dragging the end repeats it
    tl.setLoopNotes({clip}, true);
    CHECK(clip->loop && clip->loopLength == 1.0 && clip->loopStart == 0.0);
    e = mouse(198, 60);
    tl.mousePress(e);
    e = mouse(400, 60);
    tl.mouseMove(e);
    CHECK(clip->frames() == r.sec(3) && clip->data->frames() >= clip->offset + clip->frames());
    tl.mouseRelease(e);
    int64_t spare = clip->data->frames() - clip->offset - clip->frames();
    CHECK(spare >= 0 && spare <= timeline::LOOP_SLACK_S * SR);
    tl.selection.clear();
    savePng(outDir2() + "/loop_notches.png", paintScroll(r.scroll));
    // the menu shows it on; off again: no longer than its audio
    tl.selection.selectOnly(clip);
    ui::Menu menu;
    tl.fillMenu(menu, Point(150, 60));
    CHECK(actionStarting(menu, "Loop (it repeats")->checked);
    ui::Menu::activate(actionStarting(menu, "Loop (it repeats"));
    CHECK(!clip->loop && clip->frames() <= clip->data->frames() - clip->offset);
    // a MIDI clip's notes repeat
    auto m1 = r.project.addTrack(Kind::Midi);
    auto mc = midiClip(r.project, m1, 0, 2);
    mc->prepare(SR);
    tl.setLoopNotes({mc}, true);
    mc->length = r.sec(6);
    tl.prepareLive(mc);
    CHECK(mc->played && mc->played->size() > mc->notes->size() * 2);
}

void testMarkers() {
    Rig r;
    Timeline& tl = *r.tl;
    auto a1 = r.project.addTrack(Kind::Audio);
    audioClip(r.project, a1, 0.5, 4);
    r.project.setMarkers({Marker{2.0, "Chorus"}, Marker{2.1, "Close"}});
    tl.relayout();
    auto flags = tl.markerFlags();
    CHECK(flags.size() == 2 && flags[0].second.x == 200 && flags[0].second.y == config::RULER_H / 2);
    CHECK(flags[0].second.w == 9);  // (cut where the next one starts)
    CHECK(tl.markerAt(PointF(205, 18))->name == "Chorus");
    CHECK(tl.markerAt(PointF(212, 18))->name == "Close");
    CHECK(!tl.markerAt(PointF(205, 5)) && !tl.markerAt(PointF(205, 40)));
    r.project.setMarkers({Marker{2.0, "Chorus"}});
    // click: the cursor goes there; drag: it moves (snapped)
    auto e = mouse(205, 18);
    tl.mousePress(e);
    CHECK(tl.cursor == r.sec(2) && std::holds_alternative<MarkerDrag>(tl.drag));
    e = mouse(262, 18);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(r.project.markers.size() == 1 && r.project.markers[0].seconds == 2.5);
    // double-click: rename
    answerWith({{"Rename the marker", " Verse "}, {"Add a marker", "Bridge"}});
    e = mouse(255, 18);
    tl.mouseDoubleClick(e);
    CHECK(r.project.markers[0].name == "Verse");
    // the ruler's menu
    ui::Menu menu;
    tl.fillRulerMenu(menu, PointF(255, 18));
    CHECK(actionStarting(menu, "Rename the marker “Verse”…") && actionStarting(menu, "Delete the marker “Verse”"));
    CHECK(actionStarting(menu, "Delete every marker")->enabled);
    ui::Menu::activate(actionStarting(menu, "Add a marker here"));  // (snapped: 2.5 s, it sorts after Verse… at once)
    CHECK(r.project.markers.size() == 2);
    tl.setCursor(r.sec(1));
    ui::Menu menu2;
    tl.fillRulerMenu(menu2, PointF(50, 18));
    CHECK(!actionStarting(menu2, "Rename the marker"));
    ui::Menu::activate(actionStarting(menu2, "Add a marker at the selection cursor"));
    CHECK(r.project.markers.size() == 3 && r.project.markers[0].seconds == 1.0 && r.project.markers[0].name == "Bridge");
    tl.update();
    savePng(outDir2() + "/markers.png", paintScroll(r.scroll));
    ui::Menu menu3;
    tl.fillRulerMenu(menu3, PointF(255, 18));
    ui::Menu::activate(actionStarting(menu3, "Delete the marker"));
    CHECK(r.project.markers.size() == 2);
    ui::Menu menu4;
    tl.fillRulerMenu(menu4, PointF(255, 18));
    ui::Menu::activate(actionStarting(menu4, "Delete every marker"));
    CHECK(r.project.markers.empty());
    track_dialogs::answer = nullptr;
}

void testTempoChanges() {
    Rig r(1200, 300);
    Timeline& tl = *r.tl;
    Timing& t = r.project.timing;
    auto a1 = r.project.addTrack(Kind::Audio);
    audioClip(r.project, a1, 0.5, 9);
    tl.relayout();
    t.setChange(3, 140, 3);
    int timings = 0;
    tl.timingChanged.connect([&] { ++timings; });
    ui::Menu menu;
    tl.fillRulerMenu(menu, PointF(410, 18));  // bar 3 starts at 4 s
    CHECK(actionStarting(menu, "Tempo and time signature from bar 3…  (140 BPM 3/4)") != nullptr);
    ui::Menu other;
    tl.fillRulerMenu(other, PointF(150, 18));
    CHECK(actionStarting(other, "Tempo and time signature from bar 1…")->text == "Tempo and time signature from bar 1…");
    tl.update();
    savePng(outDir2() + "/tempo_change.png", paintScroll(r.scroll));
    // the dialog (answered): another tempo from bar 3; then the change removed; bar 1: the song's own
    TempoDialog::testRun = [](TempoDialog& d) {
        CHECK(d.bar == 3 && d.removeButton != nullptr && d.tempo->value() == 140 && d.signature->intValue() == 3);
        d.tempo->setValue(150);
        return true;
    };
    ui::Menu::activate(actionStarting(menu, "Tempo and time signature from bar 3"));
    CHECK(t.changeAtBar(3) && t.changeAtBar(3)->bpm == 150 && t.changeAtBar(3)->beatsPerBar == 3 && timings == 1);
    TempoDialog::testRun = [](TempoDialog& d) {
        d.remove();
        return true;
    };
    tl.editTempo(3);
    CHECK(!t.changeAtBar(3) && timings == 2);
    TempoDialog::testRun = [](TempoDialog& d) {
        CHECK(d.removeButton == nullptr);
        d.tempo->setValue(90);
        d.signature->setValue(6);
        return true;
    };
    tl.editTempo(1);
    CHECK(t.bpm == 90 && t.beatsPerBar == 6);
    TempoDialog::testRun = nullptr;
    // not in bars and beats: no tempo changes
    t.beats = false;
    ui::Menu off;
    tl.fillRulerMenu(off, PointF(410, 18));
    CHECK(!actionStarting(off, "Tempo and time signature")->enabled);
}

void testGridAndSnapping() {
    Rig r;
    Timeline& tl = *r.tl;
    Timing& t = r.project.timing;  // 120 BPM: a beat is 50 px at 100 px/s; the zoom's grid is in 8ths
    CHECK(tl.snappedFrameAt(70, 0) == r.sec(0.75));
    t.grid = 1.0;  // a fixed grid of beats, whatever the zoom
    CHECK(tl.snappedFrameAt(70, 0) == r.sec(0.5));
    tl.zoom(4, 0);
    CHECK(tl.snappedFrameAt(280, 0) == r.sec(0.5));
    tl.zoom(0.25, 0);
    // snapping off: freely; Alt does the opposite
    t.snapOn = false;
    CHECK(tl.snappedFrameAt(70, 0) == tl.frameAt(70));
    CHECK(tl.snappedFrameAt(70, plat::ModAlt) == r.sec(0.5));
    t.snapOn = true;
    CHECK(tl.snappedFrameAt(70, plat::ModAlt) == tl.frameAt(70));
    // moving a clip follows it too
    auto a1 = r.project.addTrack(Kind::Audio);
    auto clip = audioClip(r.project, a1, 1, 1);
    tl.relayout();
    auto e = mouse(150, 30);
    tl.mousePress(e);
    e = mouse(170, 30);
    tl.mouseMove(e);
    CHECK(clip->start == r.sec(1));  // (20 px: nearer the beat it was on)
    e = mouse(180, 30);
    tl.mouseMove(e);
    CHECK(clip->start == r.sec(1.5));
    t.snapOn = false;
    e = mouse(180, 30);
    tl.mouseMove(e);
    CHECK(clip->start == r.sec(1) + tl.frameAt(180) - tl.frameAt(150));
    tl.mouseRelease(e);
    t.snapOn = true;
    t.grid = 0;
    // the zoom keeps a long song within the widest a widget can be
    auto far = audioClip(r.project, a1, 20000, 1);
    tl.relayout();
    tl.zoom(1e9, 0);
    CHECK(tl.pps < config::MAX_PPS && tl.pps * (tl.endSeconds() + config::EMPTY_SECONDS_AFTER_END) <= timeline::WIDEST_PX + 1);
    (void)far;
}

void testTakes() {
    Rig r;
    Timeline& tl = *r.tl;
    auto a1 = r.project.addTrack(Kind::Audio);
    auto take = makeAudioClip("Take 1", sine(3, 5), SR, SR);
    take->start = r.sec(1);
    auto take2 = makeAudioClip("Take 2", sine(3, 2), SR, SR);
    take2->start = r.sec(1);
    a1->takes = std::vector<ClipPtr>{take, take2};
    a1->showTakes = true;
    tl.lanesChanged();
    auto ls = tl.lanes();
    CHECK(ls.size() == 4 && ls[1].take == take && ls[1].owner == a1 && ls[1].height == config::TAKE_H && ls[2].take == take2);
    CHECK(tl.laneAt(110)->take == take && !tl.trackAt(110));
    // drag across a part of a take: that part goes on the track
    auto e = mouse(150, 110);
    tl.mousePress(e);
    CHECK(std::holds_alternative<TakeDrag>(tl.drag) && tl.currentTrack == a1);
    e = mouse(250, 110);
    tl.mouseMove(e);
    tl.update();
    savePng(outDir2() + "/takes_comping.png", paintScroll(r.scroll));
    tl.mouseRelease(e);
    CHECK(r.project.clips.size() == 1);
    ClipPtr piece = r.project.clips[0];
    CHECK(piece->track == a1 && piece->start == r.sec(1.5) && piece->frames() == r.sec(1) && tl.selection.contains(piece));
    // double-click the other take: all of it, replacing what is there
    e = mouse(150, 110 + config::TAKE_H);
    tl.mouseDoubleClick(e);
    CHECK(r.project.clips.size() == 1 && r.project.clips[0]->start == r.sec(1) && r.project.clips[0]->frames() == r.sec(3));
    // the take lane's menu
    ui::Menu menu;
    tl.fillTakeMenu(menu, ls[1]);
    CHECK(menu.actions().size() == 3 && menu.actions()[0]->text == "Use all of Take 1" &&
          menu.actions()[1]->text == "Delete Take 1 (from the takes)" && menu.actions()[2]->text == "Hide the takes");
    tl.update();
    savePng(outDir2() + "/takes.png", paintScroll(r.scroll));
    // a frozen track's takes are locked; deleting the last hides the lanes
    a1->frozen = std::make_shared<Clip>();
    tl.deleteTake(a1, take);
    CHECK(a1->takes.size() == 2);
    a1->frozen = nullptr;
    tl.deleteTake(a1, take);
    tl.deleteTake(a1, take2);
    CHECK(a1->takes.empty() && !a1->showTakes && tl.lanes().size() == 2);
    // a take only goes onto a track of its kind
    auto m1 = r.project.addTrack(Kind::Midi);
    tl.comp(m1, take, take->start, take->end());
    CHECK(r.project.clipsOn(m1).empty());
}

void testTimeEdits() {
    Rig r;
    Timeline& tl = *r.tl;
    auto a1 = r.project.addTrack(Kind::Audio);
    auto late = audioClip(r.project, a1, 4, 1);
    tl.relayout();
    std::vector<std::string> said;
    tl.timeEdited.connect([&](std::string m) { said.push_back(m); });
    CHECK(tl.deleteTime() == "Select a time range first (drag across a lane): its stretch is taken out of the whole song");
    tl.selection.range = TimeRange{r.sec(1), r.sec(2), {a1}};
    CHECK(tl.deleteTime().find("Deleted 1.00 s from the song") == 0);
    CHECK(late->start == r.sec(3) && !tl.selection.range);
    // without a range: bars (in bars and beats) at the selection cursor
    tl.setCursor(r.sec(1));
    answerWith({{"Insert time", "2"}});
    CHECK(tl.insertTime().find("Inserted 4.00 s of empty time") == 0);
    CHECK(late->start == r.sec(7));
    // in seconds
    r.project.timing.beats = false;
    answerWith({{"Insert time", "1.5"}});
    CHECK(tl.insertTime().find("Inserted 1.50 s of empty time") == 0);
    CHECK(late->start == r.sec(8.5));
    // cancelled: nothing
    track_dialogs::answer = [](const std::string&, std::string&) { return false; };
    CHECK(tl.insertTime().empty());
    // the ruler's menu says it in the status bar (timeEdited)
    tl.selection.range = TimeRange{r.sec(0), r.sec(0.5), {a1}};
    ui::Menu menu;
    tl.fillRulerMenu(menu, PointF(50, 18));
    CHECK(actionStarting(menu, "Insert time…  (as long as the range)") && actionStarting(menu, "Delete the selected time")->enabled);
    ui::Menu::activate(actionStarting(menu, "Delete the selected time"));
    CHECK(said.size() == 1 && said[0].find("Deleted 0.50 s") == 0 && late->start == r.sec(8));
    track_dialogs::answer = nullptr;
}

void testMidiClipsAndExport() {
    Rig r;
    Timeline& tl = *r.tl;
    auto m1 = r.project.addTrack(Kind::Midi);
    auto mc = midiClip(r.project, m1, 1, 2);
    tl.relayout();
    // a new MIDI clip is exactly a bar long (at the tempo where it is)
    r.project.timing.setChange(2, 60, 3);
    ClipPtr made = tl.newMidiClip(m1, r.sec(10));
    CHECK(made->length == r.sec(3) && made->lengthS == 3.0);
    // Export as MIDI file…
    std::vector<ClipPtr> exported;
    tl.exportMidi.connect([&](std::vector<ClipPtr> cs) { exported = cs; });
    tl.selection.selectOnly(mc);
    ui::Menu menu;
    tl.fillMenu(menu, Point(150, 60));
    CHECK(actionStarting(menu, "Export as MIDI file…")->enabled);
    ui::Menu::activate(actionStarting(menu, "Export as MIDI file…"));
    CHECK(exported.size() == 1 && exported[0] == mc);
    // Select all: only what can be seen and edited
    auto a1 = r.project.addTrack(Kind::Audio);
    auto a2 = r.project.addTrack(Kind::Audio);
    audioClip(r.project, a1, 0, 1);
    auto hidden = audioClip(r.project, a2, 0, 1);
    tl.groupTracks({a2});
    r.project.groups()[0]->folded = true;
    tl.tracksRegrouped();
    tl.selectAll();
    CHECK(tl.selection.clips.size() == 3 && !tl.selection.contains(hidden));
    // a MIDI file dropped: its beats on the song's beats where it lands (here at 60 BPM), whole bars long
    {
        Timing written;  // 120 BPM: a note on beat 2, half a beat long; the file 5 beats long
        std::string path = outDir2() + "/beats.mid", error;
        CHECK(file_io::saveMidi(path, {{"Part", {{0.5, 0.75, 64, 100}, {2.0, 2.5, 67, 90}}}}, written, 0, error));
        Rig r2;
        r2.project.timing.bpm = 60;
        auto m2 = r2.project.addTrack(Kind::Midi);
        ClipPtr loaded = r2.tl->loadClip(path);
        CHECK(loaded && loaded->kind == Kind::Midi);
        if (loaded) {
            r2.tl->placeClip(loaded, r2.sec(4), m2);  // (bar 2 at 60 BPM)
            CHECK(loaded->notes->size() == 2);
            CHECK_NEAR((*loaded->notes)[0].start, 1.0, 1e-6);   // beat 1 of the file: 1 s at 60 BPM
            CHECK_NEAR((*loaded->notes)[1].end, 5.0, 1e-6);
            CHECK_NEAR(loaded->lengthS, 8.0, 1e-6);              // 5 beats: 2 bars of 4
            CHECK(loaded->length == r2.sec(8));
        }
    }
    // its time signatures: an empty project in 4/4 (a new one) takes them; one with its own, or with clips, keeps its own
    {
        Timing written;
        written.setSignature(1, {3, 4});
        written.setSignature(3, {7, 8});
        std::string path = outDir2() + "/sigs.mid", error;
        CHECK(file_io::saveMidi(path, {{"Part", {{0.0, 1.0, 60, 100}}}}, written, 0, error));
        Rig fresh;
        ClipPtr c = fresh.tl->loadClip(path);
        CHECK(c);
        if (c) fresh.tl->placeClip(c, 0, fresh.project.addTrack(Kind::Midi));
        const Timing& ft = fresh.project.timing;
        CHECK(ft.sigAtBar(1) == TimeSig({3, 4}) && ft.sigAtBar(2) == TimeSig({3, 4}) && ft.sigAtBar(3) == TimeSig({7, 8}));
        CHECK(c && c->lengthS == 3.0);  // (the file runs to its 7/8 at beat 6: two bars of 3/4 at 120 BPM)
        Rig own;
        own.project.timing.setSignature(2, {5, 4});
        ClipPtr c2 = own.tl->loadClip(path);
        if (c2) own.tl->placeClip(c2, 0, own.project.addTrack(Kind::Midi));
        CHECK(own.project.timing.sigAtBar(1) == TimeSig({4, 4}) && own.project.timing.sigAtBar(3) == TimeSig({5, 4}));
        Rig busy;
        audioClip(busy.project, busy.project.addTrack(Kind::Audio), 0, 1);
        ClipPtr c3 = busy.tl->loadClip(path);
        if (c3) busy.tl->placeClip(c3, 0, busy.project.addTrack(Kind::Midi));
        CHECK(busy.project.timing.plainSignature());
    }
    // a removed current track is forgotten
    tl.setCurrentTrack(a1);
    r.project.removeTrack(a1);
    tl.relayout();
    CHECK(!tl.currentTrack);
}

}  // namespace


// A track's own Volume and Pan lanes name their range as the fader and knob show it (not "max" / "min"),
// and are drawn as present (no plugin to load): automation.py's TrackParameter texts.
void testControlLaneLabels() {
    Rig r;
    auto track = r.project.addTrack(Kind::Audio);
    auto volume = std::make_shared<Automation>();
    volume->slot = track->controls;
    volume->param = "Volume";
    auto pan = std::make_shared<Automation>();
    pan->slot = track->controls;
    pan->param = "Pan";
    track->automation = std::vector<AutomationPtr>{volume, pan};
    automation::resolve(r.project);
    auto v = r.tl->rangeLabels(*volume);
    CHECK(v.first == "+6.0 dB");
    CHECK(v.second == "-inf dB");
    auto p = r.tl->rangeLabels(*pan);
    CHECK(p.first == "100 R");
    CHECK(p.second == "100 L");
    CHECK(r.tl->valueText(*pan, 0.5) == "C");
    CHECK(r.tl->valueText(*pan, 0.35) == "30 L");
    CHECK(r.tl->valueText(*volume, 66.0 / 66.0) == "+6.0 dB");
    CHECK(r.tl->valueText(*volume, 60.0 / 66.0) == "+0.0 dB");
}

// The stylesheets restyle the timeline: a sheet over the built-in default.css changes the ruler's height (and so
// where the lanes start), an alternate lane's colour, a clip part (the selected clip's overlay) and the overview's
// and master lane's boxes; theme::setSkin() brings everything back.
void testStylesheets() {
    Rig r(600, 300);
    Timeline& tl = *r.tl;
    auto t1 = r.project.addTrack(Kind::Audio);
    auto t2 = r.project.addTrack(Kind::Audio);
    auto clip = audioClip(r.project, t1, 1, 1);
    tl.relayout();
    auto picture = [&] {
        ui::App::get().doLayouts();
        Image img(tl.width(), tl.height());
        Painter p(img);
        tl.paintTree(p, tl.rect());
        return img;
    };
    auto lanes = tl.lanes();
    CHECK(lanes.size() == 3 && lanes[0].top == config::RULER_H);
    Image before = picture();
    int alternateY = lanes[1].top + lanes[1].height / 2;
    Color lane = theme::c("lane_alt");
    CHECK(before.pixel(462, alternateY) == lane.premul());  // (no clip, nor grid line, there)
    CHECK(before.pixel(462, config::RULER_H + 2) == theme::c("lane").premul());
    tl.selection.selectOnly(clip);
    Image selected = picture();
    RectF box = *tl.clipRect(*clip, tl.trackTops());
    int clipX = (int)box.x + 30, clipY = (int)box.bottom() - 6;  // (in its body, under its waveform's middle)
    Color body = Color::fromPremul(selected.pixel(clipX, clipY));

    std::vector<css::Problem> problems;
    std::string text;  // (default.css's rules read without a problem, and computing them finds none)
    CHECK(util::readFile("data/skins/default.css", text));
    css::parse(text, "default.css", problems);
    CHECK(problems.empty());
    theme::setStyles({theme::builtinSheet()}, &problems);
    CHECK(problems.empty());
    for (auto& pr : problems) std::printf("%s\n", pr.text().c_str());
    Overview ov(r.project, r.engine, &tl, &r.scroll, 60);
    ov.resize(600, 60);
    auto sheet = css::parse(":root { --ruler-height: 30px; --clip-header-height: 20px; --overview-max-height: 50px; }\n"
                            "Lane:alternate { background-color: #102030; }\n"
                            "Clip:selected::overlay { background-color: #00ff00; }\n"
                            "Ruler { background-color: #400000; }\n"
                            "MasterLane { background-color: #004000; border-top-width: 3px; }\n"
                            "Timeline::playhead { color: #0000ff; stroke-width: 3px; }\n",
                            "test.css", problems);
    CHECK(problems.empty());
    theme::setStyles({theme::builtinSheet(), sheet});
    CHECK(lanes::rulerHeight() == 30 && clip_painter::headerHeight() == 20);
    lanes = tl.lanes();
    CHECK(lanes[0].top == 30 && lanes[1].top == 30 + t1->height);  // (the lanes start under the taller ruler)
    tl.relayout();
    Image styled = picture();
    CHECK(Color::fromPremul(styled.pixel(462, 27)) == Color(0x40, 0, 0));  // the ruler, taller and recoloured
    CHECK(Color::fromPremul(styled.pixel(462, lanes[1].top + lanes[1].height / 2)) == Color(0x10, 0x20, 0x30));
    CHECK(Color::fromPremul(styled.pixel(462, 32)) == theme::c("lane"));  // (the first lane: not alternate)
    CHECK(Color::fromPremul(styled.pixel(clipX, clipY + 6)) == Color(0, 255, 0));  // the selected clip's overlay
    // the clip's name strip is taller: a click 18 px under its top grabs it (the strip), not its body
    RectF moved = *tl.clipRect(*clip, tl.trackTops());
    CHECK(tl.hit(PointF(moved.x + 30, moved.y + 18)).second == ClipZone::Handle);
    // the playhead, 3 px wide
    tl.setPlayhead(r.sec(4));
    Image wide = picture();
    for (int dx = -1; dx <= 1; ++dx) CHECK(Color::fromPremul(wide.pixel(tl.playheadX + dx, 200)) == Color(0, 0, 255));
    // the master lane's box: its background and a 3 px line above it
    MasterLaneView master(&tl);
    master.resize(300, 40);
    Image ml(300, 40);
    {
        Painter p(ml);
        master.paintTree(p, master.rect());
    }
    CHECK(Color::fromPremul(ml.pixel(5, 2)) == theme::c("master_divider"));
    CHECK(Color::fromPremul(ml.pixel(5, 20)) == Color(0, 0x40, 0));

    // the overview is kept within the skin's heights (and says so)
    int resizedTo = 0;
    ov.resizedTo.connect([&](int h) { resizedTo = h; });
    ov.forgetImage();
    CHECK(ov.height() == 50 && resizedTo == 50);
    CHECK(ov.setHeight(200) == 50);

    // back to the default look
    theme::setSkin();
    CHECK(lanes::rulerHeight() == config::RULER_H && clip_painter::headerHeight() == config::CLIP_HANDLE_H);
    CHECK(ov.setHeight(200) == 200 && ov.setHeight(10) == config::OVERVIEW_H);
    tl.setPlayhead(0);
    tl.relayout();
    Image again = picture();
    CHECK(again.width() == selected.width() && again.height() == selected.height());
    int differ = 0;
    for (int y = 0; y < std::min(again.height(), selected.height()); ++y)
        for (int x = 0; x < std::min(again.width(), selected.width()); ++x)
            if (again.pixel(x, y) != selected.pixel(x, y)) ++differ;
    CHECK(differ == 0);
    CHECK(Color::fromPremul(again.pixel(clipX, clipY)) == body);
    CHECK(tl.hit(PointF(box.x + 30, box.y + 18)).second == ClipZone::Body);
    (void)t2;
}

int main() {
    ui::App::get().initHeadless();
    theme::setSkin();
    testSnapping();
    testHitTesting();
    testMove();
    testTrim();
    testRangeEdits();
    testDrops();
    testMidiAndAutomation();
    testControlLaneLabels();
    testLoop();
    testTileCache();
    paintSamples();
    testClipboard();
    testCopyDrag();
    testFadesAndGain();
    testClipEdits();
    testLoopingClips();
    testMarkers();
    testTempoChanges();
    testGridAndSnapping();
    testTakes();
    testTimeEdits();
    testMidiClipsAndExport();
    testStylesheets();
    TEST_MAIN_END();
}
