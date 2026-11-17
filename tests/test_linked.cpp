// Linked MIDI clips (model.h) and the Groove MIDI effect (audio/midi_effects.h), without the GUI: notes
// passed on to every linked clip (prepared again), places and windows kept, making clips unique, undo and
// redo, saving and loading (and old files unchanged), copies independent, a render with linked clips and
// grooves equal to playback; Groove's swing on 1/16 at 120 BPM and over a tempo change, its accent, its
// humanizing the same each time, and the effect kept with its track.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "audio/engine.h"
#include "audio/midi_effects.h"
#include "audio/patch.h"
#include "audio/render.h"
#include "check.h"
#include "config.h"
#include "core/json.h"
#include "core/util.h"
#include "model/edits.h"
#include "model/history.h"
#include "model/model.h"
#include "model/project_io.h"

namespace {

const int SR = 48000;

Note note(double s, double e, double pitch, double vel = 100) {
    Note n;
    n.start = s;
    n.end = e;
    n.pitch = pitch;
    n.velocity = vel;
    return n;
}

ClipPtr midiClip(Project& p, const TrackPtr& t, double start, std::vector<Note> notes, double lengthS) {
    auto c = makeMidiClip("Riff", std::move(notes), lengthS, SR);
    c->track = t;
    c->start = (int64_t)std::llround(start * SR);
    c->prepare(SR);
    p.addClip(c);
    return c;
}

bool sameNotes(const std::vector<Note>& a, const std::vector<Note>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].start != b[i].start || a[i].end != b[i].end || a[i].pitch != b[i].pitch || a[i].velocity != b[i].velocity)
            return false;
    return true;
}

std::shared_ptr<const std::vector<Note>> notesOf(std::vector<Note> v) {
    return std::make_shared<const std::vector<Note>>(std::move(v));
}

// as the MIDI editor commits an edit: new notes, prepared, then passed on to the linked clips
void edit(Project& p, const ClipPtr& c, std::vector<Note> notes) {
    c->notes = notesOf(std::move(notes));
    c->prepare(SR);
    links::notesChanged(p, c, SR);
}

MidiEffect effect(const std::string& kind, const std::map<std::string, double>& values = {}) {
    MidiEffect e;
    MidiEffect::make(kind, values, e);
    return e;
}

// --- linked clips: the model ---

void testLinkedEdits() {
    Project p;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    auto u = p.addTrack(Kind::Midi);
    u->needsInstrument = false;
    auto a = midiClip(p, t, 0, {note(0, 0.5, 60), note(0.5, 1.0, 64)}, 2.0);
    CHECK(!a->link && links::count(p, *a) == 0 && !links::linked(p, *a) && links::partners(p, *a).empty());
    // a linked copy: the same link, notes and data, its own place
    ClipPtr b = links::linkedCopy(a);
    CHECK(a->link && b->link == a->link && b->notes == a->notes);
    b->start = 4 * SR;
    b->track = u;
    b->color = "#ff0000";
    b->name = "Other";
    b->prepare(SR);
    p.addClip(b);
    CHECK(links::count(p, *a) == 2 && links::linked(p, *b));
    auto parts = links::partners(p, *b);
    CHECK(parts.size() == 2 && parts[0] == a && parts[1] == b);
    CHECK(links::counts(p)[a->link.get()] == 2);
    // a plain copy is independent
    ClipPtr plain = links::plainCopy(a);
    CHECK(!plain->link && plain->notes == a->notes);
    // an audio clip's "linked" copy is a plain one
    auto audio = std::make_shared<Clip>();
    CHECK(!links::linkedCopy(audio)->link && !audio->link);
    // editing a's notes: b gets them, prepared again (its played notes and events)
    auto eventsBefore = b->events;
    edit(p, a, {note(0, 0.25, 72, 90), note(0.25, 0.5, 74, 90), note(1.0, 1.5, 76, 90)});
    CHECK(b->notes == a->notes && b->notes->size() == 3);
    CHECK(b->events != eventsBefore && b->events->size() == 3 && (*b->events)[0].pitch == 72);
    CHECK(b->played && b->played->size() == 3 && (*b->played)[2].pitch == 76);
    CHECK(b->data && a->data && b->data->samples == a->data->samples);  // (the same synth, the same notes)
    // their own: place, track, name, colour
    CHECK(b->start == 4 * SR && b->track == u && b->name == "Other" && b->color == "#ff0000" && a->start == 0);
    // switched-off notes, the loop's length and its switch go to the partners too; a window stays its own
    b->length = SR;  // (b plays only its first second)
    b->offset = 0;
    a->mutedNotes = notesOf({note(1.5, 1.75, 50)});
    a->lengthS = 3.0;
    a->loop = true;
    a->length = 6 * SR;
    a->prepare(SR);
    auto changed = links::notesChanged(p, a, SR);
    CHECK(changed.size() == 1 && changed[0] == b);
    CHECK(b->mutedNotes == a->mutedNotes && b->lengthS == 3.0 && b->loop && b->length == SR && a->length == 6 * SR);
    // nothing to pass on: nothing changes
    CHECK(links::notesChanged(p, a, SR).empty());
    // a partner playing to the end of its notes keeps the window it had
    ClipPtr c = links::linkedCopy(a);
    c->start = 10 * SR;
    c->length = -1;
    c->loop = false;
    c->lengthS = 1.0;
    c->prepare(SR);
    int64_t frames = c->frames();
    p.addClip(c);
    links::notesChanged(p, a, SR);
    CHECK(c->length == frames && c->loop && c->lengthS == 3.0);
    // the MIDI effects of each clip's own track, at its own place
    u->midiEffects = std::vector<MidiEffect>{effect("Pitch", {{"semitones", 12}})};
    edit(p, a, {note(0, 0.5, 60)});
    CHECK(b->played->size() >= 1 && (*b->played)[0].pitch == 72 && (*a->played)[0].pitch == 60);
    // Make unique: b keeps its notes but leaves the link; a and c stay linked
    links::makeUnique(p, {b});
    CHECK(!b->link && a->link && c->link == a->link && links::count(p, *a) == 2);
    auto bNotes = b->notes;
    edit(p, a, {note(0, 1, 40)});
    CHECK(b->notes == bNotes && c->notes == a->notes);
    // the last but one leaving: the link is gone from the last too
    links::makeUnique(p, {c});
    CHECK(!c->link && !a->link);
    // (not linked: notesChanged does nothing)
    CHECK(links::notesChanged(p, a, SR).empty());
}

void testCopiesAndPieces() {
    Project p;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    auto a = midiClip(p, t, 0, {note(0, 0.5, 60), note(1.0, 1.5, 62)}, 2.0);
    ClipPtr b = links::linkedCopy(a);
    b->start = 3 * SR;
    p.addClip(b);
    // Ctrl+D and duplicating a range: independent copies
    auto dup = edits::duplicateClips(p, {a});
    CHECK(dup.size() == 1 && !dup[0]->link && a->link);
    auto range = edits::duplicateRange(p, {t}, 3 * SR, 3 * SR + SR / 2);
    CHECK(!range.empty());
    for (auto& r : range) CHECK(!r->link);
    // cutting a range out of a linked clip: the pieces left are the same notes in smaller windows, linked
    Project q;
    auto qt = q.addTrack(Kind::Midi);
    qt->needsInstrument = false;
    auto x = midiClip(q, qt, 0, {note(0, 0.5, 60), note(1.0, 1.5, 62)}, 2.0);
    ClipPtr y = links::linkedCopy(x);
    y->start = 4 * SR;
    q.addClip(y);
    edits::deleteRange(q, {qt}, SR / 2, SR);
    CHECK(q.clips.size() == 3);
    for (auto& c : q.clips) CHECK(c->link == x->link);
    // a joined clip is a new one: not linked
    ClipPtr joined = edits::consolidated({q.clips[0], q.clips[1]}, SR);
    CHECK(joined && !joined->link);
}

void testUndoRedo() {
    Project p;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    auto a = midiClip(p, t, 0, {note(0, 0.5, 60)}, 2.0);
    history::History h(p, SR);
    ClipPtr b = links::linkedCopy(a);
    b->start = 4 * SR;
    p.addClip(b);
    CHECK(h.check(SR));
    auto before = a->notes;
    auto eventsBefore = b->events;
    edit(p, a, {note(0, 0.5, 67), note(0.5, 1.0, 69)});
    CHECK(h.check(SR));
    CHECK(b->notes->size() == 2);
    // undo: both clips' notes (and b's prepared events) as they were
    CHECK(h.undo(SR));
    CHECK(a->notes == before && b->notes == before && b->events == eventsBefore && b->link == a->link);
    // redo: both again
    CHECK(h.redo(SR));
    CHECK(a->notes->size() == 2 && b->notes == a->notes && (*b->events)[0].pitch == 67);
    // Make unique, undone: linked again
    links::makeUnique(p, {b});
    CHECK(h.check(SR) && !a->link && !b->link);
    CHECK(h.undo(SR));
    CHECK(a->link && b->link == a->link);
    // undoing the linked copy: gone, and a not linked any more
    CHECK(h.undo(SR) && h.undo(SR));
    CHECK(p.clips.size() == 1 && !a->link);
}

std::string dirFor(const std::string& name) {
    std::string d = "build/test-linked/" + name;
    util::makeDirs(d);
    return d;
}

void testSaveLoad() {
    Project p;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    auto u = p.addTrack(Kind::Midi);
    u->needsInstrument = false;
    u->midiEffects = std::vector<MidiEffect>{effect("Groove", {{"swing", 66}, {"grid", 1}, {"accent", 20}, {"timing", 5},
                                                                {"velocity", 8}, {"seed", 3}})};
    auto a = midiClip(p, t, 0, {note(0, 0.5, 60), note(0.5, 1.0, 64)}, 2.0);
    a->mutedNotes = notesOf({note(1.0, 1.5, 50)});
    auto plain = midiClip(p, t, 2.5, {note(0, 0.25, 48)}, 1.0);
    ClipPtr b = links::linkedCopy(a);
    b->start = 4 * SR;
    b->track = u;
    b->length = SR;
    b->prepare(SR);
    p.addClip(b);
    ClipPtr c = links::linkedCopy(a);
    c->start = 6 * SR;
    p.addClip(c);
    ClipPtr d = links::linkedCopy(plain);  // (a second link)
    d->start = 8 * SR;
    p.addClip(d);
    project_io::MasterSettings master;
    std::string path = dirFor("links") + "/Linked.json", err;
    CHECK(project_io::saveProject(path, p, SR, master, err));
    Json saved = project_io::describe(p, SR, master);
    const Json& clips = saved["clips"];
    CHECK(clips.size() == 5);
    CHECK(clips[size_t(0)]["link"].toDouble() == 1 && clips[size_t(2)]["link"].toDouble() == 1 &&
          clips[size_t(3)]["link"].toDouble() == 1);
    CHECK(clips[size_t(1)]["link"].toDouble() == 2 && clips[size_t(4)]["link"].toDouble() == 2);
    Project q;
    project_io::MasterSettings m2;
    CHECK(project_io::loadProject(path, q, SR, m2, err));
    CHECK(q.clips.size() == 5);
    CHECK(project_io::describe(q, SR, m2).dump(2) == saved.dump(2));  // (as saved)
    if (q.clips.size() == 5) {
        const ClipPtr &qa = q.clips[0], &qp = q.clips[1], &qb = q.clips[2], &qc = q.clips[3], &qd = q.clips[4];
        CHECK(qa->link && qb->link == qa->link && qc->link == qa->link && qp->link && qd->link == qp->link &&
              qp->link != qa->link);
        CHECK(qb->notes == qa->notes && qc->notes == qa->notes && qb->mutedNotes == qa->mutedNotes);
        CHECK(sameNotes(*qa->notes, *a->notes) && qb->length == SR && qb->start == 4 * SR);
        // the effect, with its values, on its track
        CHECK(q.tracks[1]->midiEffects.size() == 1 && q.tracks[1]->midiEffects[0] == u->midiEffects[0]);
        // and they stay linked: an edit of one reaches the others
        edit(q, qc, {note(0, 1, 70)});
        CHECK(qa->notes == qc->notes && qb->notes == qc->notes && qd->notes != qc->notes);
    }
    // a link with one clip left in the project isn't saved
    p.removeClips({c, b});
    const Json one = project_io::describe(p, SR, master);
    CHECK(!one["clips"][size_t(0)].has("link") && one["clips"][size_t(1)]["link"].toDouble() == 1);
    // a project without links saves exactly as before: no "link" anywhere
    Project r;
    auto rt = r.addTrack(Kind::Midi);
    rt->needsInstrument = false;
    midiClip(r, rt, 0, {note(0, 0.5, 60)}, 1.0);
    std::string text = project_io::describe(r, SR, master).dump(2);
    CHECK(text.find("\"link\"") == std::string::npos);
    // an older project (no links) opens unchanged: its clips independent, though their notes are the same
    std::string oldPath = dirFor("old") + "/Old.json";
    std::string old = R"({"format": "tinydaw-project", "version": 1, "tracks": [{"name": "MIDI 1", "kind": "midi"}],
        "clips": [{"kind": "midi", "name": "A", "track": 0, "start": 0, "offset": 0, "length": null,
                   "notes": [[0, 0.5, 60, 100]], "midi_length": 1.0},
                  {"kind": "midi", "name": "B", "track": 0, "start": 2, "offset": 0, "length": null,
                   "notes": [[0, 0.5, 60, 100]], "midi_length": 1.0}]})";
    FILE* f = std::fopen(oldPath.c_str(), "wb");
    if (f) {
        std::fwrite(old.data(), 1, old.size(), f);
        std::fclose(f);
    }
    Project o;
    project_io::MasterSettings m3;
    bool opened = project_io::loadProject(oldPath, o, SR, m3, err);
    if (!opened) printf("old project: %s\n", err.c_str());
    CHECK(opened && o.clips.size() == 2);
    if (o.clips.size() == 2) {
        CHECK(!o.clips[0]->link && !o.clips[1]->link && o.clips[0]->notes != o.clips[1]->notes);
        CHECK(project_io::describe(o, SR, m3).dump().find("\"link\"") == std::string::npos);
    }
}

// --- Groove ---

void testGroove() {
    Timing timing;  // 120 BPM: a beat 0.5 s, a 1/16 0.125 s
    midi_effects::TimingScope scope(&timing);
    std::vector<Note> in;
    for (int i = 0; i < 8; i++) in.push_back(note(i * 0.125, i * 0.125 + 0.1, 60 + i, 100));
    in.push_back(note(0.0625, 0.08, 50, 90));  // (half way to the first off-beat)
    // 50 %: unchanged, exactly
    auto straight = midi_effects::process(effect("Groove", {{"swing", 50}}), in, 0);
    CHECK(straight.size() == in.size());
    bool same = straight.size() == in.size();
    for (size_t i = 0; same && i < in.size(); i++)
        same = straight[i].start == in[i].start && straight[i].end == in[i].end && straight[i].pitch == in[i].pitch &&
               straight[i].velocity == in[i].velocity;
    CHECK(same);
    // 66 % on 1/16: the off-beats (every second 1/16) 0.16 of a pair of 1/16s later: 0.04 s at 120 BPM
    auto swung = midi_effects::process(effect("Groove", {{"swing", 66}, {"grid", 1}}), in, 0);
    for (int i = 0; i < 8; i++) {
        double expect = i * 0.125 + (i % 2 ? 0.04 : 0.0);
        CHECK_NEAR(swung[i].start, expect, 1e-9);
        CHECK_NEAR(swung[i].end - swung[i].start, 0.1, 1e-9);  // (lengths kept)
        CHECK(swung[i].velocity == 100);
    }
    CHECK_NEAR(swung[8].start, 0.0625 * 1.32, 1e-9);  // (between the lines: in proportion)
    // at the timeline's 10 s (origin): the same, on the song's grid
    auto later = midi_effects::process(effect("Groove", {{"swing", 66}}), in, 10.0);
    CHECK_NEAR(later[1].start, 0.125 + 0.04, 1e-9);
    // 1/8: the off-beat is the second 1/8 (0.25 s), 0.08 s later
    auto eighths = midi_effects::process(effect("Groove", {{"swing", 66}, {"grid", 0}}), in, 0);
    CHECK_NEAR(eighths[2].start, 0.25 + 0.08, 1e-9);
    CHECK_NEAR(eighths[4].start, 0.5, 1e-9);
    // over a tempo change: 60 BPM from bar 2 (4 s at 120 BPM… bar 2 starts at 2 s): a beat 1 s there
    Timing slow;
    slow.setChange(2, 60, 4);
    slow.publish();
    {
        midi_effects::TimingScope s2(&slow);
        std::vector<Note> two = {note(2.25, 2.35, 60), note(0.125, 0.2, 62)};
        auto out = midi_effects::process(effect("Groove", {{"swing", 66}}), two, 0);
        CHECK_NEAR(out[0].start, 2.25 + 0.08, 1e-9);  // (0.16 of half a beat: 0.08 beats of 1 s)
        CHECK_NEAR(out[1].start, 0.125 + 0.04, 1e-9);  // (before the change: 120 BPM)
    }
    // the accent: the off-beats louder, the beats not
    auto accented = midi_effects::process(effect("Groove", {{"swing", 50}, {"accent", 20}}), in, 0);
    CHECK(accented[1].velocity == 120 && accented[0].velocity == 100 && accented[8].velocity == 99);
    auto softer = midi_effects::process(effect("Groove", {{"swing", 50}, {"accent", -50}}), in, 0);
    CHECK(softer[3].velocity == 50);
    // humanizing: within its amounts, and the same every time (seeded by the note); another seed: other amounts
    MidiEffect human = effect("Groove", {{"swing", 50}, {"timing", 10}, {"velocity", 12}, {"seed", 4}});
    auto h1 = midi_effects::process(human, in, 0), h2 = midi_effects::process(human, in, 0);
    bool repeat = true, moved = false, within = true;
    for (size_t i = 0; i < in.size(); i++) {
        repeat = repeat && h1[i].start == h2[i].start && h1[i].velocity == h2[i].velocity;
        moved = moved || h1[i].start != in[i].start;
        within = within && h1[i].start >= 0 && std::fabs(h1[i].start - in[i].start) <= 0.010 + 1e-12 &&
                 std::fabs(h1[i].velocity - in[i].velocity) <= 12 && h1[i].end - h1[i].start == in[i].end - in[i].start;
    }
    CHECK(repeat && moved && within);
    auto h3 = midi_effects::process(human.with("seed", 5), in, 0);
    bool differs = false;
    for (size_t i = 0; i < in.size(); i++) differs = differs || h3[i].start != h1[i].start;
    CHECK(differs);
    // in a clip: prepared again and again, the same notes; and not live (it changes timing)
    Project p;
    midi_effects::TimingScope s3(&p.timing);
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    t->midiEffects = std::vector<MidiEffect>{human.with("swing", 62)};
    auto c = midiClip(p, t, 1.0, in, 1.0);
    auto played = c->played;
    c->prepare(SR);
    CHECK(c->played != played && sameNotes(*c->played, *played));
    CHECK(!midi_effects::isLive("Groove") && midi_effects::live(*t, 60, 100).size() == 1 &&
          midi_effects::live(*t, 60, 100)[0].second == 100);
    // its parameters and description
    std::vector<midi_effects::ParamInfo> ps = midi_effects::params("Groove");
    CHECK(ps.size() == 6 && ps[0].key == "swing" && ps[0].low == 50 && ps[0].high == 75 && ps[1].choices.size() == 3);
    CHECK(!midi_effects::describe("Groove").empty() && midi_effects::describe("Nope").empty());
    CHECK(effect("Groove").get("swing") == 60 && effect("Groove").get("grid") == 1);
}

// --- a render of linked clips (with a groove) is what playback plays ---

void testRenderEqualsPlayback() {
    Project p;
    audio::Engine e(p);  // (first: it installs the MIDI effects and the sampler for clips being prepared)
    e.offline = true;
    p.timing.setChange(2, 100, 4);  // (a tempo change: the groove follows it)
    p.timing.publish();
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    auto u = p.addTrack(Kind::Midi);
    u->needsInstrument = false;
    u->midiEffects = std::vector<MidiEffect>{effect("Groove", {{"swing", 64}, {"accent", 15}, {"timing", 6}, {"velocity", 9}})};
    std::vector<Note> riff;
    for (int i = 0; i < 12; i++) riff.push_back(note(i * 0.15, i * 0.15 + 0.12, 55 + (i * 7) % 12, 80 + i));
    auto a = midiClip(p, t, 0.5, riff, 2.0);
    ClipPtr b = links::linkedCopy(a);
    b->start = SR * 2;
    b->track = u;
    b->prepare(SR);
    p.addClip(b);
    ClipPtr c = links::linkedCopy(a);
    c->start = SR * 3;
    c->offset = SR / 4;
    c->length = SR;
    c->prepare(SR);
    p.addClip(c);
    edit(p, a, riff);  // (an edit passed on: what plays is what the partners were prepared with)
    std::vector<Note> more = riff;
    more.push_back(note(1.8, 1.95, 79, 110));
    edit(p, b, more);
    CHECK(a->notes == b->notes && c->notes == b->notes);
    patch::ensureStates(p);
    const int block = config::RENDER_BLOCK;
    const int64_t start = 0, end = SR * 5;
    e.seek(0);
    e.play();
    std::vector<float> played;
    std::vector<float> buf((size_t)block * 2);
    while ((int64_t)played.size() / 2 < end + SR) {
        e.callback(nullptr, buf.data(), block);
        played.insert(played.end(), buf.begin(), buf.end());
    }
    e.stop();
    int latency = e.latency;
    auto rendered = render::renderMix(p, start, end, SR);
    CHECK(rendered && (int64_t)rendered->size() == (end - start) * 2);
    if (!rendered) return;
    int64_t differ = 0;
    double loudest = 0;
    for (int64_t j = 0; j < (end - start) * 2; j++) {
        float x = (*rendered)[(size_t)j], y = played[(size_t)((start + latency) * 2 + j)];
        if (x != y) differ++;
        loudest = std::max(loudest, (double)std::fabs(x));
    }
    CHECK(differ == 0);
    CHECK(loudest > 0.01);
    if (differ) printf("render vs playback: %lld samples differ\n", (long long)differ);
}

// features meeting: note properties (chance, repeats, slide…) set on one linked clip reach every copy, and
// the Groove effect moves the notes in time without losing them
void testWithNoteProperties() {
    const int sr = 48000;
    Project p;
    TrackPtr t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    Timing timing;  // 120 BPM: a 1/16 is 0.125 s; the second and fourth notes are off-beat 16ths
    midi_effects::TimingScope scope(&timing);
    auto a = makeMidiClip("a", {{0.0, 0.1, 60, 100}, {0.125, 0.225, 62, 100}, {0.25, 0.35, 64, 100}, {0.375, 0.475, 65, 100}},
                          1.0, sr);
    a->track = t;
    a->prepare(sr);
    p.addClip(a);
    ClipPtr b = links::linkedCopy(a);
    b->start = sr * 2;
    b->prepare(sr);
    p.addClip(b);
    // the second note: half the time, a roll of 3, sliding; the third panned
    std::vector<Note> notes = *a->notes;
    notes[1].chance = 50;
    notes[1].repeats = 3;
    notes[1].slide = true;
    notes[2].pan = -0.5;
    a->notes = std::make_shared<const std::vector<Note>>(notes);
    auto changed = links::notesChanged(p, a, sr);
    CHECK(std::find(changed.begin(), changed.end(), b) != changed.end());
    CHECK(b->notes && b->notes->size() == 4);
    if (b->notes && b->notes->size() == 4) {
        const Note& n = (*b->notes)[1];
        CHECK(n.chance == 50 && n.repeats == 3 && n.slide && (*b->notes)[2].pan == -0.5);
    }
    // the copies play the same (a hit of the roll and the chance alike: decided by the notes, not the clip)
    auto pa = a->performed(sr), pb = b->performed(sr);
    CHECK(pa.size() == pb.size());
    // Groove moves the off-beat 16ths later and keeps every note's properties
    MidiEffect groove;
    CHECK(MidiEffect::make("Groove", {{"swing", 66}, {"grid", 1}}, groove));
    t->midiEffects = std::vector<MidiEffect>{groove};
    std::vector<Note> written = *a->notes;
    std::vector<Note> swung = midi_effects::apply(*t, written, 0.0);
    CHECK(swung.size() == written.size());
    bool kept = false, panKept = false, moved = false, onBeatStill = true;
    for (auto& n : swung) {
        if (n.pitch == 62) {
            kept = n.chance == 50 && n.repeats == 3 && n.slide;
            moved = n.start > 0.125 + 1e-3;  // (an off-beat 16th: later)
        }
        if (n.pitch == 64) {
            panKept = n.pan == -0.5;
            onBeatStill = std::fabs(n.start - 0.25) < 1e-9;  // (on the grid's first step: where it was)
        }
    }
    CHECK(kept);
    CHECK(panKept);
    CHECK(moved);
    CHECK(onBeatStill);
}

}  // namespace

int main() {
    midi_effects::install(nullptr);
    testLinkedEdits();
    testCopiesAndPieces();
    testUndoRedo();
    testSaveLoad();
    testGroove();
    testRenderEqualsPlayback();
    testWithNoteProperties();
    TEST_MAIN_END();
}
