// KEYBOARD FOCUS (ui/midi_input.h): one keyboard playing several instruments. Headless over the main window: the
// focus follows the selection, a MIDI track's toggle adds it or takes it out (every lit track gets the same notes,
// sustain and bend, a note let go where it started), Lock keyboard focus keeps the lit set through selection changes,
// the failsafes (a focused track removed, unable to play, none left: the selected one), undo leaves the focus alone,
// saving and opening keep it (older projects: none), the terminal (Shift+K, ♪, F2's lock, F1) and a worst case
// (64 MIDI tracks focused, notes hammered while toggling focus, lock and selection). Pictures of the panel's toggles
// in every skin go to build/ui-samples/keyboard-focus/.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "audio/engine.h"
#include "audio/live.h"
#include "audio/midi_in.h"
#include "audio/plugin_audio.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "gfx/painter.h"
#include "model/project_io.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/lane_panel.h"
#include "ui/main_window.h"
#include "ui/midi_input.h"
#include "ui/skin.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/widgets.h"

using namespace tui;
using midi_in::Event;

namespace {

const std::string OUT = "build/ui-samples/keyboard-focus";
const std::string TMP = OUT + "/tmp-focus";

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

struct Driver {
    MainWindow* w;
    Front* f;
    Screen screen{120, 34};
    void keys(const std::vector<Key>& ks) {
        for (const Key& k : ks) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            f->key(k);
        }
        settle();
    }
    void settle() {
        for (int i = 0; i < 7; i++) {  // (the undo history looks every 250 ms)
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
        }
        f->draw(screen);
    }
    bool shows(const std::string& t) const { return screen.lines().find(t) != std::string::npos; }
};

Image paintWidget(ui::Widget* w) {
    ui::App::get().doLayouts();
    Image img(std::max(1, w->width()), std::max(1, w->height()), theme::c("window"));
    Painter p(img);
    w->paintTree(p, w->rect());
    return img;
}

ui::Action* find(const ui::Menu& m, const std::string& text) {
    for (auto& a : m.actions())
        if (a->text == text) return a.get();
    return nullptr;
}

struct Rig {
    MainWindow& w;
    audio::Engine& engine;
    midi_input::MidiInput& in;
    // is a note held live on the track (what was handed over taken, as the audio thread would)
    // (the notes each track started and stopped, as its live notes hand them back: built-in voices too)
    std::map<const Track*, std::map<int, int>> sounding;
    bool held(const TrackPtr& t) {
        auto live = engine.liveNotes(t.get());
        live->report(true);
        plugin_audio::Messages m;
        live->take(64, engine.sr, m);
        std::vector<live::Played> ps;
        live->played(ps);
        auto& counts = sounding[t.get()];
        for (auto& pl : ps) counts[pl.pitch] += pl.on ? 1 : -1;
        for (auto& kv : counts)
            if (kv.second > 0) return true;
        return false;
    }
    // the semitones its built-in voices are bent by, once the wheel's smoothing has caught up
    double bend(const TrackPtr& t) {
        auto live = engine.liveNotes(t.get());
        plugin_audio::Messages m;
        live->take(64, engine.sr, m);
        std::vector<float> l(4096), r(4096);
        float* sig[2] = {l.data(), r.data()};
        for (int i = 0; i < 6; i++) live->addVoices(sig, 2, 4096, engine.sr);
        return live->bend();
    }
    void play(const Event& e) { in.handle(e); }
    void noteOn(int pitch, int port = 0) {
        Event e = Event::noteOn(pitch, 100);
        e.port = (int16_t)port;
        play(e);
    }
    void noteOff(int pitch, int port = 0) {
        Event e = Event::noteOff(pitch);
        e.port = (int16_t)port;
        play(e);
    }
    void control(int cc, int v, int port = 0) {
        Event e = Event::control(cc, v);
        e.port = (int16_t)port;
        play(e);
    }
    void bendTo(int value, int port = 0) {
        Event e;
        e.type = Event::PitchBend;
        e.bend = (int16_t)value;
        e.port = (int16_t)port;
        play(e);
    }
};

TrackPtr midiTrack(MainWindow& w) {
    TrackPtr t = w.timeline->addTrack(Kind::Midi);
    t->needsInstrument = false;  // (the built-in synth: it can play)
    return t;
}

bool lit(MainWindow& w, const TrackPtr& t) {
    auto it = w.panel->focusButtons.find(t);
    return it != w.panel->focusButtons.end() && it->second->isChecked();
}

std::vector<TrackPtr> plays(MainWindow& w) { return midi_input::keyboardTracks(w.project, w.timeline->currentTrack); }

void lockFocus(MainWindow& w, bool on) {
    ui::Menu m;
    w.panel->fillFocusMenu(m);
    ui::Action* a = find(m, "Lock keyboard focus");
    CHECK(a && a->checkable && a->checked == w.project.keyFocusLocked);
    if (a && a->toggled) a->toggled(on);
}

void testFollowAndLayer(Rig& r) {
    MainWindow& w = r.w;
    Project& p = w.project;
    TrackPtr audio = w.timeline->addTrack(Kind::Audio);
    TrackPtr m1 = midiTrack(w), m2 = midiTrack(w), m3 = midiTrack(w);
    w.panel->rebuild();
    CHECK(w.panel->focusButtons.size() == 3 && !w.panel->focusButtons.count(audio));
    // by default the focus follows the selection: the selected MIDI track's toggle alone is lit
    w.timeline->setCurrentTrack(m1);
    CHECK(m1->keyFocus && !m2->keyFocus && !m3->keyFocus);
    CHECK(lit(w, m1) && !lit(w, m2) && !lit(w, m3));
    w.timeline->setCurrentTrack(m2);
    CHECK(!m1->keyFocus && m2->keyFocus && lit(w, m2) && !lit(w, m1));
    w.timeline->setCurrentTrack(audio);  // (an audio track: nothing plays, as before)
    CHECK(plays(w).empty() && !lit(w, m1) && !lit(w, m2));
    r.noteOn(60);
    CHECK(!r.held(m1) && !r.held(m2) && !r.held(m3));
    r.noteOff(60);

    // a click on a toggle adds its track: one keyboard, two instruments
    w.timeline->setCurrentTrack(m1);
    w.panel->focusButtons[m3]->setChecked(true);  // (as a click: toggled)
    CHECK(m1->keyFocus && m3->keyFocus && !m2->keyFocus && lit(w, m1) && lit(w, m3) && !lit(w, m2));
    CHECK((plays(w) == std::vector<TrackPtr>{m1, m3}));
    r.noteOn(60);
    CHECK(r.held(m1) && r.held(m3) && !r.held(m2));
    r.noteOff(60);
    CHECK(!r.held(m1) && !r.held(m3));
    // the computer keyboard (port -1) plays the same tracks
    r.noteOn(64, -1);
    CHECK(r.held(m1) && r.held(m3) && !r.held(m2));
    r.noteOff(64, -1);
    CHECK(!r.held(m1) && !r.held(m3));
    // the sustain pedal holds on each; the note is let go where it started, though the focus moved meanwhile
    r.control(64, 127);
    r.noteOn(62);
    r.noteOff(62);
    CHECK(r.held(m1) && r.held(m3));
    w.timeline->setCurrentTrack(m2);  // (unlocked: the focus follows, m2 alone)
    CHECK(!lit(w, m1) && !lit(w, m3) && lit(w, m2));
    r.control(64, 0);
    CHECK(!r.held(m1) && !r.held(m3) && !r.held(m2));
    // the bend wheel bends each lit track; a track no longer lit goes straight
    w.timeline->setCurrentTrack(m1);
    w.panel->toggleFocus(m3, true);
    r.bendTo(8191);
    CHECK(std::fabs(r.bend(m1) - 2.0) < 0.05 && std::fabs(r.bend(m3) - 2.0) < 0.05 && std::fabs(r.bend(m2)) < 1e-6);
    w.panel->toggleFocus(m3, false);
    CHECK(!lit(w, m3) && lit(w, m1));
    r.bendTo(4096);
    CHECK(std::fabs(r.bend(m1) - live::bendSemitones(4096, 2)) < 0.05 && std::fabs(r.bend(m3)) < 0.05);
    r.bendTo(0);
    CHECK(std::fabs(r.bend(m1)) < 0.05);
    // a track listening to a port of its own still plays from it alone, lit or not (as before)
    m2->midiIn = "Some port";
    w.panel->toggleFocus(m2, true);
    r.noteOn(67);  // (port 0: no such port here: its "" name: not m2's)
    CHECK(r.held(m1) && !r.held(m2));
    r.noteOff(67);
    r.noteOn(67, -1);  // (the computer keyboard plays every lit track, whatever its input)
    CHECK(r.held(m1) && r.held(m2));
    r.noteOff(67, -1);
    m2->midiIn.clear();
    w.panel->toggleFocus(m2, false);
    // turning the last lit toggle off: the selected track plays (never none)
    w.panel->toggleFocus(m1, false);
    CHECK((plays(w) == std::vector<TrackPtr>{m1}) && lit(w, m1));
    (void)p;
}

void testLockAndFailsafes(Rig& r, Driver& d) {
    MainWindow& w = r.w;
    Project& p = w.project;
    std::vector<TrackPtr> ms;
    for (auto& t : p.tracks)
        if (t->kind == Kind::Midi) ms.push_back(t);
    TrackPtr m1 = ms[0], m2 = ms[1], m3 = ms[2];
    w.timeline->setCurrentTrack(m1);
    w.panel->toggleFocus(m1, true);
    w.panel->toggleFocus(m3, true);
    // locked: selecting another track leaves the lit set
    lockFocus(w, true);
    CHECK(p.keyFocusLocked && w.panel->focusButtons[m1]->locked);
    w.timeline->setCurrentTrack(m2);
    CHECK(m1->keyFocus && m3->keyFocus && !m2->keyFocus && lit(w, m1) && lit(w, m3) && !lit(w, m2));
    r.noteOn(60);
    CHECK(r.held(m1) && r.held(m3) && !r.held(m2));
    r.noteOff(60);
    // its toggles still change it while locked
    w.panel->focusButtons[m2]->setChecked(true);
    CHECK(lit(w, m2) && m2->keyFocus);
    w.panel->toggleFocus(m2, false);
    // locking keeps what plays: the selected track too, when it plays for want of a lit toggle
    lockFocus(w, false);
    for (auto& t : p.tracks) t->keyFocus = false;
    w.timeline->setCurrentTrack(m2);
    for (auto& t : p.tracks) t->keyFocus = false;  // (none lit: m2 plays as the selected one)
    lockFocus(w, true);
    CHECK(m2->keyFocus);
    w.timeline->setCurrentTrack(m1);
    CHECK((plays(w) == std::vector<TrackPtr>{m2}));
    w.panel->toggleFocus(m1, true);
    w.panel->toggleFocus(m3, true);

    // failsafes: a focused track removed is left out; brought back by undo, it comes back lit
    d.settle();
    w.timeline->removeTrack(m3);
    d.settle();
    CHECK(!p.tracks.contains(m3));
    CHECK((plays(w) == std::vector<TrackPtr>{m2, m1}) || (plays(w) == std::vector<TrackPtr>{m1, m2}));
    r.noteOn(61);
    CHECK(r.held(m1) && r.held(m2));
    r.noteOff(61);
    d.keys({key('Z', plat::ModCtrl)});  // (undo)
    CHECK(p.tracks.contains(m3));
    CHECK(m3->keyFocus && p.keyFocusLocked);  // (undo leaves the focus alone)
    w.panel->rebuild();
    CHECK(lit(w, m3));
    // undo of something else leaves the focus as it is
    w.panel->toggleFocus(m3, false);
    m1->muted = true;
    d.settle();
    d.keys({key('Z', plat::ModCtrl)});
    CHECK(!m1->muted && !m3->keyFocus && m1->keyFocus && m2->keyFocus);
    // a focused track that can't play (no instrument, frozen): left out
    m2->needsInstrument = true;
    CHECK((plays(w) == std::vector<TrackPtr>{m1}));
    // none left (locked): the selected track plays
    m1->needsInstrument = true;
    w.timeline->setCurrentTrack(m3);  // (locked: m3 isn't lit, but it is selected)
    CHECK((plays(w) == std::vector<TrackPtr>{m3}));
    r.noteOn(65);
    CHECK(r.held(m3) && !r.held(m1) && !r.held(m2));
    r.noteOff(65);
    m1->needsInstrument = m2->needsInstrument = false;
    // selecting no track (removed, the project replaced): the focus left as it is
    w.timeline->setCurrentTrack(nullptr);
    lockFocus(w, false);
    w.timeline->setCurrentTrack(nullptr);
    CHECK(m1->keyFocus && m2->keyFocus);
    // duplicating a lit track: the copy isn't lit (it is selected, and the focus follows it alone)
    w.timeline->setCurrentTrack(m1);
    w.panel->duplicate(m1, false);
    TrackPtr copy = w.timeline->currentTrack;
    CHECK(copy && copy != m1 && copy->keyFocus && !m1->keyFocus);
}

void testSaved(Rig& r) {
    MainWindow& w = r.w;
    Project& p = w.project;
    std::vector<TrackPtr> ms;
    for (auto& t : p.tracks)
        if (t->kind == Kind::Midi) ms.push_back(t);
    w.timeline->setCurrentTrack(ms[0]);
    w.panel->toggleFocus(ms[2], true);
    lockFocus(w, true);
    std::vector<std::string> names;
    for (auto& t : plays(w)) names.push_back(t->name);
    CHECK(names.size() == 2);
    std::string path = util::absolutePath(TMP + "/focus.json");
    CHECK(w.saveProjectFile(path));
    // the file has them; opened again (as File → Open), they are back, and the selection doesn't move them
    std::string text;
    CHECK(util::readFile(path, text) && text.find("\"key_focus_lock\"") != std::string::npos &&
          text.find("\"key_focus\"") != std::string::npos);
    CHECK(w.openProjectFile(path));
    ui::App::get().tick();
    CHECK(p.keyFocusLocked);
    std::vector<std::string> again;
    for (auto& t : p.tracks)
        if (t->keyFocus) again.push_back(t->name);
    CHECK(again == names);
    for (auto& t : p.tracks)
        if (t->kind == Kind::Midi && !t->keyFocus) w.timeline->setCurrentTrack(t);
    again.clear();
    for (auto& t : plays(w)) again.push_back(t->name);
    CHECK(again == names);
    // unlocked several lit: kept too (opening selects no track)
    lockFocus(w, false);
    CHECK(w.saveProjectFile(path));
    CHECK(w.openProjectFile(path));
    ui::App::get().tick();
    int focused = 0;
    for (auto& t : p.tracks) focused += t->keyFocus;
    CHECK(!p.keyFocusLocked && focused == 2);
    // an older project (no such keys): nothing lit, unlocked; the selection plays
    // (saved with none: the file as an older version wrote it, without the keys)
    for (auto& t : p.tracks) t->keyFocus = false;
    std::string oldPath = util::absolutePath(TMP + "/old.json");
    CHECK(w.saveProjectFile(oldPath));
    std::string old;
    CHECK(util::readFile(oldPath, old) && old.find("key_focus") == std::string::npos);
    CHECK(w.openProjectFile(oldPath));
    ui::App::get().tick();
    focused = 0;
    for (auto& t : p.tracks) focused += t->keyFocus;
    CHECK(!p.keyFocusLocked && focused == 0);
    TrackPtr first;
    for (auto& t : p.tracks)
        if (t->kind == Kind::Midi && !first) first = t;
    w.timeline->setCurrentTrack(first);
    CHECK(first && first->keyFocus && (plays(w) == std::vector<TrackPtr>{first}));
    // a damaged value (not a boolean): read as off
    std::string odd = text;
    size_t at = odd.find("\"key_focus_lock\"");
    at = at == std::string::npos ? at : odd.find("true", at);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) odd.replace(at, 4, "\"yes\"");
    CHECK(util::writeFile(oldPath, odd) && w.openProjectFile(oldPath));
    CHECK(!p.keyFocusLocked);
}

void testTerminal(Rig& r, Front& f, Driver& d) {
    MainWindow& w = r.w;
    Project& p = w.project;
    while (f.pane != Front::Pane::Panel) d.keys({key(plat::Key_Tab)});
    std::vector<TrackPtr> ms;
    TrackPtr audio;
    for (auto& t : p.tracks) {
        if (t->kind == Kind::Midi) ms.push_back(t);
        if (t->kind == Kind::Audio && !audio) audio = t;
    }
    CHECK(ms.size() >= 3);
    for (auto& t : ms) t->needsInstrument = false;
    w.panel->rebuild();
    f.setCurrent(ms[0]);
    d.settle();
    CHECK(ms[0]->keyFocus && d.shows("♪"));
    // F2 on a MIDI track: Lock keyboard focus; chosen, the title says so
    d.keys({key(plat::Key_F2)});
    CHECK(f.menuOpen());
    ui::Menu* menu = f.open.empty() ? nullptr : f.open[0].menu;
    int item = -1;
    if (menu)
        for (int i = 0; i < (int)menu->actions().size(); i++)
            if (menu->actions()[(size_t)i]->text == "Lock keyboard focus") item = i;
    CHECK(item >= 0);
    for (int i = 0; i < 60 && f.menuOpen() && f.open[0].item != item; i++) d.keys({key(plat::Key_Down)});
    d.keys({key(plat::Key_Return)});
    CHECK(p.keyFocusLocked && d.shows("Tracks  ♪ locked"));
    // Shift+K on another MIDI track: it plays too; again: no more
    f.setCurrent(ms[1]);
    d.settle();
    CHECK(!ms[1]->keyFocus);
    d.keys({key('K', plat::ModShift, "K")});
    CHECK(ms[1]->keyFocus && ms[0]->keyFocus);
    r.noteOn(60);
    CHECK(r.held(ms[0]) && r.held(ms[1]));
    r.noteOff(60);
    d.keys({key('K', plat::ModShift, "K")});
    CHECK(!ms[1]->keyFocus && ms[0]->keyFocus);
    // on an audio track: said, nothing changed
    if (audio) {
        f.setCurrent(audio);
        d.keys({key('K', plat::ModShift, "K")});
        CHECK(!audio->keyFocus && d.shows("Only MIDI tracks"));
    }
    // K alone still shows takes (unchanged); the keys' list has Shift+K
    d.keys({key(plat::Key_F1)});
    for (int i = 0; i < 200 && !d.shows("Shift+K           the keyboards play"); i++) d.keys({key(plat::Key_Down)});
    CHECK(d.shows("Shift+K           the keyboards play"));
    d.keys({key(plat::Key_Escape)});
    lockFocus(w, false);
    std::ofstream(OUT + "/terminal.txt") << d.screen.lines();
}

// pictures of the panel's toggles (lit, unlit, locked) in every skin, each checked: the socket is drawn in a colour
// apart from the button's face, lit or not
void testPictures(Rig& r) {
    MainWindow& w = r.w;
    Project& p = w.project;
    std::vector<TrackPtr> ms;
    for (auto& t : p.tracks)
        if (t->kind == Kind::Midi) ms.push_back(t);
    for (auto& t : ms) t->needsInstrument = false;
    w.timeline->setCurrentTrack(ms[0]);
    w.panel->toggleFocus(ms[1], true);
    skin::setBuiltinDir("data/skins");
    skin::setUserDir(TMP + "/skins");
    auto contrast = [](::Color a, ::Color b) {
        auto lum = [](::Color c) {
            auto ch = [](int v) {
                double s = v / 255.0;
                return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * ch(c.r) + 0.7152 * ch(c.g) + 0.0722 * ch(c.b);
        };
        double x = lum(a), y = lum(b);
        return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
    };
    int skins = 0;
    auto shoot = [&](const std::string& name) {
        for (int locked = 0; locked < 2; locked++) {
            lockFocus(w, locked != 0);
            w.panel->rebuild();
            ui::App::get().collectGarbage();
            ui::App::get().repaintAll();
            ui::App::get().doLayouts();
            savePng(OUT + "/panel-" + name + (locked ? "-locked" : "") + ".png", paintWidget(w.panel));
            for (TrackPtr t : {ms[0], ms[2]}) {  // (lit, unlit)
                KeyFocusButton* b = w.panel->focusButtons[t];
                CHECK(b && b->width() > 0 && b->height() > 0);
                if (!b) continue;
                Image img = paintWidget(b);
                if (!locked) savePng(OUT + "/toggle-" + name + (b->isChecked() ? "-lit" : "-unlit") + ".png", img);
                // the face (a corner pixel inside) and the socket's ring (its farthest colour from the face)
                ::Color face = img.colorAt(img.width() / 2, 3);
                double best = 1;
                for (int y = 0; y < img.height(); y++)
                    for (int x = 0; x < img.width(); x++) best = std::max(best, contrast(face, img.colorAt(x, y)));
                if (best < 3.0)
                    std::printf("  %s: the %s toggle's socket at %.2f:1 against its face\n", name.c_str(),
                                b->isChecked() ? "lit" : "unlit", best);
                CHECK(best >= 3.0);
            }
        }
        skins++;
    };
    shoot("default");
    for (const skin::Skin& sk : skin::library()) {
        skin::apply(sk);
        w.restyle();
        std::string name = sk.name;
        for (auto& c : name)
            if (c == ' ') c = '-';
        shoot(name);
    }
    std::printf("keyboard focus toggles drawn in %d skins\n", skins);
    CHECK(skins >= 9);
    theme::setSkin();
    w.restyle();
    lockFocus(w, false);
}

// --- the worst case: 64 MIDI tracks all lit, notes hammered while toggling focus, lock and selection ---
void testWorst(Rig& r) {
    MainWindow& w = r.w;
    Project& p = w.project;
    std::vector<TrackPtr> ms;
    r.sounding.clear();  // (tracks of the projects opened before are gone: their addresses may be taken again)
    for (int i = 0; i < 64; i++) ms.push_back(midiTrack(w));
    w.panel->rebuild();
    w.timeline->setCurrentTrack(ms[0]);
    for (auto& t : ms) w.panel->toggleFocus(t, true);
    CHECK(plays(w).size() >= 64);
    r.noteOn(48);
    int heldOn = 0;
    for (auto& t : ms) heldOn += r.held(t);
    int act = 0;
    for (auto& t : ms) act += r.engine.liveNotes(t.get())->active();
    auto tg = r.in.targets(0, 0);
    if (heldOn != 64) std::printf("  held on %d of 64 (plays %zu, targets %zu, active %d)\n", heldOn, plays(w).size(), tg.size(), act);
    CHECK(heldOn == 64);
    r.noteOff(48);
    std::mt19937 rng(11);
    double t0 = plat::now();
    for (int i = 0; i < 4000; i++) {
        int what = (int)(rng() % 10);
        int pitch = 36 + (int)(rng() % 48);
        int port = (int)(rng() % 2) - 1;
        if (what < 4) r.noteOn(pitch, port);
        else if (what < 7) r.noteOff(pitch, port);
        else if (what == 7) w.panel->toggleFocus(ms[rng() % ms.size()], rng() % 2);
        else if (what == 8) w.timeline->setCurrentTrack(ms[rng() % ms.size()]);
        else if (rng() % 8 == 0) w.panel->setFocusLocked(!p.keyFocusLocked);
        else r.control(64, rng() % 2 ? 127 : 0, port);
        if (i % 256 == 0)
            for (auto& t : ms) r.held(t);  // (the hand-over rings emptied, as the audio thread would)
    }
    double took = plat::now() - t0;
    r.in.releaseAll();
    int still = 0;
    for (auto& t : ms) still += r.held(t);
    CHECK(still == 0);  // (every note let go where it started)
    std::printf("worst case: 4000 events over 64 MIDI tracks in %.3f s, no note left hanging\n", took);
    for (auto& t : ms) w.timeline->removeTrack(t);
    r.noteOn(50);  // (the focused tracks gone: the selection, or nothing; no crash)
    r.noteOff(50);
}

}  // namespace

int main() {
    util::setDebug(false);
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    util::removeFile(TMP + "/settings.json");
    settings::setPath(TMP + "/settings.json");
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    Driver d{w, &f};
    app.nestedLoop = [&d](const std::function<bool()>& done) {
        while (!done()) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            d.f->key(Key{plat::Key_Escape});  // (no dialog is expected: closed)
        }
    };
    d.settle();
    Rig r{*w, w->engine, *w->midiInput};
    w->engine.close();  // (no audio thread: the test takes what is played, as it would, alone)
    testFollowAndLayer(r);
    testLockAndFailsafes(r, d);
    testSaved(r);
    testTerminal(r, f, d);
    testPictures(r);
    testWorst(r);
    TEST_MAIN_END();
}
