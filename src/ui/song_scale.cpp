#include "ui/song_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "audio/key_detect.h"
#include "audio/midi_effects.h"
#include "audio/workers.h"
#include "core/util.h"

namespace song_scale {

namespace {
bool sameAmount(double a, double b) { return std::fabs(a - b) < 1e-6; }

// a checkable entry of an exclusive group
ui::Action* choice(ui::Menu& m, const std::string& text, bool on, int group, std::function<void()> fn) {
    ui::Action* a = m.addAction(text, std::move(fn));
    a->checkable = true;
    a->checked = on;
    a->group = group;
    return a;
}

int nextGroup() {
    static int groups = 5000;
    return ++groups;
}

// the amounts to choose from, and the one in use when it isn't one of them
void addAmounts(ui::Menu& m, double now, bool chosen, const std::function<void(double)>& set) {
    int group = nextGroup();
    bool listed = false;
    for (double a : swingChoices()) {
        bool on = chosen && sameAmount(a, now);
        listed |= on;
        choice(m, a == 0 ? "Straight (0 %)" : a == 67 ? "67 % (triplets)" : swingText(a), on, group, [set, a] { set(a); });
    }
    if (chosen && !listed) choice(m, swingText(now), true, group, [set, now] { set(now); });
}

// the candidates for `notes`, each setting the key and scale it names
void addCandidates(ui::Menu& m, const std::vector<Note>& notes, const std::function<void(int, std::string)>& set) {
    std::vector<key_detect::Candidate> found = key_detect::detect(notes, 5);
    if (found.empty()) {
        m.addAction("(no notes to tell from)")->enabled = false;
        return;
    }
    for (size_t i = 0; i < found.size(); i++) {
        const key_detect::Candidate c = found[i];
        char score[32];
        std::snprintf(score, sizeof score, " (%.0f %%)", std::max(0.0, std::min(1.0, c.score)) * 100);
        m.addAction(key_detect::name(c) + score + (i == 0 ? "  \xe2\x80\x94 the likeliest" : ""),
                    [set, c] { set(c.key, c.scale); });
    }
}

std::vector<Note> notesOf(const std::vector<ClipPtr>& clips) {
    std::vector<Note> all;
    for (auto& c : clips)
        if (c->kind == Kind::Midi && c->notes) all.insert(all.end(), c->notes->begin(), c->notes->end());
    return all;
}

bool onAny(const Clip& c, const std::vector<TrackPtr>& tracks) {
    if (tracks.empty()) return true;
    TrackPtr t = std::atomic_load(&c.track);
    return std::find(tracks.begin(), tracks.end(), t) != tracks.end();
}

void call(const std::function<void()>& fn) {
    if (fn) fn();
}
}  // namespace

const std::vector<double>& swingChoices() {
    static const std::vector<double> c = {0, 10, 20, 25, 33, 40, 50, 60, 67, 75, 100};
    return c;
}

std::string swingText(double amount) {
    if (amount <= 0) return "straight";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g %%", std::round(amount * 10) / 10);
    return buf;
}

std::string scaleName(int key, const std::string& scale) {
    if (scale.empty()) return "no scale";
    return key_detect::name({key, scale, 0});
}

int keyOf(const Project& p, const Clip& c) { return c.key >= 0 ? c.key : p.timing.key; }
std::string scaleOf(const Project& p, const Clip& c) { return c.key >= 0 ? c.scale : p.timing.scale; }

// --- preparing clips again, on every core ---

int reprepare(Project& p, int sr, const std::vector<TrackPtr>& tracks) {
    std::vector<ClipPtr> todo;
    for (const ClipPtr& c : p.clips)
        if (c->kind == Kind::Midi && std::atomic_load(&c->track) && onAny(*c, tracks)) todo.push_back(c);
    if (todo.empty()) return 0;
    p.timing.publish();  // (the workers read the tempo map: up to date before they start)
    struct Work {
        std::vector<ClipPtr>* todo;
        std::vector<Clip> ready;
        int sr;
    } work{&todo, {}, sr};
    work.ready.reserve(todo.size());
    for (auto& c : todo) {
        work.ready.push_back(*c);
        work.ready.back().track = std::atomic_load(&c->track);
    }
    auto job = [](void* ctx, int i) {
        Work& w = *static_cast<Work*>(ctx);
        w.ready[(size_t)i].prepare(w.sr);
    };
    int threads = todo.size() >= 4 ? workers::workersFor(workers::chosenCores()) : 0;
    {
        workers::Pool pool(threads, false);  // (as a render's: never outranking playback)
        pool.run((int)todo.size(), job, &work);
    }
    for (size_t i = 0; i < todo.size(); i++) {  // (the audio thread atomic_loads them)
        Clip& r = work.ready[i];
        todo[i]->baseFrames = r.baseFrames;
        std::atomic_store(&todo[i]->peaks, r.peaks);
        std::atomic_store(&todo[i]->played, r.played);
        std::atomic_store(&todo[i]->events, r.events);
        std::atomic_store(&todo[i]->data, r.data);
    }
    util::debugf("song_scale: %zu MIDI clips prepared again on %d worker threads (+1)\n", todo.size(), threads);
    return (int)todo.size();
}

// --- swing ---

std::function<void()> notesCommitted;

int commitSwing(Project& p, int sr, const std::vector<TrackPtr>& tracks) {
    std::vector<TrackPtr> scope = tracks;
    if (scope.empty())
        for (auto& t : p.tracks)
            if (t->kind == Kind::Midi) scope.push_back(t);
    std::set<const ClipLink*> done;  // (linked clips: the first one's notes are everyone's)
    std::vector<ClipPtr> changed;
    for (const ClipPtr& c : p.clips) {
        TrackPtr t = std::atomic_load(&c->track);
        if (c->kind != Kind::Midi || !t || !onAny(*c, scope) || !midi_effects::swings(*t)) continue;
        if (c->link && !done.insert(c->link.get()).second) continue;
        double origin = (double)(c->start - c->offset) / sr;
        if (c->notes) c->notes = std::make_shared<const std::vector<Note>>(midi_effects::swung(*t, *c->notes, origin));
        if (c->mutedNotes)
            c->mutedNotes = std::make_shared<const std::vector<Note>>(midi_effects::swung(*t, *c->mutedNotes, origin));
        changed.push_back(c);
    }
    // now straight: the song's (the whole song) or those tracks' own; their rows' amounts gone
    if (tracks.empty()) p.timing.swing = 0;
    for (auto& t : scope) {
        t->swing = tracks.empty() ? -1 : 0;
        std::vector<MidiEffect> effects = t->midiEffects.copy();
        bool rows = false;
        for (auto& e : effects)
            for (auto it = e.values.begin(); it != e.values.end();)
                if (it->first.compare(0, 4, "row:") == 0) {
                    it = e.values.erase(it);
                    rows = true;
                } else {
                    ++it;
                }
        if (rows) t->midiEffects = effects;
    }
    for (auto& c : changed) links::notesChanged(p, c, sr);
    reprepare(p, sr, scope);
    if (!changed.empty() && notesCommitted) notesCommitted();
    util::debugf("song_scale: swing committed into the notes of %zu MIDI clips (%s)\n", changed.size(),
                 tracks.empty() ? "the whole song" : "some tracks");
    return (int)changed.size();
}

void setSongSwing(Project& p, int sr, double amount, int grid) {
    if (amount >= 0) p.timing.swing = std::min(100.0, amount);
    if (grid >= 0) p.timing.swingGrid = grid ? 1 : 0;
    util::debugf("song_scale: the song's swing %s of the %s\n", swingText(p.timing.swing).c_str(),
                 p.timing.swingGrid ? "16ths" : "8ths");
    reprepare(p, sr);
}

void setTrackSwing(Project& p, int sr, const TrackPtr& t, double amount) {
    if (!t) return;
    t->swing = amount < 0 ? -1 : std::min(100.0, amount);
    util::debugf("song_scale: track \"%s\" swings %s\n", t->name.c_str(),
                 t->swing < 0 ? "as the song does" : swingText(t->swing).c_str());
    reprepare(p, sr, {t});
}

void setRowSwing(Project& p, int sr, const TrackPtr& t, int pitch, double amount) {
    if (!t || pitch < 0 || pitch > 127) return;
    std::vector<MidiEffect> effects = t->midiEffects.copy();
    auto groove = std::find_if(effects.begin(), effects.end(), [](const MidiEffect& e) { return e.kind == "Groove"; });
    if (groove == effects.end()) {
        if (amount < 0) return;  // (nothing to take away)
        MidiEffect e;
        MidiEffect::make("Groove", {{"swing", 50}}, e);  // (straight itself: only the row swings)
        effects.push_back(e);
        groove = effects.end() - 1;
    }
    *groove = midi_effects::withRowSwing(*groove, pitch, amount);
    t->midiEffects = effects;  // (replaced whole: undo and the audio thread see whole lists)
    util::debugf("song_scale: track \"%s\", row %d swings %s\n", t->name.c_str(), pitch,
                 amount < 0 ? "as the track does" : swingText(amount).c_str());
    reprepare(p, sr, {t});
}

// --- the menus ---

void fillKeyMenu(ui::Menu& menu, Project& p, const std::vector<Note>& notes, const std::string& from,
                 const std::function<void()>& changed) {
    Project* project = &p;
    auto set = [project, changed](int key, const std::string& scale) {
        project->timing.key = key;
        project->timing.scale = scale;
        util::debugf("song_scale: the song's key %s\n", scaleName(key, scale).c_str());
        call(changed);
    };
    const Timing& t = p.timing;  // (short, for the terminal too: Detect first, the keys and scales in submenus)
    addCandidates(*menu.addMenu("Detect from " + from), notes, set);
    menu.addSeparator();
    ui::Menu* keyMenu = menu.addMenu(std::string("Key: ") + (t.scale.empty() ? "none" : midi_effects::KEYS[((t.key % 12) + 12) % 12]));
    int keys = nextGroup();
    for (int k = 0; k < 12; k++)  // (a key with no scale yet: major)
        choice(*keyMenu, midi_effects::KEYS[k], !t.scale.empty() && t.key == k, keys,
               [set, project, k] { set(k, project->timing.scale.empty() ? "Major" : project->timing.scale); });
    ui::Menu* scaleMenu = menu.addMenu("Scale: " + (t.scale.empty() ? std::string("none") : t.scale));
    int scales = nextGroup();
    choice(*scaleMenu, "None", t.scale.empty(), scales, [set, project] { set(project->timing.key, ""); });
    for (auto& s : midi_effects::SCALES) {
        std::string name = s.first;
        choice(*scaleMenu, name, t.scale == name, scales, [set, project, name] { set(project->timing.key, name); });
    }
}

void fillSwingMenu(ui::Menu& menu, Project& p, int sr, const std::function<void()>& changed) {
    Project* project = &p;
    menu.addSection("The song's swing");
    addAmounts(menu, p.timing.swing, true, [project, sr, changed](double a) {
        setSongSwing(*project, sr, a);
        call(changed);
    });
    menu.addSection("What swings");
    int grid = nextGroup();
    for (int g : {0, 1})
        choice(menu, g ? "The 16ths" : "The 8ths", p.timing.swingGrid == g, grid, [project, sr, changed, g] {
            setSongSwing(*project, sr, -1, g);
            call(changed);
        });
    menu.addSeparator();
    bool any = false;
    for (auto& t : p.tracks) any |= t->kind == Kind::Midi && midi_effects::swings(*t);
    ui::Action* commit = menu.addAction("Commit swing into the notes", [project, sr, changed] {
        commitSwing(*project, sr);
        call(changed);
    });
    commit->enabled = any;
    commit->tooltip = "Write the swing into every MIDI clip's notes; the song then plays straight (it sounds the same)";
}

void addClipScaleMenu(ui::Menu& menu, Project& p, const std::vector<ClipPtr>& midiClips,
                      const std::function<void()>& changed) {
    bool own = false;
    for (auto& c : midiClips) own |= c->key >= 0;
    std::string now = midiClips.size() == 1 && own ? scaleName(midiClips[0]->key, midiClips[0]->scale) : "";
    ui::Menu* m = menu.addMenu(own ? "Scale: its own" + (now.empty() ? std::string() : " (" + now + ")")
                                   : "Scale: the song's");
    if (!menu.actions().empty()) menu.actions().back()->enabled = !midiClips.empty();
    std::vector<ClipPtr> clips = midiClips;
    auto set = [clips, changed](int key, const std::string& scale) {
        for (auto& c : clips) {
            c->key = key;
            c->scale = key < 0 ? std::string() : scale;
        }
        util::debugf("song_scale: %zu MIDI clips use %s\n", clips.size(),
                     key < 0 ? "the song's scale" : scaleName(key, scale).c_str());
        call(changed);
    };
    int group = nextGroup();
    choice(*m, "The song's (" + scaleName(p.timing.key, p.timing.scale) + ")", !own, group, [set] { set(-1, ""); });
    const Clip* first = midiClips.empty() ? nullptr : midiClips[0].get();
    int key = first ? keyOf(p, *first) : 0;
    std::string scale = first ? scaleOf(p, *first) : std::string();
    ui::Menu* keys = m->addMenu("Its own key");
    int kg = nextGroup();
    for (int k = 0; k < 12; k++)
        choice(*keys, midi_effects::KEYS[k], own && key == k, kg,
               [set, scale, k] { set(k, scale.empty() ? "Major" : scale); });
    ui::Menu* scales = m->addMenu("Its own scale");
    int sg = nextGroup();
    choice(*scales, "None", own && scale.empty(), sg, [set, key] { set(key, ""); });
    for (auto& s : midi_effects::SCALES) {
        std::string name = s.first;
        choice(*scales, name, own && scale == name, sg, [set, key, name] { set(key, name); });
    }
    addCandidates(*m->addMenu("Detect from its notes"), notesOf(midiClips), set);
}

void addTrackSwingMenu(ui::Menu& menu, Project& p, int sr, const TrackPtr& t, const std::function<void()>& changed) {
    if (!t || t->kind != Kind::Midi) return;
    ui::Menu* m = menu.addMenu("Swing: " + (t->swing < 0 ? "the song's" : swingText(t->swing)));
    Project* project = &p;
    int group = nextGroup();
    choice(*m, "The song's (" + swingText(p.timing.swing) + ")", t->swing < 0, group, [project, sr, t, changed] {
        setTrackSwing(*project, sr, t, -1);
        call(changed);
    });
    m->addSection("Its own");
    addAmounts(*m, t->swing, t->swing >= 0, [project, sr, t, changed](double a) {
        setTrackSwing(*project, sr, t, a);
        call(changed);
    });
    m->addSeparator();
    ui::Action* commit = m->addAction("Commit its swing into the notes", [project, sr, t, changed] {
        commitSwing(*project, sr, {t});
        call(changed);
    });
    commit->enabled = midi_effects::swings(*t);
}

void addRowSwingMenu(ui::Menu& menu, Project& p, int sr, const TrackPtr& t, int pitch,
                     const std::function<void()>& changed) {
    if (!t) return;
    double own = midi_effects::rowSwing(*t, pitch);
    double track = t->swing >= 0 ? t->swing : midi_effects::songSwing();
    ui::Menu* m = menu.addMenu("Swing of this row: " + (own < 0 ? "the track's" : swingText(own)));
    Project* project = &p;
    int group = nextGroup();
    choice(*m, "The track's (" + swingText(track) + ")", own < 0, group, [project, sr, t, pitch, changed] {
        setRowSwing(*project, sr, t, pitch, -1);
        call(changed);
    });
    m->addSection("Its own");
    addAmounts(*m, own, own >= 0, [project, sr, t, pitch, changed](double a) {
        setRowSwing(*project, sr, t, pitch, a);
        call(changed);
    });
}

}  // namespace song_scale
