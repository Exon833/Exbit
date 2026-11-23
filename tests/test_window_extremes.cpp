// Every dialog and separate window at its size extremes (headless): each opened at interface sizes 75 %, 100 %,
// 150 % and 200 %, on screens of 1920x1080, 3840x2160, 1024x600 and 800x480; at its default size, its smallest,
// smaller still (as a window manager may force it), maximised, and back to its default. What a user would see as
// broken is checked: windows opening off the screen or bigger than it, OK / Cancel / Close pushed out of reach,
// widgets outside their parents or overlapping, text cut without "…", widgets given less than their minimum,
// negative or absurd geometry, a layout that isn't the same after shrinking and growing back; menus taller than
// the screen (they must fit and scroll), tooltips staying on the screen. What it fixed is CHECKed (fails); the
// rest is reported: build/ui-samples/window-audit/report.md, pictures beside it.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "core/util.h"
#include "effects/effects.h"
#include "gfx/image.h"
#include "layout_audit.h"
#include "model/model.h"
#include "paths.h"
#include "platform/platform.h"
#include "plugins/library.h"
#include "tui/app.h"
#include "ui/color_picker.h"
#include "ui/lane_panel.h"
#include "ui/main_window.h"
#include "ui/midi_effects_dialog.h"
#include "ui/pad_kit_dialog.h"
#include "ui/plugin_chooser.h"
#include "ui/plugin_host.h"
#include "ui/sampler_dialog.h"
#include "ui/skin.h"
#include "ui/synth_dialog.h"
#include "ui/tempo_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

using namespace ui;

namespace {

const std::string OUT = "build/ui-samples/window-audit";
const int SR = 48000;
audit::Report rep;
std::vector<std::string> notes;
auto started = std::chrono::steady_clock::now();
double elapsed() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); }

std::string fileSafe(std::string s) {
    for (auto& c : s)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '-';
    return s.substr(0, 80);
}
std::string sz(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }
bool inside(const Rect& r, const Rect& scr) { Rect i = r.intersected(scr); return i.w == r.w && i.h == r.h; }

void settle(int frames = 2) {
    for (int i = 0; i < frames; i++) {
        App::get().advanceTimers(0.02);
        App::get().tick();
    }
    App::get().doLayouts();
}

// --- the contexts: interface size × physical screen ---
struct Ctx {
    int percent, pw, ph;
    Rect screen() const { return Rect(0, 0, (int)(pw * 100 / percent), (int)(ph * 100 / percent)); }  // logical px
    std::string name() const { return std::to_string(percent) + "% " + sz(pw, ph); }
    bool full() const { return pw == 1920; }  // the size sweep (smallest, smaller, maximised, back)
    // (pictures take most of the time: a few contexts, every one with TINYDAW_EXTREMES_PICTURES=all)
    bool pictures() const {
        static bool all = getenv("TINYDAW_EXTREMES_PICTURES") != nullptr;
        return all || (percent == 100 && pw == 1920) || (percent == 200 && pw == 800) || (percent == 150 && pw == 1024);
    }
};

// --- the project ---
AudioPtr sine(double seconds, double hz) {
    auto a = std::make_shared<AudioBuffer>();
    int64_t n = (int64_t)(seconds * SR);
    a->samples.resize(n * 2);
    for (int64_t i = 0; i < n; ++i) a->samples[i * 2] = a->samples[i * 2 + 1] = (float)(0.5 * std::sin(2 * M_PI * hz * i / SR));
    return a;
}
struct Rig {
    MainWindow* w = nullptr;
    std::vector<TrackPtr> tracks;
    ClipPtr midi, audio;
};
Rig makeRig(const Rect& scr) {
    Rig r;
    r.w = new MainWindow();
    r.w->askBeforeClosing = false;
    r.w->resizeWindow(scr.w, scr.h);  // (maximised)
    r.w->moveWindow(0, 0);
    r.w->showWindow();
    Project& p = r.w->project;
    const char* names[6] = {"Drums", "Bass — a long name for the track that plays the bass line", "Keys", "Lead",
                            "Drawn", "Pads"};
    for (int i = 0; i < 6; i++) {
        TrackPtr t = r.w->timeline->addTrack(i < 2 ? Kind::Audio : Kind::Midi);
        t->name = names[i];
        r.tracks.push_back(t);
        ClipPtr c;
        if (i < 2) {
            c = makeAudioClip(std::string(names[i]) + " 1", sine(4, 220 + 50 * i), SR, SR);
        } else {
            std::vector<Note> ns;
            for (int k = 0; k < 16; ++k) ns.push_back({k * 0.25, k * 0.25 + 0.2, 60.0 + (k * 5) % 14, 100});
            c = makeMidiClip(std::string(names[i]) + " 1", ns, 4, SR);
        }
        c->track = t;
        c->start = (int64_t)(i * 0.5 * SR);
        p.addClip(c);
        if (i < 2 && !r.audio) r.audio = c;
        if (i >= 2 && !r.midi) r.midi = c;
    }
    r.tracks[3]->sampler = sampler_dialog::newSettings(r.audio);
    r.tracks[4]->synth = std::make_shared<const subsynth::Patch>(subsynth::drawnPatch());
    if (!r.tracks[5]->padKit) r.tracks[5]->padKit = std::make_shared<PadKit>();
    r.w->timeline->lanesChanged();
    r.w->timeline->tracksChanged.emit();
    settle();
    return r;
}

std::vector<Window*> shownOthers(Window* main) {
    std::vector<Window*> out;
    for (Window* w : App::get().windows())
        if (w != main && w->isWindowShown() && !w->isPopup() && w != App::get().toolTipWindow()) out.push_back(w);
    return out;
}

void closeRig(Rig& r) {
    App::get().nestedLoop = nullptr;
    r.w->closeFxWindows();
    App::get().closeAllPopups();
    for (Window* win : shownOthers(r.w)) win->hideWindow();
    delete r.w;
    r.w = nullptr;
    settle(1);
    App::get().collectGarbage();
}

Point windowMinimum(Window& w) {
    App::get().doLayouts();
    Point m = w.minimumSize();
    if (w.layout()) {
        Point lm = w.layout()->minimumSize();
        m = Point(std::max(m.x, lm.x), std::max(m.y, lm.y));
    }
    return m;
}

// --- the checks ---
// every widget's geometry sane: no negative size, nothing absurdly far (a NaN or overflow shows as that)
void checkSane(Widget* w, const std::string& where, int& bad) {
    for (auto& up : w->children()) {
        Widget* c = up.get();
        if (!c->isVisible()) continue;
        Rect g = c->geometry();
        if (g.w < 0 || g.h < 0 || std::abs(g.x) > 100000 || std::abs(g.y) > 100000 || g.w > 100000 || g.h > 100000) {
            if (bad++ < 3)
                rep.add("insane-geometry", audit::pathOf(c), audit::describe(c), where + ": " + audit::rectText(g));
        }
        checkSane(c, where, bad);
    }
}

bool isDialogButton(const Button* b) {
    for (const Widget* p = b->parent(); p; p = p->parent())
        if (dynamic_cast<const ButtonBox*>(p)) return true;
    static const std::set<std::string> words{"OK", "Cancel", "Close", "Apply", "Done", "Save", "Yes", "No", "Don't Save"};
    return words.count(b->text()) > 0;
}

// OK / Cancel / Close: inside the window, and the window's part of them on the screen
int buttonsOffScreen(Window& win, const Rect& scr, const std::string& where) {
    int n = 0;
    std::function<void(Widget*)> walk = [&](Widget* w) {
        for (auto& up : w->children()) {
            Widget* c = up.get();
            if (!c->isVisible() || c->inFrame()) continue;
            if (auto* b = dynamic_cast<Button*>(c); b && isDialogButton(b)) {
                // (a window smaller than its minimum scrolls: the button scrolled into view, then back)
                Point was = win.scrollOffset();
                if (win.overflows()) {
                    win.ensureVisible(b);
                    App::get().doLayouts();
                }
                Point at = b->mapToWindow(Point(0, 0));
                win.scrollTo(was);
                App::get().doLayouts();
                Rect inWin(at.x, at.y, b->width(), b->height());
                Rect sg = win.screenGeometry();
                Rect onScr(sg.x + at.x, sg.y + at.y, b->width(), b->height());
                if (!inside(inWin, win.rect()) || !inside(onScr, scr) || b->width() <= 0 || b->height() <= 0) {
                    n++;
                    rep.add("button-unreachable", audit::pathOf(b), audit::describe(b),
                            where + ": at " + audit::rectText(inWin) + " in a window of " + sz(win.width(), win.height()) +
                                " placed at " + audit::rectText(sg) + " on a screen of " + audit::rectText(scr),
                            "button|" + win.type() + "|" + b->text());
                }
            }
            walk(c);
        }
    };
    walk(&win);
    return n;
}

audit::Options treeOptions() {
    audit::Options o;
    return o;
}

std::string picture(Window& w, const std::string& name) {
    App::get().doLayouts();
    w.invalidate(w.outerRect());
    w.flush();
    std::string path = OUT + "/" + fileSafe(name) + ".png";
    savePng(path, w.framebuffer());
    return path;
}

int windowsSeen = 0, failedButtons = 0, failedOffScreen = 0, failedSane = 0;
std::map<std::string, std::string> titles;  // name → what it is

void sweepWindow(Window& win, const std::string& name, const Ctx& ctx) {
    App& app = App::get();
    app.doLayouts();
    windowsSeen++;
    titles[name] = win.type() + " \"" + win.title() + "\"";
    Rect scr = ctx.screen();
    std::string where = name + " @ " + ctx.name();
    rep.context = where;
    rep.picture = "";
    Point def(win.width(), win.height());
    // opened: on the screen, not bigger than it
    Rect o = win.outerScreenGeometry();
    bool pics = ctx.pictures();
    std::string pic;
    if (!inside(o, scr)) {
        pic = picture(win, where + " default");
        rep.picture = pic;
        bool bigger = o.w > scr.w || o.h > scr.h;
        rep.add(bigger ? "bigger-than-screen" : "off-screen", win.type(), win.type() + " \"" + win.title() + "\"",
                where + ": opens at " + audit::rectText(o) + " on a screen of " + sz(scr.w, scr.h) +
                    (bigger ? " (its smallest: " + sz(windowMinimum(win).x, windowMinimum(win).y) + ")" : ""),
                std::string(bigger ? "big|" : "off|") + name);
        failedOffScreen++;
    } else if (pics) {
        pic = picture(win, where + " default");
    }
    rep.picture = pic;
    int before = rep.size();
    audit::checkTree(&win, rep, treeOptions());
    failedButtons += buttonsOffScreen(win, scr, where + " default");
    int bad = 0;
    checkSane(&win, where + " default", bad);
    failedSane += bad;
    if (rep.size() > before && pic.empty()) picture(win, where + " default");
    std::map<Widget*, Rect> first;
    win.scrollTo(Point(0, 0));  // (compared unscrolled)
    app.doLayouts();
    audit::geometries(&win, first);
    if (!win.isResizable()) return;
    // maximised (the whole screen)
    win.moveWindow(scr.x, scr.y);
    win.resizeWindow(scr.w, scr.h);
    settle(1);
    rep.context = where + " maximised";
    rep.picture = pics ? picture(win, where + " maximised") : "";
    audit::checkTree(&win, rep, treeOptions());
    failedButtons += buttonsOffScreen(win, scr, where + " maximised");
    bad = 0;
    checkSane(&win, where + " maximised", bad);
    failedSane += bad;
    if (ctx.full()) {
        // its smallest
        Point mn = windowMinimum(win);
        mn = Point(std::max(1, mn.x), std::max(1, mn.y));
        win.resizeWindow(mn.x, mn.y);
        settle(1);
        rep.context = where + " smallest " + sz(mn.x, mn.y);
        rep.picture = pics ? picture(win, where + " smallest") : "";
        audit::checkTree(&win, rep, treeOptions());
        failedButtons += buttonsOffScreen(win, Rect(-100000, -100000, 200000, 200000), where + " smallest");
        bad = 0;
        checkSane(&win, where + " smallest", bad);
        failedSane += bad;
        // smaller than it allows (a window manager may): nothing broken, only cut
        for (Point s : {Point(mn.x / 2, mn.y / 2), Point(1, 1), Point(mn.x, 1), Point(1, mn.y)}) {
            win.resizeWindow(s.x, s.y);
            settle(1);
            bad = 0;
            checkSane(&win, where + " forced to " + sz(s.x, s.y), bad);
            failedSane += bad;
            win.invalidate(win.outerRect());
            win.flush();
        }
    }
    // back to its default: as it was?
    win.resizeWindow(def.x, def.y);
    settle(1);
    std::map<Widget*, Rect> back;
    win.scrollTo(Point(0, 0));
    app.doLayouts();
    audit::geometries(&win, back);
    rep.context = where + " back to default";
    audit::compareGeometries(first, back, rep, "shrunk / maximised and back to " + sz(def.x, def.y));
}

// --- the windows: each opened (a modal one checked inside its loop), swept, closed ---
struct Opener {
    std::string name;
    std::function<void(Rig&)> open;
};

void openAndSweep(Rig& r, const Opener& op, const Ctx& ctx) {
    App& app = App::get();
    std::vector<Window*> before = shownOthers(r.w);
    std::set<Window*> done;
    app.nestedLoop = [&](const std::function<bool()>& finished) {
        settle(1);
        for (int i = 0; i < 6 && !finished(); i++) {
            Window* m = app.modalWindow();
            if (!m) {
                app.closeAllPopups();
                app.advanceTimers(0.05);
                continue;
            }
            if (!done.count(m)) {
                done.insert(m);
                sweepWindow(*m, op.name + (done.size() > 1 ? " › " + m->title() : ""), ctx);
            }
            m->close();
            settle(1);
        }
    };
    op.open(r);
    app.nestedLoop = nullptr;
    settle(2);
    for (Window* win : shownOthers(r.w))
        if (std::find(before.begin(), before.end(), win) == before.end() && !done.count(win)) {
            done.insert(win);
            sweepWindow(*win, op.name + (done.size() > 1 ? " › " + win->title() : ""), ctx);
            win->close();
            if (win->isWindowShown()) win->hideWindow();
        }
    app.closeAllPopups();
    settle(1);
    if (done.empty()) notes.push_back(op.name + " @ " + ctx.name() + ": no window opened");
}

// every window that stays open (not the modal ones) opened at once, then all of them resized together, to their
// smallest, to 1x1, maximised and back, over and over: nothing breaks, every one keeps sane geometry
void allAtOnce(Rig& r, const std::vector<Opener>& ops, const Ctx& ctx) {
    App& app = App::get();
    app.nestedLoop = [&](const std::function<bool()>& finished) {
        for (int i = 0; i < 6 && !finished(); i++)
            if (Window* m = app.modalWindow()) m->close();
            else app.closeAllPopups();
    };
    for (const Opener& op : ops) op.open(r);
    app.nestedLoop = nullptr;
    settle(2);
    std::vector<Window*> open = shownOthers(r.w);
    Rect scr = ctx.screen();
    for (int round = 0; round < 3; round++)
        for (Point s : {Point(1, 1), Point(-1, -1), Point(scr.w, scr.h), Point(scr.w * 2, 7), Point(0, 0)}) {
            for (Window* w : open) {
                if (!Widget::alive(w) || !w->isWindowShown()) continue;
                Point mn = windowMinimum(*w);
                Point to = s.x == -1 ? mn : s.x == 0 ? Point(400, 300) : s;
                w->resizeWindow(to.x, to.y);
            }
            settle(1);
            for (Window* w : open) {
                if (!Widget::alive(w) || !w->isWindowShown()) continue;
                int bad = 0;
                checkSane(w, "all at once @ " + ctx.name(), bad);
                CHECK(bad == 0);
                w->invalidate(w->outerRect());
                w->flush();
            }
        }
    picture(*r.w, "all at once " + ctx.name());
    notes.push_back("all at once @ " + ctx.name() + ": " + std::to_string(open.size()) + " windows open together");
    for (Window* w : shownOthers(r.w)) {
        w->close();
        if (Widget::alive(w) && w->isWindowShown()) w->hideWindow();
    }
    settle(1);
}

// a window smaller than its minimum scrolls: the wheel where nothing takes it, the keyboard focus kept in view,
// a wrapped label's lines at its width counted in the minimum
void checkScrolling(Rig& r, const Ctx& ctx) {
    App& app = App::get();
    synth_dialog::openSynth(r.tracks[2]);
    settle(2);
    Window* synth = nullptr;
    for (Window* w : shownOthers(r.w))
        if (w->type() == "SynthDialog") synth = w;
    CHECK(synth != nullptr);
    if (!synth) return;
    Rect scr = ctx.screen();
    CHECK(inside(synth->outerScreenGeometry(), scr));
    Point mn = windowMinimum(*synth);
    synth->resizeWindow(mn.x / 2, mn.y / 2);
    settle(1);
    CHECK(synth->overflows());
    // the layout isn't squashed: its widgets keep at least their minimum
    for (auto& c : synth->children())
        if (c->isVisible() && c->layout()) {
            Point m = c->minimumSizeHint();
            CHECK(c->width() >= m.x && c->height() >= m.y);
        }
    // the wheel over its background scrolls it, down and back up
    plat::Event e;
    e.type = plat::Event::Wheel;
    e.x = 2;
    e.y = synth->height() / 2;
    e.dy = -1;
    synth->dispatchWheel(e);
    settle(1);
    CHECK(synth->scrollOffset().y > 0);
    e.dy = 10;
    synth->dispatchWheel(e);
    settle(1);
    CHECK(synth->scrollOffset().y == 0);
    // Tab through it: the focus always shown (inside the window)
    for (int i = 0; i < 40; i++) {
        synth->focusNext(true);
        settle(1);
        Widget* f = synth->focusWidget();
        if (!f || f == synth) continue;
        Point at = f->mapToWindow(Point(0, 0));
        bool shown = at.x + std::min(f->width(), synth->width()) <= synth->width() + 1 && at.x >= -1 &&
                     at.y + std::min(f->height(), synth->height()) <= synth->height() + 1 && at.y >= -1;
        CHECK(shown);
        if (!shown) printf("focus out of view: %s at %d,%d in %dx%d\n", audit::describe(f).c_str(), at.x, at.y,
                           synth->width(), synth->height());
    }
    // grown back: no scrolling left, nothing moved
    synth->resizeWindow(mn.x, mn.y);
    settle(1);
    CHECK(!synth->overflows());
    CHECK(synth->scrollOffset().x == 0 && synth->scrollOffset().y == 0);
    synth->close();
    settle(1);
    // a dialog with a long wrapped text: narrower, its minimum height grows with the lines
    Dialog d(r.w, "Wrapped");
    auto* lay = vbox(&d, 12, 8);
    std::string text;
    for (int i = 0; i < 20; i++) text += "a sentence that wraps ";
    auto* label = d.add<Label>(text);
    label->setWordWrap(true);
    lay->addWidget(label);
    auto* box = d.add<ButtonBox>(ButtonBox::Ok);
    lay->addWidget(box);
    d.open();
    settle(2);
    d.resizeWindow(500, 400);
    settle(2);
    int wide = d.layout()->minimumSize().y;
    d.resizeWindow(120, 400);
    settle(2);
    int narrow = d.layout()->minimumSize().y;
    CHECK(narrow > wide);
    // the OK button below the text, inside the window or reached by scrolling to it
    d.ensureVisible(box->button(ButtonBox::Ok));
    settle(1);
    Point at = box->mapToWindow(Point(0, 0));
    CHECK(at.y >= 0 && at.y + box->height() <= d.height());
    CHECK(label->mapToWindow(Point(0, label->height())).y <= at.y);
    d.reject();
    settle(1);
    (void)app;
}

// actions never triggered: they leave, run other programs, scan, change the user's files or take long
bool unsafe(const std::string& t) {
    for (const char* s : {"Quit", "Exit", "Edit this skin", "skins folder", "Install", "pdate", "Rescan", "Scan", "Report",
                          "ebsite", "Open recent", "Restart", "Crash", "Delete", "Remove", "Clear", "Freeze", "Bounce",
                          "Open a backup", "Collect", "archive", "New Wine", "Wine prefix", "Wine program"})
        if (t.find(s) != std::string::npos) return true;
    return false;
}

void menuOpeners(Menu& m, const std::string& path, int depth, std::vector<Opener>& out, std::set<std::string>& seen) {
    if (depth > 3) return;
    if (m.aboutToShow) m.aboutToShow(&m);
    for (auto& a : m.actions()) {
        if (a->separator || a->section || !a->visible) continue;
        std::string t = tui::menuText(a->text);
        if (a->submenu) {
            menuOpeners(*a->submenu, path + "/" + t, depth + 1, out, seen);
            continue;
        }
        if (!a->enabled || unsafe(t) || t.find("…") == std::string::npos) continue;
        std::string name = path + "/" + t;
        if (seen.count(name)) continue;
        seen.insert(name);
        out.push_back({name, nullptr});
    }
}

// the menu entry by its path (found again in the new rig's menus)
Action* findAction(Menu& m, const std::string& path, const std::string& prefix) {
    if (m.aboutToShow) m.aboutToShow(&m);
    for (auto& a : m.actions()) {
        if (a->separator || a->section || !a->visible) continue;
        std::string here = prefix + "/" + tui::menuText(a->text);
        if (a->submenu) {
            if (Action* x = findAction(*a->submenu, path, here)) return x;
        } else if (here == path) {
            return a.get();
        }
    }
    return nullptr;
}

std::vector<Opener> allOpeners(Rig& r) {
    std::vector<Opener> ops = {
        {"MIDI editor", [](Rig& r) { r.w->openMidiEditor(r.midi); }},
        {"Effects patch panel", [](Rig& r) { r.w->openTrackPlugins(r.tracks[0]); }},
        {"Master effects", [](Rig& r) { r.w->openMasterPlugins(); }},
        {"Instrument patch panel", [](Rig& r) { r.w->openInstrumentPanel(r.tracks[2]); }},
        {"Keyboard shortcuts", [](Rig& r) { r.w->showShortcuts(); }},
        {"Tempo and time signature", [](Rig& r) { TempoDialog(r.w, 5, 132.5, TimeSig{7, 8}, true, 0, true, true, true).run(); }},
        {"Tempo inside a bar", [](Rig& r) { TempoDialog(r.w, 5, 90, TimeSig{4, 4}, true, 1.5, false, false).run(); }},
        {"Time signature", [](Rig& r) { SignatureDialog(r.w, 9, TimeSig{12, 16}, true, true).run(); }},
        {"Tempo lane range", [](Rig& r) { TempoRangeDialog(r.w, 80, 160, false).run(); }},
        {"Command palette", [](Rig& r) { r.w->showPalette(); }},
        {"History", [](Rig& r) { r.w->showHistory(); }},
        {"MIDI ports", [](Rig& r) { r.w->showMidiPorts(); }},
        {"Metronome", [](Rig& r) { r.w->showMetronomeSettings(); }},
        {"Recording offset", [](Rig& r) { r.w->showRecordOffset(); }},
        {"Audio properties", [](Rig& r) { r.w->showAudioProperties(); }},
        {"Synth", [](Rig& r) { synth_dialog::openSynth(r.tracks[2]); }},
        {"Drawn synth", [](Rig& r) { synth_dialog::openSynth(r.tracks[4]); }},
        {"Sampler", [](Rig& r) { sampler_dialog::openSampler(r.tracks[3]); }},
        {"Pad Kit", [](Rig& r) { pad_kit_dialog::openPadKit(r.tracks[5]); }},
        {"MIDI effects", [](Rig& r) { midi_effects_dialog::openMidiEffects(r.tracks[2]); }},
        {"Slice window", [](Rig& r) { r.w->timeline->sliceToPadKit(r.audio); }},
        {"Warp", [](Rig& r) {
             r.w->timeline->selection.selectOnly(r.audio);
             r.w->timeline->warpSelected(r.audio->start + SR);
         }},
        {"Colour picker", [](Rig& r) {
             auto* d = new ColorPickerDialog(r.w, "#3366cc", "", ColorSubject::Track);
             d->deleteOnClose = true;
             d->open();
         }},
        {"Plugin chooser", [](Rig& r) {
             std::vector<plug::PluginInfo> choices;
             for (auto& e : effects::list()) choices.push_back(plug::PluginInfo{e.path(), e.name, plug::EFFECT, ""});
             plugin_chooser::choosePlugin(r.w, choices);
         }},
        {"Message box (short)", [](Rig& r) { msg::information(r.w, "Saved", "The project was saved."); }},
        {"Message box (long)", [](Rig& r) {
             std::string s;
             for (int i = 0; i < 60; i++) s += "the disk was full and ";
             msg::box(r.w, msg::Icon::Warning, "Warning", "The project couldn't be saved because " + s, msg::Ok | msg::Cancel, msg::Ok);
         }},
        {"Message box (many lines)", [](Rig& r) {
             std::string s;
             for (int i = 0; i < 80; i++) s += "Missing: sample " + std::to_string(i) + ".wav\n";
             msg::warning(r.w, "Missing samples", s);
         }},
        {"Text input", [](Rig& r) { std::string t = "Bass"; getText(r.w, "Rename track", "Name:", t); }},
        {"File dialog (open)", [](Rig& r) { files::openFile(r.w, "Open a project", util::absolutePath(OUT), "Projects (*.tdaw)"); }},
        {"File dialog (save)", [](Rig& r) { files::saveFile(r.w, "Save as", util::absolutePath(OUT), "Projects (*.tdaw)"); }},
        {"File dialog (folder)", [](Rig& r) { files::folder(r.w, "Choose a folder", util::absolutePath(OUT)); }},
    };
    // every built-in effect's window and the modulators'
    for (auto& e : effects::list()) {
        plug::PluginInfo info{e.path(), e.name, plug::EFFECT, ""};
        ops.push_back({"Effect " + e.name, [info](Rig& r) {
                           SlotPtr slot = r.w->plugins->load(info);
                           if (slot) r.w->plugins->edit(slot);
                       }});
    }
    // the menu bar's, the instrument menu's, a track's and a clip's entries that open a window ("…")
    std::vector<Opener> fromMenus;
    std::set<std::string> seen;
    for (auto& m : r.w->menuBar->menus()) menuOpeners(*m, "Menu " + tui::menuText(m->title), 0, fromMenus, seen);
    {
        Menu menu;
        r.w->fillInstrumentMenu(menu, r.tracks[2]);
        menuOpeners(menu, "Instrument menu", 0, fromMenus, seen);
    }
    {
        Menu menu;
        r.w->panel->fillTrackMenu(menu, r.tracks[2]);
        menuOpeners(menu, "Track menu", 0, fromMenus, seen);
    }
    for (Opener& op : fromMenus) {
        std::string path = op.name;
        op.open = [path](Rig& r) {
            Action* a = nullptr;
            for (auto& m : r.w->menuBar->menus())
                if (!a) a = findAction(*m, path, "Menu " + tui::menuText(m->title));
            Menu inst, track;
            if (!a && path.rfind("Instrument menu", 0) == 0) {
                r.w->fillInstrumentMenu(inst, r.tracks[2]);
                a = findAction(inst, path, "Instrument menu");
            }
            if (!a && path.rfind("Track menu", 0) == 0) {
                r.w->panel->fillTrackMenu(track, r.tracks[2]);
                a = findAction(track, path, "Track menu");
            }
            if (a && a->enabled) Menu::activate(a);
        };
        ops.push_back(op);
    }
    return ops;
}

// --- menus taller than the screen; tooltips at the screen's corners ---
int menusChecked = 0;
void checkMenus(Rig& r, const Ctx& ctx) {
    App& app = App::get();
    Rect scr = ctx.screen();
    // a menu of 120 entries (as a long plugin or preset list) and the menu bar's own, opened at the screen's corners
    Menu tall("Tall");
    for (int i = 0; i < 120; i++) tall.addAction("Preset number " + std::to_string(i) + "…", [] {});
    Menu* sub = tall.addMenu("More presets");
    for (int i = 0; i < 90; i++) sub->addAction("Sub preset " + std::to_string(i), [] {});
    std::vector<Menu*> menus{&tall};
    for (auto& m : r.w->menuBar->menus()) menus.push_back(m.get());
    for (Menu* m : menus) {
        for (Point at : {Point(scr.x + 2, scr.y + 2), Point(scr.right() - 3, scr.bottom() - 3), Point(scr.x + scr.w / 2, scr.y + scr.h / 2)}) {
            m->popup(at, r.w);
            settle(1);
            menusChecked++;
            for (Window* p : app.popups()) {
                Rect g = p->outerScreenGeometry();
                CHECK(inside(g, scr));
                if (!inside(g, scr)) {
                    rep.context = "menu " + tui::menuText(m->title) + " @ " + ctx.name();
                    rep.add("menu-off-screen", "Menu", "Menu \"" + tui::menuText(m->title) + "\"",
                            rep.context + ": " + audit::rectText(g) + " on " + sz(scr.w, scr.h), "menu|" + m->title);
                }
            }
            app.closeAllPopups();
        }
    }
    // the tall menu: the last entry reachable by the keyboard and the wheel (it scrolls into view)
    tall.popup(Point(scr.x, scr.y), r.w);
    settle(1);
    std::vector<Window*> pops = app.popups();
    if (!pops.empty()) {
        Window* p = pops.back();
        if (ctx.pictures()) picture(*p, "menu tall top @ " + ctx.name());
        plat::Event e;
        e.type = plat::Event::KeyDown;
        e.key = plat::Key_End;
        p->dispatchKey(e);  // (End: the last entry chosen)
        settle(1);
        // the item chosen is painted inside the popup: the popup's picture shows it lit (its rect inside)
        if (ctx.pictures()) picture(*p, "menu tall End @ " + ctx.name());
        // the submenu (the last entry) opens with Right: also on the screen
        e.key = plat::Key_Right;
        p->dispatchKey(e);
        settle(1);
        for (Window* q : app.popups()) CHECK(inside(q->outerScreenGeometry(), scr));
        if (app.popups().size() > 1 && ctx.pictures()) picture(*app.popups().back(), "menu tall submenu @ " + ctx.name());
    }
    app.closeAllPopups();
    // tooltips: a long one at each corner of the screen, on the screen
    std::string longTip;
    for (int i = 0; i < 30; i++) longTip += "a long explanation of what this control does ";
    for (Point at : {Point(scr.x, scr.y), Point(scr.right() - 1, scr.bottom() - 1), Point(scr.right() - 1, scr.y)}) {
        for (const std::string& tip : {std::string("Volume"), longTip}) {
            app.showToolTip(at, tip, r.w);
            settle(1);
            if (Window* t = app.toolTipWindow(); t && t->isWindowShown()) {
                Rect g = t->outerScreenGeometry();
                CHECK(inside(g, scr));
                if (!inside(g, scr)) {
                    rep.context = "tooltip @ " + ctx.name();
                    rep.add("tooltip-off-screen", "ToolTip", "ToolTip (" + std::to_string(tip.size()) + " chars)",
                            rep.context + ": " + audit::rectText(g) + " on " + sz(scr.w, scr.h), "tip|" + std::to_string(tip.size() > 10));
                }
                if (tip.size() > 10 && ctx.pictures() && at.x == scr.x) picture(*t, "tooltip long @ " + ctx.name());
            }
            app.hideToolTip();
        }
    }
}

void writeReport() {
    std::ofstream o(OUT + "/report.md");
    o << "# Every dialog and window at its size extremes (tests/test_window_extremes.cpp)\n\n";
    o << windowsSeen << " windows swept, " << menusChecked << " menus opened, in " << elapsed() << " s\n\n";
    o << "## Windows\n";
    for (auto& [n, t] : titles) o << "- " << n << ": " << t << "\n";
    o << "\n## Notes\n";
    for (auto& n : notes) o << "- " << n << "\n";
    std::map<std::string, int> kinds;
    for (auto& k : rep.order) kinds[rep.entries.at(k).f.kind]++;
    o << "\n## Findings by kind\n";
    for (auto& [k, n] : kinds) o << "- " << k << ": " << n << "\n";
    o << "\n## Findings\n" << rep.text();
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    skin::setBuiltinDir("data/skins");
    paths::setDataDir(util::absolutePath(OUT + "/tmp/data"));
    App::get().initHeadless();
    theme::setSkin();
    util::makeDirs(OUT);
    for (auto& e : std::filesystem::directory_iterator(OUT))
        if (e.is_regular_file() && e.path().extension() == ".png") std::filesystem::remove(e.path());
    util::makeDirs(OUT + "/tmp");
    settings::setPath(OUT + "/tmp/settings.json");
    App& app = App::get();
    const char* only = getenv("TINYDAW_EXTREMES_ONLY");  // (a name's part: only those windows, while fixing)
    std::vector<Ctx> ctxs;
    for (int pct : {100, 75, 150, 200})
        for (auto [pw, ph] : std::vector<std::pair<int, int>>{{1920, 1080}, {3840, 2160}, {1024, 600}, {800, 480}})
            ctxs.push_back({pct, pw, ph});
    for (const Ctx& ctx : ctxs) {
        double t0 = elapsed();
        app.setScale(ctx.percent / 100.f);
        app.headlessScreen = ctx.screen();
        Rig r = makeRig(ctx.screen());
        std::vector<Opener> ops = allOpeners(r);
        int n = 0;
        for (const Opener& op : ops) {
            if (only && op.name.find(only) == std::string::npos) continue;
            openAndSweep(r, op, ctx);
            n++;
        }
        if (!only) {
            checkMenus(r, ctx);
            checkScrolling(r, ctx);
            if (ctx.pictures()) allAtOnce(r, ops, ctx);
        }
        closeRig(r);
        printf("window extremes: %s: %d windows in %.1f s (%d findings so far)\n", ctx.name().c_str(), n, elapsed() - t0,
               rep.size());
        fflush(stdout);
    }
    app.setScale(1);
    app.headlessScreen = Rect(0, 0, 1920, 1080);
    writeReport();
    printf("window extremes: %d windows swept, %d findings in %.1f s: %s/report.md\n", windowsSeen, rep.size(), elapsed(),
           OUT.c_str());
    CHECK(windowsSeen > 0);
    // what was fixed stays fixed: every window opens on the screen and no bigger, its OK / Cancel / Close reachable,
    // no broken geometry at any size
    CHECK(failedOffScreen == 0);
    CHECK(failedButtons == 0);
    CHECK(failedSane == 0);
    TEST_MAIN_END();
}
