// A layout audit (tests/layout_audit.h): the main window with a realistic project at many sizes, interface sizes
// and skins; every resizable part at its limits; the dialogs the menus open; the terminal's picture at many sizes.
// What it finds is reported, not failed: build/ui-samples/layout-audit/report.md, with pictures beside it.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "layout_audit.h"
#include "model/model.h"
#include "paths.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/explorer.h"
#include "ui/lane_panel.h"
#include "ui/lanes.h"
#include "ui/main_window.h"
#include "ui/midi_editor.h"
#include "ui/midi_effects_dialog.h"
#include "ui/pad_kit_dialog.h"
#include "ui/project_safety.h"
#include "ui/sampler_dialog.h"
#include "ui/synth_dialog.h"
#include "ui/skin.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

using namespace ui;

namespace {

const std::string OUT = "build/ui-samples/layout-audit";
const int SR = 48000;
audit::Report rep;
std::vector<std::string> notes;  // what the sweeps couldn't do (in the report)
auto started = std::chrono::steady_clock::now();
double elapsed() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); }

std::string fileSafe(std::string s) {
    for (auto& c : s)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '-';
    return s;
}

void settle(int frames = 3) {
    for (int i = 0; i < frames; i++) {
        App::get().advanceTimers(0.02);
        App::get().tick();
    }
    App::get().doLayouts();
}

// one window checked in one context; its pictures saved when something new was found there
void auditWindow(Window& w, const std::string& context, bool unpainted = true, bool keep = false) {
    double t0 = elapsed();
    rep.context = context;
    std::string base = OUT + "/" + fileSafe(context);
    rep.picture = base + ".png";
    int before = rep.size();
    App::get().doLayouts();
    w.update();
    App::get().tick();
    Image canary = audit::checkWindow(w, rep, audit::Options(), true, unpainted);
    if (rep.size() > before || keep) {
        w.update();
        App::get().tick();
        savePng(base + ".png", w.framebuffer());
        bool anyUnpainted = false;
        for (auto& k : rep.order) {
            auto& e = rep.entries[k];
            if (e.f.kind == "unpainted" && e.picture == base + ".png") anyUnpainted = true;
        }
        if (anyUnpainted && !canary.empty()) savePng(base + "-canary.png", canary);
    }
    if (getenv("AUDIT_TIMES")) printf("  %.2f s  %s\n", elapsed() - t0, context.c_str());
}

// lag: a whole repaint, and a resize by a pixel (its layout and the repaint of what changed), as a frame at 60 Hz
// would need them (this thread alone, as the GUI thread does); the slowest of 3
void perf(Window& w, const std::string& context) {
    App& app = App::get();
    auto ms = [](const std::function<void()>& fn) {
        double worst = 0;
        for (int i = 0; i < 3; i++) {
            auto t0 = std::chrono::steady_clock::now();
            fn();
            worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        return worst;
    };
    double paint = ms([&] {
        w.invalidate(w.outerRect());
        w.flush();
    });
    int W = w.width(), H = w.height();
    double resize = ms([&] {
        w.resizeWindow(W + 1, H + 1);
        app.doLayouts();
        w.flush();
        w.resizeWindow(W, H);
        app.doLayouts();
        w.flush();
    }) / 2;
    char buf[200];
    snprintf(buf, sizeof buf, "%s: whole repaint %.1f ms, a resize step %.1f ms", context.c_str(), paint, resize);
    notes.push_back(std::string("timing: ") + buf);
    rep.context = context;
    rep.picture = "";
    if (paint > 16.7)
        rep.add("slow-frame", w.type(), w.type(), "a whole repaint takes " + std::to_string((int)paint) + " ms (over a 60 Hz frame)",
                "repaint " + w.type());
    if (resize > 16.7)
        rep.add("slow-frame", w.type(), w.type(), "a resize step (layout + repaint) takes " + std::to_string((int)resize) +
                    " ms: resizing the window lags", "resize " + w.type());
}

AudioPtr sine(double seconds, double hz) {
    auto a = std::make_shared<AudioBuffer>();
    int64_t n = (int64_t)(seconds * SR);
    a->samples.resize(n * 2);
    for (int64_t i = 0; i < n; ++i) a->samples[i * 2] = a->samples[i * 2 + 1] = (float)(0.5 * std::sin(2 * M_PI * hz * i / SR));
    return a;
}

ClipPtr audioClip(Project& p, const TrackPtr& t, double start, double seconds, const std::string& name) {
    auto c = makeAudioClip(name, sine(seconds, 2 + start), SR, SR);
    c->track = t;
    c->start = (int64_t)(start * SR);
    p.addClip(c);
    return c;
}

ClipPtr midiClip(Project& p, const TrackPtr& t, double start, double seconds, const std::string& name) {
    std::vector<Note> ns;
    for (int i = 0; i < 16; ++i) ns.push_back({i * 0.25, i * 0.25 + 0.2, 60.0 + (i * 5) % 14, 100});
    auto c = makeMidiClip(name, ns, seconds, SR);
    c->track = t;
    c->start = (int64_t)(start * SR);
    p.addClip(c);
    return c;
}

AutomationPtr volumeLane(const TrackPtr& t) {
    auto a = std::make_shared<Automation>();
    a->slot = t->controls;
    a->param = "Volume";
    a->setPoints({{0, 0.2}, {2, 0.9}, {4, 0.4}, {8, 0.7}});
    return a;
}

struct Rig {
    MainWindow* w = nullptr;
    std::vector<TrackPtr> tracks;
    GroupPtr group, inner;
    TrackPtr ret;
    ClipPtr firstMidi, firstAudio;
};

// the realistic project: 8 audio and MIDI tracks (long names too), a group bus with a nested group, a return,
// clips, automation lanes shown, take lanes, the tempo lane, markers
Rig makeMain(int w = 1600, int h = 1000) {
    Rig r;
    r.w = new MainWindow();
    r.w->askBeforeClosing = false;
    r.w->resizeWindow(w, h);
    r.w->showWindow();
    Timeline* tl = r.w->timeline;
    Project& p = r.w->project;
    const char* names[8] = {"Drums", "Bass", "Rhythm guitar (doubled, left and right)", "Vocals",
                            "Keys", "Lead synth", "Pad", "Arp — 16ths through the chorus"};
    for (int i = 0; i < 8; i++) {
        TrackPtr t = tl->addTrack(i < 4 ? Kind::Audio : Kind::Midi);
        t->name = names[i];
        r.tracks.push_back(t);
        for (int k = 0; k < 3; k++) {
            ClipPtr c = i < 4 ? audioClip(p, t, k * 6.0 + i * 0.5, 4, std::string(names[i]) + " " + std::to_string(k + 1))
                              : midiClip(p, t, k * 6.0 + i * 0.5, 4, std::string(names[i]) + " " + std::to_string(k + 1));
            if (!r.firstMidi && i >= 4) r.firstMidi = c;
            if (!r.firstAudio && i < 4) r.firstAudio = c;
        }
    }
    r.tracks[0]->automation = std::vector<AutomationPtr>{volumeLane(r.tracks[0])};
    r.tracks[0]->showAutomation = true;
    r.tracks[5]->automation = std::vector<AutomationPtr>{volumeLane(r.tracks[5])};
    r.tracks[5]->showAutomation = true;
    auto take = makeAudioClip("Take 1", sine(3, 5), SR, SR), take2 = makeAudioClip("Take 2", sine(3, 3), SR, SR);
    take->start = take2->start = SR;
    r.tracks[3]->takes = std::vector<ClipPtr>{take, take2};
    r.tracks[3]->showTakes = true;
    r.group = tl->groupTracks({r.tracks[0], r.tracks[1], r.tracks[2]});
    if (r.group) r.group->name = "Rhythm section";
    r.inner = tl->groupTracks({r.tracks[1], r.tracks[2]});
    if (r.inner) r.inner->name = "Low end";
    r.ret = tl->addTrack(Kind::Return);
    r.ret->name = "Reverb return";
    p.setMarkers({{0, "Intro"}, {8, "Verse"}, {16, "Chorus — the big one"}, {24, "Outro"}});
    r.w->showTempoLane(true);
    tl->lanesChanged();
    tl->tracksChanged.emit();
    settle();
    return r;
}

void closeMain(Rig& r) {
    r.w->closeFxWindows();
    for (Window* win : std::vector<Window*>(App::get().windows()))
        if (win != r.w && win->isWindowShown() && !win->isPopup()) win->hideWindow();
    App::get().closeAllPopups();
    delete r.w;
    r.w = nullptr;
    settle(1);
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

std::string sz(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

// --- 1. the main window from its minimum to 3840x2160, very wide, very tall ---
void sweepSizes() {
    Rig r = makeMain();
    // (the same project for the real program: EXBIT_SCRIPT=… with `call open` of it, on a real display)
    util::makeDirs(OUT + "/tmp/project");
    r.w->saveProjectFile(util::absolutePath(OUT + "/tmp/project/audit.json"));
    Point mn = windowMinimum(*r.w);
    notes.push_back("main window's minimum (its layout's): " + sz(mn.x, mn.y));
    std::vector<std::pair<int, int>> sizes{{mn.x, mn.y}, {std::max(mn.x, 800), std::max(mn.y, 600)}, {1024, 768}, {1280, 800},
                                           {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {3840, 400}, {500, 2000},
                                           {mn.x, 2000}, {3840, mn.y}};
    for (auto [w, h] : sizes) {
        r.w->resizeWindow(w, h);
        settle(2);
        auditWindow(*r.w, "main " + sz(w, h) + " Dark 100%", true, true);
        if (w >= 1280) perf(*r.w, "main " + sz(w, h));
    }
    // below the minimum (the window manager keeps it from going there; a tiling one may not)
    r.w->resizeWindow(std::max(1, mn.x - 200), std::max(1, mn.y - 150));
    settle(2);
    rep.context = "main below minimum " + sz(mn.x - 200, mn.y - 150);
    rep.picture = "";
    closeMain(r);
}

// --- 2. interface sizes and skins ---
void sweepScalesAndSkins() {
    for (int size : prefs::interfaceSizes()) {
        if (elapsed() > 17) { notes.push_back("interface sizes from " + std::to_string(size) + " % skipped (time)"); break; }
        App::get().setScale(size / 100.f);
        Rig r = makeMain(1280, 800);
        for (auto [w, h] : std::vector<std::pair<int, int>>{{1280, 800}, {1920, 1080}}) {
            if (w == 1920 && size % 50) continue;  // (the big one at 100, 150, 200 %: time)
            r.w->resizeWindow(w, h);
            settle(2);
            auditWindow(*r.w, "main " + sz(w, h) + " Dark " + std::to_string(size) + "%", size == 100 || size == 200, true);
        }
        closeMain(r);
    }
    App::get().setScale(1);
    Rig r = makeMain(1280, 800);
    for (const skin::Skin& s : skin::library()) {
        r.w->chooseSkin(s);  // (as Options → Skin does)
        settle(2);
        for (auto [w, h] : std::vector<std::pair<int, int>>{{1024, 700}, {1920, 1080}}) {
            r.w->resizeWindow(w, h);
            settle(2);
            auditWindow(*r.w, "main " + sz(w, h) + " " + s.name + " 100%", true, true);
        }
    }
    r.w->chooseSkin(skin::find("Dark"));
    closeMain(r);
}

// --- 3. every resizable part at its limits ---
void sweepParts() {
    Rig r = makeMain(1280, 800);
    MainWindow& w = *r.w;
    Timeline* tl = w.timeline;
    auto check = [&](const std::string& what) {
        settle(2);
        auditWindow(w, "main 1280x800 " + what, true, true);
    };
    for (auto& t : r.tracks) tl->setTrackHeight(*t, config::MIN_TRACK_H);
    for (auto& t : r.tracks)
        for (auto& a : *t->automation) tl->setTrackHeight(*a, config::AUTOMATION_MIN_H);
    tl->lanesChanged();
    check("lanes at their smallest");
    for (auto& t : r.tracks) tl->setTrackHeight(*t, config::MAX_TRACK_H);
    for (auto& t : r.tracks)
        for (auto& a : *t->automation) tl->setTrackHeight(*a, config::AUTOMATION_MAX_H);
    tl->lanesChanged();
    check("lanes at their largest");
    for (auto& t : r.tracks) tl->setTrackHeight(*t, config::TRACK_H);
    tl->lanesChanged();
    w.setExplorerWidth(0);
    check("explorer narrowest");
    w.setExplorerWidth(100000);
    check("explorer widest");
    int widest = w.explorerWidth;
    w.resizeWindow(900, 700);
    check("explorer widest then window narrowed to 900");
    w.resizeWindow(1280, 800);
    check("explorer widest, window back to 1280");
    // the width it was dragged to, lost when the window was narrower for a moment (a maximize and restore, a
    // window manager's resize below the minimum before it clamps it)
    auto lossy = [&](int was, const std::string& how) {
        if (w.explorerWidth == was) return;
        rep.context = "main 1280x800 " + how;
        rep.picture = OUT + "/main-1280x800-explorer-widest--window-back-to-1280.png";
        rep.add("unstable", "MainWindow::explorerWidth", "ExplorerDock",
                "the Explorer was " + std::to_string(was) + " px wide; after " + how + " it is " +
                    std::to_string(w.explorerWidth) + " px (its width is not restored)",
                "explorer width lost");
    };
    lossy(widest, "the window narrowed to 900 and back to 1280");
    w.setExplorerWidth(config::EXPLORER_W);
    w.resizeWindow(600, 400);
    settle(1);
    w.resizeWindow(1280, 800);
    settle(1);
    lossy(config::EXPLORER_W, "a resize to 600x400 (below the minimum, as a window manager may send before clamping) and back");
    w.setExplorerWidth(config::EXPLORER_W);
    w.setExplorerVisible(false);
    check("explorer hidden");
    w.setExplorerVisible(true);
    w.showTempoLane(false);
    check("tempo lane off");
    w.showTempoLane(true);
    if (r.group) {
        tl->toggleFold(r.inner);
        check("inner group folded");
        tl->toggleFold(r.group);
        check("outer group folded (nested folded)");
        tl->toggleFold(r.group);
        tl->toggleFold(r.inner);
    }
    w.showAllAutomation(true);
    check("all automation shown");
    // 100 tracks
    for (int i = 0; i < 92; i++) {
        TrackPtr t = w.project.addTrack(i % 2 ? Kind::Midi : Kind::Audio);  // (one rebuild after, not 92)
        if (i % 3 == 0) audioClip(w.project, t, i * 0.25, 2, "Clip " + std::to_string(i));
    }
    tl->lanesChanged();
    tl->tracksChanged.emit();
    check("100 tracks");
    perf(w, "main 1280x800 100 tracks");
    // what a click costs with 100 tracks: a track added from the panel (A / + Add), the panel rebuilt, lanes changed
    auto timeIt = [&](const std::string& what, const std::function<void()>& fn) {
        auto t0 = std::chrono::steady_clock::now();
        fn();
        settle(1);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        notes.push_back("timing: 100 tracks: " + what + " " + std::to_string((int)ms) + " ms");
        rep.context = "main 1280x800 100 tracks";
        rep.picture = "";
        if (ms > 50) rep.add("slow-action", what, what, "takes " + std::to_string((int)ms) + " ms with 100 tracks (a visible hitch)", what);
    };
    timeIt("a track added (Timeline::addTrack)", [&] { tl->addTrack(Kind::Audio); });
    timeIt("the track panel rebuilt", [&] { w.panel->rebuild(); });
    timeIt("lanes changed (an automation lane shown)", [&] { tl->lanesChanged(); });
    timeIt("a track's height changed", [&] { tl->setTrackHeight(*r.tracks[3], 150); });
    w.resizeWindow(3840, 2160);
    check("100 tracks at 3840x2160");
    perf(w, "main 3840x2160 100 tracks");
    closeMain(r);
}

// --- 4. the dialogs and windows: each at its default, smallest and largest size ---
std::set<std::string> auditedWindows;
void auditOther(Window& win, const std::string& name) {
    std::string title = name.empty() ? win.title() : name;
    if (title.empty()) title = win.type();
    if (auditedWindows.count(title)) return;
    auditedWindows.insert(title);
    Point def(win.width(), win.height());
    Point least = windowMinimum(win);
    if (least.x > def.x || least.y > def.y) {  // (it opens smaller than its own layout's minimum: something is cut)
        rep.context = title;
        rep.picture = OUT + "/window-" + fileSafe(title) + ".png";
        rep.add("opens-below-minimum", win.type(), win.type() + " \"" + win.title() + "\"",
                "opens at " + sz(def.x, def.y) + " but its layout needs at least " + sz(least.x, least.y), win.type());
    }
    auditWindow(win, title + " default " + sz(def.x, def.y));
    savePng(OUT + "/window-" + fileSafe(title) + ".png", win.framebuffer());  // (every window's, to look at)
    std::string seen = "window audited: " + title + " (" + win.type() + ", \"" + win.title() + "\") default " + sz(def.x, def.y);
    if (!win.isResizable() || win.isPopup()) {
        notes.push_back(seen + ", not resizable");
        return;
    }
    Point mn = windowMinimum(win);
    mn = Point(std::max(1, mn.x), std::max(1, mn.y));
    win.resizeWindow(mn.x, mn.y);
    settle(1);
    auditWindow(win, title + " smallest " + sz(mn.x, mn.y));
    Point mx = win.maximumSize();
    int W = std::min(mx.x, 2560), H = std::min(mx.y, 1600);
    win.resizeWindow(W, H);
    settle(1);
    auditWindow(win, title + " largest " + sz(W, H));
    notes.push_back(seen + ", smallest " + sz(mn.x, mn.y) + ", largest " + sz(W, H));
    win.resizeWindow(def.x, def.y);
    settle(1);
}

std::vector<Window*> shownOthers(Window* main) {
    std::vector<Window*> out;
    for (Window* w : App::get().windows())
        if (w != main && w->isWindowShown() && w != App::get().toolTipWindow()) out.push_back(w);
    return out;
}

// actions that leave the program, run other programs, scan the system, take long, or would change the user's
// files: never triggered
bool unsafe(const std::string& t) {
    for (const char* s : {"Quit", "Exit", "Edit this skin", "folder", "Install", "update", "Update", "Rescan", "Scan",
                          "Report", "website", "Website", "Open recent", "Restart", "Crash", "Wine", "Export skin",
                          "Delete", "Remove", "Clear", "Freeze", "Bounce", "Render", "Record"})
        if (t.find(s) != std::string::npos) return true;
    return false;
}

void walkMenu(MainWindow& w, Menu& m, const std::string& path, int depth) {
    if (depth > 3 || elapsed() > 11) return;
    if (m.aboutToShow) m.aboutToShow(&m);
    std::vector<Action*> acts;
    for (auto& a : m.actions()) acts.push_back(a.get());
    for (Action* a : acts) {
        if (a->separator || a->section || !a->visible) continue;
        std::string text = tui::menuText(a->text);
        std::string here = path + "/" + text;
        if (a->submenu) {
            walkMenu(w, *a->submenu, here, depth + 1);
            continue;
        }
        if (!a->enabled || unsafe(text)) continue;
        // only entries that open a window (…) are triggered; toggles would change the state the others see
        if (text.find("…") == std::string::npos && text.find("...") == std::string::npos) continue;
        std::vector<Window*> before = shownOthers(&w);
        Menu::activate(a);
        settle(2);
        for (Window* win : shownOthers(&w))
            if (std::find(before.begin(), before.end(), win) == before.end()) {
                auditOther(*win, here);
                win->hideWindow();
            }
        App::get().closeAllPopups();
    }
}

void sweepDialogs() {
    App& app = App::get();
    Rig r = makeMain(1280, 800);
    MainWindow& w = *r.w;
    // modal dialogs and blocking menus: checked inside their loop, then closed
    std::string current;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 40 && !done(); i++) {
            for (Window* win : shownOthers(&w)) {
                if (Menu::ofWindow(win)) continue;  // (menus: their own sweep below)
                auditOther(*win, current + " › " + (win->title().empty() ? win->type() : win->title()));
            }
            for (Window* win : shownOthers(&w))
                if (!Menu::ofWindow(win)) win->close();
            app.closeAllPopups();
            app.advanceTimers(0.02);
            app.collectGarbage();
        }
    };
    // the menu bar's entries that open windows
    for (auto& m : w.menuBar->menus()) {
        current = tui::menuText(m->title);
        walkMenu(w, *m, current, 0);
    }
    // the windows of their own: the MIDI editor (and its Steps view), effects, instrument panel
    if (r.firstMidi) {
        current = "MIDI editor";
        w.openMidiEditor(r.firstMidi);
        settle(2);
        for (Window* win : shownOthers(&w)) {
            if (auto* ed = dynamic_cast<MidiEditor*>(win)) {
                auditOther(*ed, "MIDI editor");
                ed->setStepsView(true);
                settle(2);
                auditOther(*ed, "MIDI editor Steps view");
                ed->hideWindow();
            }
        }
    }
    auto openEach = [&](const std::string& name, const std::function<void()>& open) {
        if (elapsed() > 12) { notes.push_back(name + ": skipped (time)"); return; }
        current = name;
        std::vector<Window*> before = shownOthers(&w);
        open();
        settle(2);
        for (Window* win : shownOthers(&w))
            if (std::find(before.begin(), before.end(), win) == before.end()) {
                auditOther(*win, name);
                win->hideWindow();
            }
    };
    openEach("Effects (patch space) of Drums", [&] { w.openTrackPlugins(r.tracks[0]); });
    openEach("Master effects", [&] { w.openMasterPlugins(); });
    openEach("Instrument panel of Keys", [&] { w.openInstrumentPanel(r.tracks[4]); });
    openEach("Keyboard shortcuts", [&] { w.showShortcuts(); });
    openEach("Command palette", [&] { w.showPalette(); });
    openEach("MIDI ports", [&] { w.showMidiPorts(); });
    openEach("Synth of Keys", [&] { synth_dialog::openSynth(r.tracks[4]); });
    openEach("Sampler of Lead synth", [&] { sampler_dialog::openSampler(r.tracks[5]); });
    openEach("Pad Kit of Pad", [&] { pad_kit_dialog::openPadKit(r.tracks[6]); });
    openEach("MIDI effects of Arp", [&] { midi_effects_dialog::openMidiEffects(r.tracks[7]); });
    if (r.firstAudio) {
        openEach("Slice to a Pad Kit", [&] { w.timeline->sliceToPadKit(r.firstAudio); });
        w.timeline->selection.selectOnly(r.firstAudio);
        openEach("Warp", [&] { w.timeline->warpSelected(r.firstAudio->start + SR); });
        r.firstAudio->missing = std::make_shared<MissingFile>(MissingFile{"/nowhere/a very long folder name/Drums 1.wav", 1234, ""});
        openEach("Missing samples", [&] { project_safety::showMissing(w, true); });
    }
    {
        Menu menu;
        w.fillInstrumentMenu(menu, r.tracks[4]);
        current = "Instrument menu (Keys)";
        walkMenu(w, menu, current, 0);
    }
    // the track panel's right-click menus: each kind of track's dialogs (synth, sampler, Pad Kit, MIDI effects…)
    for (TrackPtr t : {r.tracks[0], r.tracks[4], r.ret}) {
        Menu menu;
        w.panel->fillTrackMenu(menu, t);
        current = "Track menu (" + t->name + ")";
        walkMenu(w, menu, current, 0);
    }
    // the clips' right-click menus (warp, slice, gain…)
    for (ClipPtr c : {r.firstAudio, r.firstMidi}) {
        if (!c) continue;
        w.timeline->selection.clear();
        w.timeline->selection.selectOnly(c);
        Menu menu;
        w.timeline->fillMenu(menu, std::nullopt);
        current = "Clip menu (" + c->name + ")";
        walkMenu(w, menu, current, 0);
    }
    app.nestedLoop = nullptr;
    closeMain(r);
}

// --- 5. the terminal's picture ---
Action* findAction(Menu& m, const std::string& prefix) {
    if (m.aboutToShow) m.aboutToShow(&m);
    for (auto& a : m.actions()) {
        if (tui::menuText(a->text).compare(0, prefix.size(), prefix) == 0 && !a->submenu) return a.get();
        if (a->submenu)
            if (Action* x = findAction(*a->submenu, prefix)) return x;
    }
    return nullptr;
}

void sweepTui() {
    App& app = App::get();
    app.paintWindows = false;
    Rig r = makeMain(1280, 800);
    r.w->resizeWindow(1280, 800);
    tui::Front f(*r.w);
    tui::Screen* screen = nullptr;
    std::string dialogAt;  // (the dialog being opened: its picture is taken inside its loop)
    std::function<void(tui::Screen&, const std::string&)> picture;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        if (screen && !dialogAt.empty()) {
            settle(1);
            picture(*screen, dialogAt);
            dialogAt.clear();
        }
        for (int i = 0; i < 40 && !done(); i++) {
            f.key(tui::Key{plat::Key_Escape, 0, ""});
            app.advanceTimers(0.05);
            app.tick();
        }
    };
    picture = [&](tui::Screen& s, const std::string& where) {
        f.draw(s);
        std::string file = OUT + "/" + fileSafe(where) + ".txt";
        rep.picture = file;
        audit::checkScreen(s, rep, where);
        std::ofstream(file) << s.lines();
    };
    for (auto [W, H] : std::vector<std::pair<int, int>>{{40, 12}, {60, 20}, {80, 24}, {120, 40}, {200, 60}, {300, 100}}) {
        tui::Screen s(W, H);
        std::string at = sz(W, H);
        settle(2);
        picture(s, "tui " + at + " start");
        for (int m = 0; m < 4; m++) {
            f.openBarMenu(m);
            picture(s, "tui " + at + " menu " + std::to_string(m));
            f.closeMenus();
        }
        f.key(tui::Key{plat::Key_F1, 0, ""});
        picture(s, "tui " + at + " F1");
        f.key(tui::Key{plat::Key_Escape, 0, ""});
        // the main dialogs (modal: drawn inside their loop) and windows of their own (closed with Ctrl+W)
        screen = &s;
        for (const char* entry : {"Save as template", "Insert time", "Audio properties", "MIDI ports", "Keyboard shortcuts",
                                  "Command palette", "Find missing samples", "History"}) {
            Action* a = nullptr;
            for (auto& m : r.w->menuBar->menus())
                if (!a) a = findAction(*m, entry);
            if (!a) { notes.push_back(std::string("TUI: no menu entry ") + entry); continue; }
            dialogAt = "tui " + at + " " + entry;
            Menu::activate(a);
            settle(2);
            if (!dialogAt.empty()) {  // (not modal: a window of its own)
                picture(s, dialogAt);
                dialogAt.clear();
                f.key(tui::Key{'W', plat::ModCtrl, ""});
                settle(1);
            }
        }
        for (auto [name, open] : std::vector<std::pair<std::string, std::function<void()>>>{
                 {"MIDI editor", [&] { r.w->openMidiEditor(r.firstMidi); }},
                 {"Synth", [&] { synth_dialog::openSynth(r.tracks[4]); }},
                 {"Effects", [&] { r.w->openTrackPlugins(r.tracks[0]); }},
                 {"Track delay", [&] {
                      Menu menu;
                      r.w->panel->fillTrackMenu(menu, r.tracks[0]);
                      if (Action* d = findAction(menu, "Track delay")) Menu::activate(d);
                  }}}) {
            dialogAt = "tui " + at + " " + name;
            open();
            settle(2);
            if (!dialogAt.empty()) {
                picture(s, dialogAt);
                dialogAt.clear();
                f.key(tui::Key{'W', plat::ModCtrl, ""});
                settle(1);
            }
        }
        screen = nullptr;
    }
    app.nestedLoop = nullptr;
    closeMain(r);
    app.paintWindows = true;
}

std::string severityOf(const std::string& kind) {
    if (kind == "overlap" || kind == "clipped" || kind == "unstable") return "A";
    if (kind == "min-size" || kind == "text" || kind == "tui-wide" || kind == "tui-box") return "B";
    return "C";
}

void writeReport() {
    std::ofstream o(OUT + "/report.md");
    o << "# Layout audit (raw findings: tests/test_layout_audit.cpp)\n\n";
    for (auto& n : notes) o << "- " << n << "\n";
    o << "\n" << rep.size() << " distinct findings.\n\n";
    for (const char* sev : {"A", "B", "C"}) {
        o << "## Severity " << sev << "\n\n";
        for (auto& k : rep.order) {
            auto& e = rep.entries.at(k);
            if (severityOf(e.f.kind) != sev) continue;
            o << "- **" << e.f.kind << "** " << e.f.widget << "\n  - at `" << e.f.path << "`\n  - " << e.f.detail
              << "\n  - seen " << e.count << "× in " << e.contexts.size() << " context(s): ";
            for (size_t i = 0; i < e.contexts.size() && i < 10; i++) o << (i ? "; " : "") << e.contexts[i];
            if (e.contexts.size() > 10) o << "; … +" << e.contexts.size() - 10;
            if (!e.variants.empty()) {
                o << "\n  - also: ";
                for (size_t i = 0; i < e.variants.size(); i++) o << (i ? ", " : "") << e.variants[i];
            }
            if (!e.picture.empty()) o << "\n  - picture: " << e.picture;
            o << "\n";
        }
        o << "\n";
    }
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    // the program's own skins (from the source tree); your data (skins, templates, presets) from a scratch folder,
    // never the real ~/.local/share/Exbit
    skin::setBuiltinDir("data/skins");
    paths::setDataDir(util::absolutePath(OUT + "/tmp/data"));
    App::get().initHeadless();
    theme::setSkin();
    util::makeDirs(OUT);
    for (auto& e : std::filesystem::directory_iterator(OUT))  // (the last run's pictures: this run's alone are listed)
        if (e.is_regular_file() && (e.path().extension() == ".png" || e.path().extension() == ".txt")) std::filesystem::remove(e.path());
    util::makeDirs(OUT + "/tmp");
    settings::setPath(OUT + "/tmp/settings.json");
    auto timed = [](const char* name, void (*fn)()) {
        double t0 = elapsed();
        fn();
        int before = 0;
        (void)before;
        printf("layout audit: %s done in %.1f s (%d findings so far)\n", name, elapsed() - t0, rep.size());
        fflush(stdout);
    };
    timed("sizes", sweepSizes);
    timed("parts", sweepParts);
    timed("dialogs", sweepDialogs);
    timed("tui", sweepTui);
    timed("scales and skins", sweepScalesAndSkins);
    writeReport();
    printf("layout audit: %d distinct findings in %.1f s: %s/report.md\n", rep.size(), elapsed(), OUT.c_str());
    CHECK(true);
    TEST_MAIN_END();
}
