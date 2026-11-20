// A hunt for visual bugs in windows, dialogs, menus and popups (headless): where windows and popups are placed on
// the screen (several tool windows at once, near the edges, a modal over a tool window), every context menu and its
// submenus near the screen's corners, tooltips, drop-down lists with many entries, long and non-Latin names, values
// at their extremes, the windows the layout audit skipped (the sampler with a sample, a plugin's), the history with
// 100 steps, the palette with none and with hundreds of matches, message boxes with long text, and keyboard focus
// (the ring visible, Tab following the visual order).
// What it finds is reported, never failed: build/ui-samples/window-hunt/raw-findings.md, pictures beside it.
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
#include "gfx/font.h"
#include "gfx/image.h"
#include "gfx/painter.h"
#include "layout_audit.h"
#include "model/model.h"
#include "paths.h"
#include "platform/platform.h"
#include "ui/color_picker.h"
#include "ui/commands.h"
#include "ui/history_window.h"
#include "ui/lane_panel.h"
#include "ui/lanes.h"
#include "ui/main_window.h"
#include "ui/midi_editor.h"
#include "ui/midi_effects_dialog.h"
#include "ui/note_tools_panel.h"
#include "tui/app.h"
#include "ui/pad_kit_dialog.h"
#include "ui/plugin_dialog.h"
#include "ui/sampler_dialog.h"
#include "ui/shortcuts_dialog.h"
#include "ui/skin.h"
#include "ui/synth_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/frame.h"
#include "ui/toolkit/layout.h"
#include "ui/plugin_host.h"
#include "plugins/library.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/style.h"
#include "ui/toolkit/widget.h"
#include "ui/widgets.h"

using namespace ui;

namespace {

const std::string OUT = "build/ui-samples/window-hunt";
const int SR = 48000;
auto started = std::chrono::steady_clock::now();
double elapsed() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); }

// --- findings: one per (id), every example of it kept (a few) ---
struct Hit {
    std::string id, title, detail, picture;
    std::vector<std::string> examples;
    int count = 0;
};
std::map<std::string, Hit> hits;
std::vector<std::string> order;
std::vector<std::string> notes;
void found(const std::string& id, const std::string& title, const std::string& example, const std::string& picture = "") {
    auto it = hits.find(id);
    if (it == hits.end()) {
        order.push_back(id);
        it = hits.emplace(id, Hit{id, title, "", picture, {}, 0}).first;
    }
    Hit& h = it->second;
    h.count++;
    if (h.picture.empty()) h.picture = picture;
    if (h.examples.size() < 12 && std::find(h.examples.begin(), h.examples.end(), example) == h.examples.end())
        h.examples.push_back(example);
}

std::string fileSafe(std::string s) {
    for (auto& c : s)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '-';
    return s.substr(0, 90);
}
std::string sz(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }
std::string rt(const Rect& r) { return audit::rectText(r); }

void settle(int frames = 2) {
    for (int i = 0; i < frames; i++) {
        App::get().advanceTimers(0.02);
        App::get().tick();
    }
    App::get().doLayouts();
}

// the screen (headless: 1920x1080) and the small laptop screen the windows should also fit
const Rect SCREEN(0, 0, 1920, 1080);
const Rect SMALL(0, 0, 1366, 768);

std::vector<Window*> shownWindows() {
    std::vector<Window*> v;
    for (Window* w : App::get().windows())
        if (w->isWindowShown()) v.push_back(w);
    std::sort(v.begin(), v.end(), [](Window* a, Window* b) { return a->showSerial_ < b->showSerial_; });
    return v;
}

// every shown window drawn where it is on the screen (in the order they were raised), on a screen of `scr`;
// what reaches outside the screen outlined in red beyond it (the picture is that much bigger)
std::string screenPicture(const std::string& name, const Rect& scr = SCREEN) {
    std::vector<Window*> ws = shownWindows();
    Rect all = scr;
    for (Window* w : ws) all = all.united(w->outerScreenGeometry());
    all = all.intersected(Rect(-1500, -1500, 7000, 5000));
    Image img(all.w, all.h, Color(40, 40, 40));
    {
        Painter p(img);
        p.fillRect(Rect(scr.x - all.x, scr.y - all.y, scr.w, scr.h), Color(70, 90, 110));
        for (Window* w : ws) {
            if (w->width() < 2 && w->height() < 2) continue;
            w->invalidate(w->outerRect());
            w->flush();
            Rect g = w->screenGeometry();
            const Image& fb = w->framebuffer();
            if (fb.width() > 0 && fb.height() > 0)
                p.drawImage(RectF((float)(g.x - all.x), (float)(g.y - all.y), (float)g.w, (float)g.h), fb);
            p.drawRect(Rect(g.x - all.x, g.y - all.y, g.w, g.h), Color(255, 230, 0), 1);
        }
        p.drawRect(Rect(scr.x - all.x, scr.y - all.y, scr.w, scr.h), Color(255, 40, 40), 3);
    }
    std::string path = OUT + "/" + fileSafe(name) + ".png";
    savePng(path, img);
    return path;
}

std::string windowPicture(Window& w, const std::string& name) {
    App::get().doLayouts();
    w.invalidate(w.outerRect());
    w.flush();
    std::string path = OUT + "/" + fileSafe(name) + ".png";
    savePng(path, w.framebuffer());
    return path;
}

bool inside(const Rect& r, const Rect& scr) { return r.intersected(scr).w == r.w && r.intersected(scr).h == r.h; }

std::string describeWin(Window* w) { return w->type() + " \"" + w->title() + "\""; }

// --- the project ---
AudioPtr sine(double seconds, double hz) {
    auto a = std::make_shared<AudioBuffer>();
    int64_t n = (int64_t)(seconds * SR);
    a->samples.resize(n * 2);
    for (int64_t i = 0; i < n; ++i) a->samples[i * 2] = a->samples[i * 2 + 1] = (float)(0.5 * std::sin(2 * M_PI * hz * i / SR));
    return a;
}
ClipPtr audioClip(Project& p, const TrackPtr& t, double start, double seconds, const std::string& name) {
    auto c = makeAudioClip(name, sine(seconds, 220 + start), SR, SR);
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

struct Rig {
    MainWindow* w = nullptr;
    std::vector<TrackPtr> tracks;
    GroupPtr group;
    TrackPtr ret;
    ClipPtr midi, audio;
};

// 12 tracks (audio and MIDI), a group, a return, clips; names: plain, long, "&", non-Latin
const char* kNames[12] = {"Drums & Bass", "Bass", "Guitar", "Vocals", "Keys", "Lead", "Pad", "Arp",
                          "Синтезатор баса", "ドラム 音", "طبول", "🥁 Beat"};
Rig makeRig(int w = 1600, int h = 1000) {
    Rig r;
    r.w = new MainWindow();
    r.w->askBeforeClosing = false;
    r.w->resizeWindow(w, h);
    r.w->moveWindow(0, 0);
    r.w->showWindow();
    Timeline* tl = r.w->timeline;
    Project& p = r.w->project;
    for (int i = 0; i < 12; i++) {
        TrackPtr t = tl->addTrack(i < 4 ? Kind::Audio : Kind::Midi);
        t->name = kNames[i];
        r.tracks.push_back(t);
        ClipPtr c = i < 4 ? audioClip(p, t, i * 0.5, 4, std::string(kNames[i]) + " 1")
                          : midiClip(p, t, i * 0.5, 4, std::string(kNames[i]) + " 1");
        if (!r.audio && i < 4) r.audio = c;
        if (!r.midi && i >= 4) r.midi = c;
    }
    r.group = tl->groupTracks({r.tracks[1], r.tracks[2]});
    if (r.group) r.group->name = "Rhythm & Low end";
    r.ret = tl->addTrack(Kind::Return);
    r.ret->name = "Reverb return";
    tl->lanesChanged();
    tl->tracksChanged.emit();
    settle();
    return r;
}

void closeAllOthers(Window* main) {
    App::get().closeAllPopups();
    for (Window* win : std::vector<Window*>(App::get().windows()))
        if (win != main && win->isWindowShown() && !win->isPopup()) win->hideWindow();
    App::get().hideToolTip();
    settle(1);
}

void closeRig(Rig& r) {
    r.w->closeFxWindows();
    closeAllOthers(r.w);
    delete r.w;
    r.w = nullptr;
    settle(1);
}

void key(Window& w, int k, int mods = 0, const std::string& text = "") {
    plat::Event e;
    e.type = plat::Event::KeyDown;
    e.key = k;
    e.mods = mods;
    e.text = text;
    w.onEvent(nullptr, e);
    e.type = plat::Event::KeyUp;
    w.onEvent(nullptr, e);
}

// =================================================================================================
// A. placement: tool windows over each other, off the screen, a modal over a tool window
// =================================================================================================
void huntPlacement(Rig& r) {
    MainWindow& w = *r.w;
    // 1. the effects windows of 10 tracks, opened one after the other
    std::map<std::pair<int, int>, std::vector<std::string>> at;
    for (int i = 0; i < 10; i++) w.openTrackPlugins(r.tracks[i]);
    settle(2);
    std::vector<Window*> fx;
    for (Window* win : shownWindows())
        if (dynamic_cast<PluginsDialog*>(win)) fx.push_back(win);
    for (Window* win : fx) {
        Rect g = win->screenGeometry();
        at[{g.x, g.y}].push_back(win->title());
    }
    std::string pic = screenPicture("A1-ten-effects-windows");
    for (auto& [pos, titles] : at)
        if (titles.size() > 1)
            found("fx-stacked", "Effects windows of several tracks open exactly on top of each other (no cascade)",
                  std::to_string(titles.size()) + " of " + std::to_string(fx.size()) + " windows at " +
                      std::to_string(pos.first) + "," + std::to_string(pos.second) + " (" + titles.front() + " … " +
                      titles.back() + ")",
                  pic);
    notes.push_back("A1: " + std::to_string(fx.size()) + " effects windows open, at " + std::to_string(at.size()) +
                    " distinct positions");
    // their sizes against a 1366x768 screen
    for (Window* win : fx) {
        Rect o = win->outerScreenGeometry();
        if (o.w > SMALL.w || o.h > SMALL.h)
            found("too-big-small-screen", "Windows larger than a 1366x768 screen at their default size",
                  describeWin(win) + " " + sz(o.w, o.h));
        break;
    }
    closeAllOthers(&w);
    w.closeFxWindows();
    settle(1);

    // 2. the tool windows at once: synth, sampler (with a sample), MIDI editor, Pad Kit, MIDI effects, master FX
    std::vector<std::pair<std::string, std::function<void()>>> tools = {
        {"MIDI editor", [&] { w.openMidiEditor(r.midi); }},
        {"Synth", [&] { synth_dialog::openSynth(r.tracks[4]); }},
        {"Sampler", [&] {
             r.tracks[5]->sampler = sampler_dialog::newSettings(r.audio);
             sampler_dialog::openSampler(r.tracks[5]);
         }},
        {"Pad Kit", [&] { pad_kit_dialog::openPadKit(r.tracks[6]); }},
        {"MIDI effects", [&] { midi_effects_dialog::openMidiEffects(r.tracks[7]); }},
        {"Master effects", [&] { w.openMasterPlugins(); }},
        {"Effects of Drums", [&] { w.openTrackPlugins(r.tracks[0]); }},
        {"Instrument panel", [&] { w.openInstrumentPanel(r.tracks[8]); }},
    };
    std::map<std::string, Rect> placed;
    for (auto& [name, open] : tools) {
        std::vector<Window*> before = shownWindows();
        open();
        settle(2);
        for (Window* win : shownWindows())
            if (std::find(before.begin(), before.end(), win) == before.end() && !win->isPopup()) {
                Rect o = win->outerScreenGeometry();
                placed[name + " (" + describeWin(win) + ")"] = o;
                if (!inside(o, SCREEN))
                    found("offscreen-1080", "A window opens (partly) outside a 1920x1080 screen",
                          name + ": " + describeWin(win) + " at " + rt(o));
                if (o.w > SMALL.w || o.h > SMALL.h)
                    found("too-big-small-screen", "Windows larger than a 1366x768 screen at their default size",
                          name + ": " + describeWin(win) + " " + sz(o.w, o.h));
                // (where it goes on a small screen can't be seen here: the headless screen is always 1920x1080)
            }
    }
    std::string pic2 = screenPicture("A2-tool-windows-at-once");
    // windows sharing the same top-left (the program placed them; the window manager may not help)
    std::map<std::pair<int, int>, std::vector<std::string>> corners;
    for (auto& [n, o] : placed) corners[{o.x, o.y}].push_back(n);
    for (auto& [pos, names] : corners)
        if (names.size() > 1) {
            std::string all;
            for (auto& n : names) all += (all.empty() ? "" : "; ") + n;
            found("tools-stacked", "Different tool windows open at the same place, hiding each other",
                  "at " + std::to_string(pos.first) + "," + std::to_string(pos.second) + ": " + all, pic2);
        }
    // a modal over a tool window: a message box from the synth window
    Window* synth = nullptr;
    for (Window* win : shownWindows())
        if (win->title().find("Synth") != std::string::npos || win->type().find("Synth") != std::string::npos) synth = win;
    if (synth) {
        synth->moveWindow(1300, 600);
        App& app = App::get();
        bool checked = false;
        app.nestedLoop = [&](const std::function<bool()>& done) {
            settle(1);
            Window* modal = app.modalWindow();
            if (modal && !checked) {
                checked = true;
                std::vector<Window*> ws = shownWindows();
                bool top = !ws.empty() && ws.back() == modal;
                Rect mg = modal->outerScreenGeometry(), sg = synth->outerScreenGeometry();
                std::string p = screenPicture("A3-modal-over-synth");
                if (!top) found("modal-not-top", "A modal box is not the topmost window", describeWin(modal), p);
                // a press on the synth window while the modal is open: does anything change order?
                Point c(sg.x + 20, sg.y + 20);
                (void)c;
                if (!sg.contains(Point(mg.x + mg.w / 2, mg.y + mg.h / 2)))
                    found("modal-far", "A message box opened from a tool window is centred on another window",
                          describeWin(modal) + " at " + rt(mg) + ", its tool window at " + rt(sg), p);
            }
            for (int i = 0; i < 5 && !done(); i++)
                if (Window* m = app.modalWindow()) m->close();
        };
        msg::information(synth, "Synth", "A message from the synth window.");
        app.nestedLoop = nullptr;
    }
    closeAllOthers(&w);

    // 3. the MIDI editor at the screen's right: its Note tools panel beside it
    w.openMidiEditor(r.midi);
    settle(2);
    MidiEditor* ed = nullptr;
    for (Window* win : shownWindows())
        if ((ed = dynamic_cast<MidiEditor*>(win))) break;
    if (ed) {
        ed->moveWindow(SCREEN.w - ed->width() - 10, 80);
        ed->openTools();
        settle(2);
        if (ed->toolsPanel) {
            Rect o = ed->toolsPanel->outerScreenGeometry();
            if (!inside(o, SCREEN))
                found("tools-panel-offscreen", "The MIDI editor's Note tools panel opens off the screen when the editor is at its right",
                      "editor at " + rt(ed->outerScreenGeometry()) + ", Note tools at " + rt(o),
                      screenPicture("A4-note-tools-offscreen"));
            ed->toolsPanel->reject();
        }
        ed->hideWindow();
    }
    closeAllOthers(&w);

    // 4. the Sends window of the lowest row's Sends button, the main window filling the screen (maximised)
    w.moveWindow(0, 30);
    w.resizeWindow(1920, 1050);
    settle(2);
    std::vector<SendsButton*> sends;
    std::function<void(Widget*)> walk = [&](Widget* x) {
        if (auto* s = dynamic_cast<SendsButton*>(x))
            if (s->isVisible()) sends.push_back(s);
        for (auto& c : x->children()) walk(c.get());
    };
    walk(&w);
    SendsButton* low = nullptr;
    for (auto* s : sends) {
        int y = s->mapToGlobal(Point(0, s->height())).y;
        if (y < SCREEN.bottom() - 60 && (!low || y > low->mapToGlobal(Point(0, low->height())).y)) low = s;
    }
    if (low) {
        low->showSends();
        settle(1);
        Point g = low->mapToGlobal(Point(0, 0));
        for (Window* p : App::get().popups()) {
            Rect o = p->screenGeometry();
            if (!inside(o, SCREEN))
                found("sends-offscreen", "The Sends popup is placed at its button's bottom-left with no screen check: off the right edge (and the bottom)",
                      "maximised main window, button at " + rt(Rect(g.x, g.y, low->width(), low->height())) + ", popup at " +
                          rt(o) + " on a 1920x1080 screen",
                      screenPicture("A5-sends-popup-offscreen"));
        }
        App::get().closeAllPopups();
    } else {
        notes.push_back("A5: no Sends button found");
    }
    w.moveWindow(0, 0);
    w.resizeWindow(1600, 1000);
    settle(2);
}

// =================================================================================================
// B. menus: every context menu (and submenu) opened near each corner of the screen
// =================================================================================================
struct MenuCase {
    std::string name;
    std::function<void(Menu&)> fill;
};

int menuCount = 0, popupCount = 0;
std::set<std::string> menuPicSaved;

void checkPopupsOnScreen(const std::string& where, const std::string& pic) {
    for (Window* p : App::get().popups()) {
        Rect o = p->screenGeometry();
        if (!inside(o, SCREEN)) {
            std::string kind = o.h > SCREEN.h ? "menu-taller-than-screen" : "menu-offscreen";
            found(kind,
                  o.h > SCREEN.h ? "Menus taller than the screen: cut at the bottom, no scrolling (wheel and arrows ignored)"
                                 : "A menu or submenu opens (partly) outside the screen",
                  where + ": popup " + rt(o) + " (" + std::to_string(Menu::ofWindow(p) ? Menu::ofWindow(p)->actions().size() : 0) +
                      " entries)",
                  pic);
        }
    }
}

// the menu's text problems: an "&" that vanishes (it is taken as a mnemonic marker), disabled items without a reason
void checkMenuText(Menu& m, const std::string& where) {
    for (auto& a : m.actions()) {
        if (a->separator) continue;
        std::string t = a->text;
        size_t amp = t.find('&');
        if (amp != std::string::npos && !(amp + 1 < t.size() && t[amp + 1] == '&') && where.rfind("menu bar", 0) == 0)
            found("menu-mnemonic", "Menu mnemonics (&File, &New…) are stripped and never underlined; Alt+letter does nothing",
                  where + ": \"" + t + "\"");
        else if (amp != std::string::npos && !(amp + 1 < t.size() && t[amp + 1] == '&'))
            found("menu-amp", "An \"&\" in a menu entry's text (a track's or group's name) is not drawn",
                  where + ": \"" + t + "\"");
        if (!a->enabled && !a->section && a->tooltip.empty() && !a->submenu && tui::menuText(t).rfind("(", 0) != 0)
            found("menu-disabled-no-reason", "Disabled menu entries without a reason (no tooltip saying why)",
                  where + ": \"" + tui::menuText(t) + "\"");
        if (a->submenu) checkMenuText(*a->submenu, where + " ▸ " + tui::menuText(t));
    }
}

// opens it at `pos`, then every submenu (keys: Down to it, Right), checking where each popup goes
void exploreMenu(Window& owner, Menu& m, Point pos, const std::string& where, bool picture, bool subs = true) {
    m.popup(pos, &owner);
    settle(1);
    if (!m.isOpen()) return;
    menuCount++;
    std::string pic;
    if (picture && !menuPicSaved.count(where)) {
        menuPicSaved.insert(where);
        pic = screenPicture("B-" + where);
    }
    checkPopupsOnScreen(where, pic);
    if (!subs) {
        App::get().closeAllPopups();
        return;
    }
    // each submenu
    int n = (int)m.actions().size();
    int sel = -1;
    for (int i = 0; i < n; i++) {
        Action* a = m.actions()[i].get();
        if (!a->visible || a->separator || a->section || !a->enabled) continue;
        sel++;
        if (!a->submenu) continue;
        // select it: Home, then Down as many times as selectable items before it
        m.popup(pos, &owner);
        settle(1);
        key(owner, plat::Key_Home);
        for (int k = 0; k < sel; k++) key(owner, plat::Key_Down);
        key(owner, plat::Key_Right);
        settle(1);
        popupCount++;
        std::string sub = where + " ▸ " + tui::menuText(a->text);
        std::string spic;
        if (picture && App::get().popups().size() > 1 && !menuPicSaved.count(sub)) {
            menuPicSaved.insert(sub);
            spic = screenPicture("B-" + sub);
        }
        checkPopupsOnScreen(sub, spic);
        // a submenu overlapping its parent's items (flipped onto it)
        auto& ps = App::get().popups();
        if (ps.size() >= 2) {
            Rect pr = ps[ps.size() - 2]->screenGeometry(), cr = ps.back()->screenGeometry();
            Rect in = pr.intersected(cr);
            if (in.w > 8 && in.h > 8)
                found("submenu-covers-parent", "A submenu is drawn over its parent menu",
                      sub + ": parent " + rt(pr) + ", submenu " + rt(cr), spic);
        }
        App::get().closeAllPopups();
        settle(1);
    }
    App::get().closeAllPopups();
    settle(1);
}

void huntMenus(Rig& r) {
    MainWindow& w = *r.w;
    Timeline* tl = w.timeline;
    // 100 tracks: menus that list tracks (outputs, sends, sidechains) grow with them
    for (int i = 0; i < 88; i++) {
        TrackPtr t = w.project.addTrack(i % 2 ? Kind::Midi : Kind::Audio);
        t->name = "Track " + std::to_string(i + 13);
    }
    for (int i = 0; i < 6; i++) {
        TrackPtr t = w.project.addTrack(Kind::Return);
        t->name = "Return " + std::to_string(i + 2);
    }
    tl->lanesChanged();
    tl->tracksChanged.emit();
    settle(2);
    std::vector<Lane> ls = tl->lanes();
    const Lane* autoLane = nullptr;
    const Lane* takeLane = nullptr;
    (void)takeLane;
    for (auto& l : ls)
        if (l.automation && !autoLane) autoLane = &l;
    std::vector<MenuCase> cases = {
        {"track menu (Drums & Bass)", [&](Menu& m) { w.panel->fillTrackMenu(m, r.tracks[0]); }},
        {"track menu (Keys, MIDI)", [&](Menu& m) { w.panel->fillTrackMenu(m, r.tracks[4]); }},
        {"track menu (return)", [&](Menu& m) { w.panel->fillTrackMenu(m, r.ret); }},
        {"output menu (Bass)", [&](Menu& m) { w.panel->fillOutputMenu(m, r.tracks[1]); }},
        {"group menu", [&](Menu& m) { if (r.group) w.panel->fillGroupMenu(m, r.group); }},
        {"clip menu (audio)", [&](Menu& m) {
             tl->selection.selectOnly(r.audio);
             tl->fillMenu(m, std::nullopt);
         }},
        {"clip menu (MIDI)", [&](Menu& m) {
             tl->selection.selectOnly(r.midi);
             tl->fillMenu(m, std::nullopt);
         }},
        {"empty timeline menu", [&](Menu& m) {
             tl->selection.clear();
             tl->fillMenu(m, std::nullopt);
         }},
        {"ruler menu", [&](Menu& m) { tl->fillRulerMenu(m, PointF(200, 5)); }},
        {"tempo lane menu", [&](Menu& m) { tl->fillTempoMenu(m, 2.0, std::nullopt); }},
        {"instrument menu", [&](Menu& m) { w.fillInstrumentMenu(m, r.tracks[4]); }},
        {"grid menu", [&](Menu& m) { w.fillGridMenu(&m); }},
    };
    // the windows' own menus: the patch space's, a pad's, the MIDI editor's Tools
    w.openTrackPlugins(r.tracks[0]);
    if (!r.tracks[6]->padKit) r.tracks[6]->padKit = std::make_shared<PadKit>();
    pad_kit_dialog::openPadKit(r.tracks[6]);
    w.openMidiEditor(r.midi);
    settle(2);
    PluginsDialog* fxw = nullptr;
    pad_kit_dialog::PadKitDialog* padw = nullptr;
    MidiEditor* edw = nullptr;
    for (Window* x : shownWindows()) {
        if (!fxw) fxw = dynamic_cast<PluginsDialog*>(x);
        if (!padw) padw = dynamic_cast<pad_kit_dialog::PadKitDialog*>(x);
        if (!edw) edw = dynamic_cast<MidiEditor*>(x);
    }
    if (fxw) cases.push_back({"patch space menu", [fxw](Menu& m) { fxw->fillSpaceMenu(m, Point(100, 100)); }});
    if (padw) cases.push_back({"pad menu", [padw](Menu& m) { padw->fillPadMenu(m, 0); }});
    if (edw) cases.push_back({"MIDI editor Tools menu", [edw](Menu& m) { edw->toolsMenu(m); }});
    if (autoLane) {
        Lane l = *autoLane;
        cases.push_back({"automation lane menu", [&, l](Menu& m) { tl->fillAutomationMenu(m, l, PointF(100, (float)l.top + 5)); }});
    }
    // the menu bar's menus too
    for (auto& mb : w.menuBar->menus()) {
        Menu* mm = mb.get();
        cases.push_back({"menu bar " + tui::menuText(mm->title), [mm](Menu& m) { (void)m; (void)mm; }});
    }
    const Point corners[] = {Point(5, 5), Point(SCREEN.w - 5, 5), Point(5, SCREEN.h - 5), Point(SCREEN.w - 5, SCREEN.h - 5),
                             Point(SCREEN.w / 2, SCREEN.h / 2)};
    const char* cornerNames[] = {"top-left", "top-right", "bottom-left", "bottom-right", "centre"};
    for (auto& c : cases) {
        if (elapsed() > 16) { notes.push_back("menus: " + c.name + " and after skipped (time)"); break; }
        Menu own;
        Menu* m = &own;
        if (c.name.rfind("menu bar ", 0) == 0) {
            for (auto& mb : w.menuBar->menus())
                if ("menu bar " + tui::menuText(mb->title) == c.name) m = mb.get();
        } else {
            c.fill(own);
        }
        if (m->aboutToShow) m->aboutToShow(m);
        checkMenuText(*m, c.name);
        for (int k = 0; k < 5; k++) exploreMenu(w, *m, corners[k], c.name + " at " + cornerNames[k], k == 3, k == 3 || k == 0);
    }
    notes.push_back("B: " + std::to_string(menuCount) + " menus opened, " + std::to_string(popupCount) + " submenus");
    // a menu with hundreds of entries, as a long list of presets or plugins makes it (the toolkit's own behaviour)
    Menu big;
    for (int i = 0; i < 300; i++) big.addAction("Preset " + std::to_string(i + 1));
    big.popup(Point(400, 100), &w);
    settle(1);
    if (!App::get().popups().empty()) {
        Window* p = App::get().popups().back();
        Rect o = p->screenGeometry();
        // the keys: End selects the last entry: is it shown?
        key(w, plat::Key_End);
        settle(1);
        std::string pic = screenPicture("B-300-entries");
        if (!inside(o, SCREEN))
            found("menu-taller-than-screen", "Menus taller than the screen: cut at the bottom, no scrolling (wheel and arrows ignored)",
                  "a menu of 300 entries: popup " + rt(o) + " on a screen of 1920x1080; End selects the 300th, drawn at y≈" +
                      std::to_string(o.y + o.h - 10) + " (off the screen)",
                  pic);
    }
    App::get().closeAllPopups();
    // remove the extra tracks again
    settle(1);
}

// =================================================================================================
// C. tooltips: every widget's in the main window and the tool windows, at the screen's bottom-right
// =================================================================================================
void huntTooltips(Rig& r) {
    MainWindow& w = *r.w;
    App& app = App::get();
    std::vector<Window*> wins{&w};
    synth_dialog::openSynth(r.tracks[4]);
    w.openTrackPlugins(r.tracks[0]);
    r.tracks[5]->sampler = sampler_dialog::newSettings(r.audio);
    sampler_dialog::openSampler(r.tracks[5]);
    settle(2);
    for (Window* x : shownWindows())
        if (x != &w && !x->isPopup()) wins.push_back(x);
    int n = 0, wide = 0;
    std::string pic;
    for (Window* win : wins) {
        std::function<void(Widget*)> walk = [&](Widget* x) {
            if (!x->isVisible()) return;
            std::string t = x->toolTip();
            if (!t.empty()) {
                n++;
                app.showToolTip(Point(SCREEN.w - 3, SCREEN.h - 3), t, x);
                Window* tip = app.toolTipWindow();
                if (tip) {
                    Rect o = tip->screenGeometry();
                    if (!inside(o, SCREEN))
                        found("tooltip-offscreen", "A tooltip goes outside the screen", audit::describe(x) + ": " + rt(o));
                    if (o.w > 640) {
                        wide++;
                        if (pic.empty() || o.w > SMALL.w) {
                            pic = OUT + "/C-tooltip-" + std::to_string(o.w) + "px.png";
                            tip->invalidate(tip->outerRect());
                            tip->flush();
                            savePng(pic, tip->framebuffer());
                        }
                        found(o.w > SMALL.w ? "tooltip-wider-than-small-screen" : "tooltip-no-wrap",
                              o.w > SMALL.w ? "Tooltips wider than a 1366 px screen (cut at both sides there)"
                                            : "Long tooltips are never wrapped: one line up to the screen's width",
                              audit::describe(x) + " in " + describeWin(win) + ": " + std::to_string(o.w) + " px wide: \"" +
                                  audit::shortText(t, 90) + "\"",
                              pic);
                    }
                }
            }
            for (auto& c : x->children()) walk(c.get());
        };
        walk(win);
    }
    app.hideToolTip();
    // a 300-character tooltip with no line breaks, as a long plugin path or file name gives one
    std::string longTip = "/home/user/Music/Samples/" + std::string(260, 'x') + ".wav";
    app.showToolTip(Point(10, 10), longTip, &w);
    if (Window* tip = app.toolTipWindow()) {
        Rect o = tip->screenGeometry();
        if (!inside(o, SCREEN))
            found("tooltip-offscreen", "A tooltip goes outside the screen",
                  "a 290-character path as a tooltip: " + rt(o) + " (no wrapping, no eliding)");
    }
    app.hideToolTip();
    notes.push_back("C: " + std::to_string(n) + " tooltips shown at the screen's corner, " + std::to_string(wide) + " wider than 640 px");
    closeAllOthers(&w);
    w.closeFxWindows();
}


// =================================================================================================
// D. focus: the ring visible on each Tab stop; Tab following the visual order
// =================================================================================================
std::vector<Widget*> tabChain(Window& win) {
    std::vector<Widget*> chain;
    std::function<void(Widget*)> walk = [&](Widget* w) {
        if (!w->isVisible() || w->deleted_ || !w->isEnabled()) return;
        if (w != &win && !w->focusProxy &&
            (w->focusPolicy() == FocusPolicy::Tab || w->focusPolicy() == FocusPolicy::Strong))
            chain.push_back(w);
        for (auto& c : w->children()) walk(c.get());
    };
    walk(&win);
    return chain;
}

Rect inWindow(Widget* w) {
    Point p = w->mapToWindow(Point(0, 0));
    return Rect(p.x, p.y, w->width(), w->height());
}

// does any pixel around the widget change when it gets the focus?
bool focusShows(Window& win, Widget* w) {
    Rect r = inWindow(w);
    r = Rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6).intersected(win.rect());
    if (r.empty()) return true;
    auto grab = [&] {
        win.invalidate(win.rect());
        win.flush();
        const Image& fb = win.framebuffer();
        float s = win.scale();
        std::vector<uint32_t> px;
        for (int y = (int)(r.y * s); y < (int)(r.bottom() * s) && y < fb.height(); y++)
            for (int x = (int)(r.x * s); x < (int)(r.right() * s) && x < fb.width(); x++) {
                Color c = fb.colorAt(x, y);
                px.push_back((uint32_t)c.r << 24 | (uint32_t)c.g << 16 | (uint32_t)c.b << 8 | c.a);
            }
        return px;
    };
    win.setFocusWidget(nullptr);
    App::get().doLayouts();
    auto before = grab();
    win.setFocusWidget(w);
    App::get().doLayouts();
    auto after = grab();
    return before != after;
}

void huntFocus(Window& win, const std::string& name) {
    App::get().doLayouts();
    std::vector<Widget*> chain = tabChain(win);
    if (chain.empty()) return;
    Widget* keep = win.focusWidget();
    std::map<std::string, int> invisible;
    std::string pic;
    int checkedN = 0;
    for (Widget* w : chain) {
        if (checkedN++ > 60) break;
        if (!focusShows(win, w)) {
            std::string t = w->type();
            if (invisible[t]++ == 0) {
                if (pic.empty()) pic = windowPicture(win, "D-focus-" + name);
                found("focus-invisible-" + t, "No visible focus ring on " + t + " (Tab reaches it, nothing changes on screen)",
                      name + ": " + audit::describe(w) + " at " + rt(inWindow(w)), pic);
            }
        }
    }
    // Tab order against the reading order: a step up and to the left (or back left on the same row)
    int jumps = 0;
    for (size_t i = 0; i + 1 < chain.size(); i++) {
        Rect a = inWindow(chain[i]), b = inWindow(chain[i + 1]);
        int acy = a.y + a.h / 2, bcy = b.y + b.h / 2;
        bool sameRow = std::abs(acy - bcy) < std::min(a.h, b.h) / 2 + 2;
        bool up = bcy < a.y - 2;
        bool left = b.right() <= a.x + 2;
        bool rightCol = b.x >= a.right() - 2;
        if ((sameRow && left) || (up && !rightCol)) {
            if (jumps++ < 3)
                found("tab-order", "Tab order doesn't follow the visual order (it jumps back up or left)",
                      name + ": Tab from " + audit::describe(chain[i]) + " at " + rt(a) + " goes to " +
                          audit::describe(chain[i + 1]) + " at " + rt(b),
                      windowPicture(win, "D-taborder-" + name));
        }
    }
    win.setFocusWidget(keep);
    notes.push_back("D: " + name + ": " + std::to_string(chain.size()) + " Tab stops, " + std::to_string(jumps) +
                    " jumps against the reading order");
}

// =================================================================================================
// E. window contents: the layout checker on a window, its value boxes at their extremes
// =================================================================================================
void auditContents(Window& win, const std::string& name) {
    audit::Report rep;
    rep.context = name;
    App::get().doLayouts();
    audit::checkTree(&win, rep);
    std::string pic;
    for (auto& k : rep.order) {
        auto& e = rep.entries[k];
        if (e.f.kind != "text" && e.f.kind != "clipped" && e.f.kind != "overlap" && e.f.kind != "elided") continue;
        if (pic.empty()) pic = windowPicture(win, "E-" + name);
        found("content-" + e.f.kind + "-" + name, name + ": " + e.f.kind + " (layout checker)",
              e.f.widget + " at " + e.f.path + ": " + e.f.detail, pic);
    }
}

void spinExtremes(Window& win, const std::string& name) {
    std::vector<SpinBox*> boxes;
    std::function<void(Widget*)> walk = [&](Widget* w) {
        if (!w->isVisible()) return;
        if (auto* s = dynamic_cast<SpinBox*>(w)) boxes.push_back(s);
        for (auto& c : w->children()) walk(c.get());
    };
    walk(&win);
    int n = 0;
    for (SpinBox* s : boxes) {
        double keep = s->value();
        for (double v : {s->minimum(), s->maximum(), (s->minimum() + s->maximum()) / 2 + 0.123456}) {
            s->setValueSilently(v);
            App::get().doLayouts();
            LineEdit* e = s->editor();
            if (!e) continue;
            const css::Style& st = e->computedStyle();
            style::Insets pad = style::insets(st, "padding"), bw = style::insets(st, "border-width");
            int need = style::textWidth(e->font(), s->text());
            int room = e->width() - pad.w() - bw.w();
            n++;
            if (need > room + 1)
                found("spin-cut", "Number boxes too narrow for their own extreme values (the text is cut)",
                      name + ": " + audit::describe(s) + " showing \"" + s->text() + "\" needs " + std::to_string(need) +
                          " px, has " + std::to_string(room),
                      windowPicture(win, "E-spin-" + name));
        }
        s->setValueSilently(keep);
    }
    notes.push_back("E: " + name + ": " + std::to_string(boxes.size()) + " number boxes, " + std::to_string(n) + " values checked");
}

// =================================================================================================
// F. combo lists: many entries, very long entries, near the screen's bottom
// =================================================================================================
void huntCombos(MainWindow& w) {
    Dialog d(&w, "Combo test", 300, 120);
    auto* lay = vbox(&d, 8, 8);
    auto* many = d.add<ComboBox>();
    for (int i = 0; i < 500; i++) many->addItem("MIDI port " + std::to_string(i + 1));
    auto* longOne = d.add<ComboBox>();
    longOne->addItem("Short");
    longOne->addItem("A plugin with a very long name " + std::string(170, 'W'));
    lay->addWidget(many);
    lay->addWidget(longOne);
    d.resizeWindow(300, 120);
    d.open();
    d.moveWindow(1700, SCREEN.bottom() - 125);
    settle(1);
    for (ComboBox* c : {many, longOne}) {
        c->showPopup();
        settle(1);
        for (Window* p : App::get().popups()) {
            Rect o = p->screenGeometry();
            if (!inside(o, SCREEN))
                found("combo-offscreen", "A drop-down list goes off the screen",
                      std::to_string(c->count()) + " entries, widest " + std::to_string(o.w) + " px: list at " + rt(o),
                      screenPicture("F-combo-" + std::to_string(c->count())));
        }
        App::get().closeAllPopups();
    }
    d.reject();
}

// =================================================================================================
// G. other windows: the sampler with a sample, a plugin's, the colour picker, history (100 steps), the palette
// (none / every command), shortcuts, message boxes with long text
// =================================================================================================
Window* newestShown(const std::vector<Window*>& before) {
    Window* got = nullptr;
    for (Window* x : shownWindows())
        if (std::find(before.begin(), before.end(), x) == before.end() && !x->isPopup()) got = x;
    return got;
}

void checkOne(Window* win, const std::string& name, bool contents = true) {
    if (!win) {
        notes.push_back("G: " + name + ": no window");
        return;
    }
    App::get().doLayouts();
    windowPicture(*win, "G-" + name);
    if (contents) auditContents(*win, name);
    spinExtremes(*win, name);
    huntFocus(*win, name);
}

void huntWindows(Rig& r) {
    MainWindow& w = *r.w;
    App& app = App::get();
    auto open = [&](const std::string& name, const std::function<void()>& fn, bool contents = true) {
        if (elapsed() > 27) { notes.push_back("G: " + name + " skipped (time)"); return; }
        std::vector<Window*> before = shownWindows();
        bool modalSeen = false;
        app.nestedLoop = [&](const std::function<bool()>& done) {  // (a modal one: checked inside its loop)
            settle(1);
            if (Window* m = app.modalWindow(); m && !modalSeen) {
                modalSeen = true;
                checkOne(m, name, contents);
            }
            for (int i = 0; i < 5 && !done(); i++)
                if (Window* m = app.modalWindow()) m->close();
        };
        fn();
        app.nestedLoop = nullptr;
        settle(2);
        if (!modalSeen) checkOne(newestShown(before), name, contents);
        closeAllOthers(&w);
    };
    // the sampler, given a sample
    r.tracks[5]->sampler = sampler_dialog::newSettings(r.audio);
    open("sampler", [&] { sampler_dialog::openSampler(r.tracks[5]); });
    open("synth", [&] { synth_dialog::openSynth(r.tracks[4]); });
    if (!r.tracks[6]->padKit) r.tracks[6]->padKit = std::make_shared<PadKit>();
    open("pad-kit", [&] { pad_kit_dialog::openPadKit(r.tracks[6]); });
    open("effects-window", [&] { w.openTrackPlugins(r.tracks[0]); });
    // built-in effects' windows and the test plugins' (check::testDir())
    {
        std::vector<plug::PluginInfo> infos = {
            plug::PluginInfo{"builtin:eq", "EQ", plug::EFFECT, ""},
            plug::PluginInfo{"builtin:compressor", "Compressor", plug::EFFECT, ""},
            plug::PluginInfo{util::absolutePath(std::string(check::testDir()) + "/TinyTestSynth.so"), "TinyTestSynth", plug::INSTRUMENT, ""},
        };
        for (auto& info : infos) {
            SlotPtr slot = w.plugins->load(info);
            if (!slot) { notes.push_back("G: plugin " + info.name + " didn't load"); continue; }
            slot->name = info.name + " — a plugin with a long name " + std::string(150, 'x');
            open("plugin-" + info.name, [&] { w.plugins->edit(slot); });
        }
    }
    open("colour-picker", [&] {
        auto* d = new ColorPickerDialog(&w, "#3366cc", "", ColorSubject::Track);
        d->deleteOnClose = true;
        d->open();
    });
    // history with 100 steps
    for (int i = 0; i < 100; i++) {
        r.tracks[1]->name = "Bass take " + std::to_string(i);
        r.tracks[1]->volumeDb = -0.1 * i;
        w.noteChanges();
    }
    open("history-100", [&] { w.showHistory(); });
    // the palette: no matches, every command
    {
        shortcuts_dialog::CommandPalette pal(&w, *w.commands);
        pal.open();
        settle(1);
        pal.search->setText("");
        pal.refill();
        settle(1);
        notes.push_back("G: palette with an empty search lists " + std::to_string(pal.list->count()) + " commands");
        checkOne(&pal, "palette-all");
        pal.search->setText("zzqqxx no such command");
        pal.refill();
        settle(1);
        windowPicture(pal, "G-palette-none");
        if (pal.list->count() == 0 && pal.info && pal.info->text().find("o match") == std::string::npos &&
            pal.info->text().find("othing") == std::string::npos)
            found("palette-empty-silent", "The palette with no matches shows an empty list and no message (its hint still says Enter: run it)",
                  "search \"zzqqxx no such command\": 0 rows, info \"" + pal.info->text() + "\"", OUT + "/G-palette-none.png");
        notes.push_back("G: palette, no matches: info \"" + (pal.info ? pal.info->text() : std::string()) + "\"");
        pal.reject();
    }
    open("shortcuts", [&] { w.showShortcuts(); });
    // message boxes: long text, an unbroken path, many lines
    std::vector<std::pair<std::string, std::string>> boxes = {
        {"long", "The project couldn't be saved because " +
                     [] { std::string s; for (int i = 0; i < 120; i++) s += "the disk was full and "; return s; }()},
        {"path", "Couldn't read /home/user/Music/Projects/" + std::string(220, 'a') + "/sample.wav"},
        {"lines", [] { std::string s; for (int i = 0; i < 80; i++) s += "Missing: sample " + std::to_string(i) + ".wav\n"; return s; }()},
    };
    for (auto& [kind, text] : boxes) {
        bool seen = false;
        app.nestedLoop = [&](const std::function<bool()>& done) {
            settle(1);
            if (Window* m = app.modalWindow(); m && !seen) {
                seen = true;
                Rect o = m->outerScreenGeometry();
                std::string pic = screenPicture("G-message-" + kind);
                if (!inside(o, SCREEN))
                    found("msgbox-offscreen", "Message boxes grow with their text past the screen (no scrolling, not resizable)",
                          kind + " text (" + std::to_string(text.size()) + " chars): box " + rt(o), pic);
                else if (!inside(o, SMALL))
                    found("msgbox-offscreen-small", "Message boxes with long text don't fit a 1366x768 screen",
                          kind + " text: box " + rt(o), pic);
                huntFocus(*m, "message-box-" + kind);
            }
            for (int i = 0; i < 5 && !done(); i++)
                if (Window* m = app.modalWindow()) m->close();
        };
        msg::box(&w, msg::Icon::Warning, "Warning", text, msg::Ok | msg::Cancel, msg::Ok);
        app.nestedLoop = nullptr;
    }
    huntCombos(w);
    // every dialog the menu bar opens (entries ending in "…"; nothing that leaves, scans, deletes or runs
    // other programs): its focus ring and Tab order
    std::function<void(Menu&, const std::string&, int)> walkMenu = [&](Menu& m, const std::string& path, int depth) {
        if (depth > 2) return;
        if (m.aboutToShow) m.aboutToShow(&m);
        std::vector<Action*> acts;
        for (auto& a : m.actions()) acts.push_back(a.get());
        for (Action* a : acts) {
            if (a->separator || a->section || !a->visible) continue;
            std::string t = tui::menuText(a->text);
            if (a->submenu) { walkMenu(*a->submenu, path + "/" + t, depth + 1); continue; }
            if (!a->enabled || t.find("…") == std::string::npos) continue;
            bool unsafe = false;
            for (const char* s : {"Quit", "Exit", "Edit this skin", "folder", "Install", "pdate", "Rescan", "Scan", "Report",
                                  "ebsite", "Open", "Restart", "Wine", "Export", "Import", "Delete", "Remove", "Clear",
                                  "Freeze", "Bounce", "Render", "Record", "Add file", "archive", "Collect", "Save as…",
                                  "Save as t"})
                unsafe = unsafe || t.find(s) != std::string::npos;
            if (unsafe) continue;
            open("menu-" + fileSafe(path + "/" + t), [a] { Menu::activate(a); }, false);
        }
    };
    for (auto& m : w.menuBar->menus()) walkMenu(*m, tui::menuText(m->title), 0);
}

// =================================================================================================
// H. names: non-Latin scripts the interface font can't draw; a 200-character track name in its windows' frames
// =================================================================================================
void huntNames(Rig& r) {
    MainWindow& w = *r.w;
    Font f = theme::uiFont();
    struct Script { const char* name; const char* sample; };
    for (Script s : {Script{"Cyrillic", "Ж"}, Script{"Greek", "Ω"}, Script{"CJK (Japanese)", "音"}, Script{"Hangul", "한"},
                     Script{"Arabic", "ط"}, Script{"Hebrew", "ש"}, Script{"Devanagari", "क"}, Script{"Thai", "ก"},
                     Script{"emoji", "🥁"}}) {
        size_t i = 0;
        uint32_t cp = utf8Next(s.sample, i);
        if (!f.hasGlyph(cp))
            found("no-glyph", "Track names in some scripts are drawn as hollow boxes (no font for them in the fallback list)",
                  std::string(s.name) + " (\"" + s.sample + "\")", "");
    }
    // a 200-character name, framed windows (title bars drawn as with a display)
    App::get().setFramesWhenHeadless(true);
    r.tracks[4]->name = "Keys " + std::string(195, 'K');
    r.tracks[9]->name = "ドラム 音 — طبول — 🥁 Beat";
    for (auto& [name, fn] : std::vector<std::pair<std::string, std::function<void()>>>{
             {"long-name-synth", [&] { synth_dialog::openSynth(r.tracks[4]); }},
             {"long-name-effects", [&] { w.openTrackPlugins(r.tracks[4]); }},
             {"non-latin-effects", [&] { w.openTrackPlugins(r.tracks[9]); }}}) {
        std::vector<Window*> before = shownWindows();
        fn();
        settle(2);
        if (Window* win = newestShown(before)) {
            std::string pic = windowPicture(*win, "H-" + name);
            if (TitleBar* tb = win->titleBar()) {
                int need = style::textWidth(theme::uiFont(), win->title());
                if (need > tb->width())
                    notes.push_back("H: " + name + ": title " + std::to_string(need) + " px in a " +
                                    std::to_string(tb->width()) + " px title bar (look at " + pic + ")");
            }
            auditContents(*win, name);
        }
        closeAllOthers(&w);
        w.closeFxWindows();
    }
    App::get().setFramesWhenHeadless(false);
    settle(1);
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
    auto timed = [](const char* name, const std::function<void()>& fn) {
        double t0 = elapsed();
        fn();
        printf("window hunt: %s done in %.1f s (%zu findings so far)\n", name, elapsed() - t0, hits.size());
        fflush(stdout);
    };
    {
        Rig r = makeRig();
        timed("placement", [&] { huntPlacement(r); });
        timed("tooltips", [&] { huntTooltips(r); });
        timed("windows", [&] { huntWindows(r); });
        timed("names", [&] { huntNames(r); });
        timed("menus", [&] { huntMenus(r); });
        closeRig(r);
    }
    std::ofstream o(OUT + "/raw-findings.md");
    o << "# Window hunt: raw findings (tests/test_window_hunt.cpp)\n\n";
    for (auto& n : notes) o << "- " << n << "\n";
    o << "\n";
    for (auto& id : order) {
        Hit& h = hits[id];
        o << "## " << h.title << "  [" << h.id << "] ×" << h.count << "\n";
        if (!h.picture.empty()) o << "picture: " << h.picture << "\n";
        for (auto& e : h.examples) o << "- " << e << "\n";
        o << "\n";
    }
    printf("window hunt: %zu findings in %.1f s: %s/raw-findings.md\n", hits.size(), elapsed(), OUT.c_str());
    CHECK(true);
    TEST_MAIN_END();
}
