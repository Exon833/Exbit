// The hidden testing aid of test_script.h.
#include "ui/test_script.h"

#include "effects/effects.h"

#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#include "core/util.h"
#include "gfx/image.h"
#include "plugins/wine.h"
#include "ui/autosave.h"
#include "ui/instrument_menu.h"
#include "ui/main_window.h"
#include "ui/master_strip.h"
#include "ui/plugin_dialog.h"
#include "ui/plugin_host.h"
#include "ui/skin.h"
#include "ui/templates.h"
#include "ui/timeline.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"

using ui::App;

namespace test_script {
namespace {

struct Runner {
    MainWindow* main;
    std::vector<std::string> lines;
    size_t next = 0;
    double resumeAt = 0;   // seconds (plat::now)
    int framesLeft = 0;
    std::string target = "main";
    int timer = -1, frame = -1;
    bool done = false;

    void say(const std::string& s) {
        printf("[script] %s\n", s.c_str());
        fflush(stdout);
    }

    ui::Window* topWindow() {
        App& app = App::get();
        if (!app.popups().empty()) return app.popups().back();
        if (ui::Window* m = app.modalWindow()) return m;
        ui::Window* best = nullptr;
        for (ui::Window* w : app.windows())
            if (w->isWindowShown() && !w->isPopup() && w != app.toolTipWindow() &&
                (!best || w->showSerial_ > best->showSerial_))
                best = w;
        return best ? best : main;
    }
    ui::Window* targetWindow() {
        if (target == "main") return main;
        if (target == "top") return topWindow();
        for (ui::Window* w : App::get().windows())
            if (w->isWindowShown() && w->title().find(target) != std::string::npos) return w;
        return nullptr;
    }
    ui::Window* keyWindow() {
        App& app = App::get();
        if (!app.popups().empty()) return app.popups().back();
        if (ui::Window* m = app.modalWindow()) return m;
        if (target != "main") {
            if (ui::Window* t = targetWindow()) return t;
        }
        if (ui::Window* a = app.activeWindow()) return a;
        return main;
    }

    void send(ui::Window* w, plat::Event e) {
        e.time = plat::now();
        w->onEvent(w->native(), e);
    }
    // (script coordinates are the window's logical pixels; the platform sends device pixels)
    int dev(ui::Window* w, int v) { return w->toDevice(v); }
    void mouse(ui::Window* w, plat::Event::Type type, int x, int y, int button, int buttons, int mods = 0) {
        plat::Event e;
        e.type = type;
        e.x = dev(w, x);
        e.y = dev(w, y);
        e.button = button;
        e.buttons = buttons;
        e.mods = mods;
        send(w, e);
    }
    void key(ui::Window* w, const ui::KeySequence& ks, const std::string& text) {
        plat::Event e;
        e.type = plat::Event::KeyDown;
        e.key = ks.key;
        e.mods = ks.mods;
        e.text = text;
        send(w, e);
        if (!ui::Widget::alive(w)) return;
        e.type = plat::Event::KeyUp;
        send(w, e);
    }

    bool shot(ui::Window* w, const std::string& path) {
        Image img;
        bool native = w->native() && w->native()->capture(img) && img.width() > 0;
        if (!native) img = w->framebuffer();
        bool ok = savePng(path, img);
        say("shot " + path + " (" + w->title() + ", " + std::to_string(img.width()) + "x" +
            std::to_string(img.height()) + (native ? ", native" : ", framebuffer") + ")" + (ok ? "" : " FAILED"));
        return ok;
    }

    TrackPtr track(const std::string& index) {
        int i = atoi(index.c_str());
        if (i >= 0 && i < (int)main->project.tracks.size()) return main->project.tracks[i];
        return nullptr;
    }
    PluginsDialog* fxWindow(const std::string& which) {
        TrackPtr t = which == "master" ? nullptr : track(which);
        if (which != "master" && !t) return nullptr;
        if (t) main->openTrackPlugins(t);
        else main->openMasterPlugins();
        auto it = main->fxWindows.find({0, t.get()});
        return it == main->fxWindows.end() ? nullptr : it->second;
    }

    void print() {
        App& app = App::get();
        say("windows:");
        for (ui::Window* w : app.windows()) {
            Rect g = w->screenGeometry();
            printf("    '%s' %s%s%s %dx%d at %d,%d\n", w->title().c_str(), w->isWindowShown() ? "shown" : "hidden",
                   w->isModal() ? " modal" : "", w->isPopup() ? " popup" : "", g.w, g.h, g.x, g.y);
        }
        printf("    status: %s\n", main->status->message().c_str());
        printf("    title: %s, sr %d, playing %d, pos %lld, device open %d\n", main->title().c_str(), main->engine.sr,
               (int)main->engine.playing.load(), (long long)main->engine.pos.load(), (int)main->engine.isOpen());
        int i = 0;
        for (auto& t : main->project.tracks) {
            int clips = 0;
            for (auto& c : main->project.clips)
                if (c->track == t) ++clips;
            printf("    track %d: %s (%s) %d clips, %d effects, instrument %s\n", i++, t->name.c_str(),
                   t->kind == Kind::Midi ? "MIDI" : t->kind == Kind::Return ? "return" : "audio", clips, (int)t->effects.size(),
                   t->kind != Kind::Midi ? "-" : t->needsInstrument ? "(none)" : t->instrument ? t->instrument->name.c_str() : "built-in synth");
        }
        printf("    master effects: %d; unsaved changes: %d\n", (int)main->project.masterEffects.size(),
               (int)main->hasUnsavedChanges());
        printf("    project: %s; undo steps %d, redo steps %d; scale %g; autosave folder: %s\n",
               main->projectPath.empty() ? "(untitled)" : main->projectPath.c_str(), main->history->undoSteps(),
               main->history->redoSteps(), main->scale(), main->autosaver->folder().c_str());
        printf("    recent:");
        for (auto& r : templates::recent(main->prefs, true)) printf(" [%s]", r.c_str());
        printf("\n    templates:");
        for (auto& t : templates::list()) printf(" [%s%s]", t.name.c_str(), t.own ? "" : " (yours)");
        printf("\n");
        fflush(stdout);
    }

    void call(std::istringstream& in) {
        std::string what;
        in >> what;
        std::string rest;
        std::getline(in, rest);
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        Timeline* tl = main->timeline;
        if (what == "addTrack") tl->addTrack(rest == "midi" ? Kind::Midi : Kind::Audio);
        else if (what == "addFile") tl->addFile(rest, tl->cursor);
        else if (what == "play") {
            if (!main->engine.playing) main->togglePlay();
        } else if (what == "stop") main->stop();
        else if (what == "home") main->goToStart();
        else if (what == "save") main->saveProjectFile(rest);
        else if (what == "open") main->openProjectFile(rest);
        else if (what == "new") main->newProject();
        else if (what == "seek") tl->seek((int64_t)(atof(rest.c_str()) * main->engine.sr));
        else if (what == "explorer") main->setExplorerVisible(rest != "off");
        else if (what == "volume") master_volume(atoi(rest.c_str()));
        else if (what == "resize") {
            int w = 0, h = 0;
            std::istringstream(rest) >> w >> h;
            main->resizeWindow(w, h);
        } else if (what == "skin") main->chooseSkin(skin::find(rest));
        else if (what == "midiEditor") {
            for (auto& c : main->project.clips)
                if (c->track && c->track->kind == Kind::Midi) {
                    main->openMidiEditor(c);
                    return;
                }
            say("no MIDI clip");
        } else if (what == "fx") {
            if (!fxWindow(rest)) say("no such FX window");
        } else if (what == "addEffect") {
            std::istringstream r(rest);
            std::string which, path;
            r >> which;
            std::getline(r, path);
            while (!path.empty() && path.front() == ' ') path.erase(path.begin());
            PluginsDialog* d = fxWindow(which);
            if (!d) return say("no such FX window");
            const plug::PluginInfo* info = nullptr;
            for (auto& p : main->plugins->library().plugins)  // by name first, else by path
                if (!info && p.name == path) info = &p;
            for (auto& p : main->plugins->library().plugins)
                if (!info && (p.path == path || p.path.find(path) != std::string::npos)) info = &p;
            plug::PluginInfo builtin;  // (built-in effects aren't in the library: "builtin:<key>" or the key)
            if (!info) {
                std::string key = path.rfind("builtin:", 0) == 0 ? path.substr(8) : path;
                if (const auto* e = effects::find(key)) {
                    builtin = {e->path(), e->name, plug::EFFECT, ""};
                    info = &builtin;
                }
            }
            if (!info) return say("no such plugin: " + path);
            SlotPtr slot = main->plugins->load(*info);
            if (!slot) return say("could not load " + path);
            if (!d->place(slot)) return say("wrong kind: " + path);
            d->wireInARow();
            say("added " + info->name);
        } else if (what == "instrument") {
            std::istringstream r(rest);
            std::string which, name;
            r >> which;
            std::getline(r, name);
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            TrackPtr t = track(which);
            if (!t) return say("no such track");
            const plug::PluginInfo* info = nullptr;
            if (name != "builtin") {
                for (auto& p : main->plugins->library().plugins)
                    if (!info && (p.name == name || p.path.find(name) != std::string::npos)) info = &p;
                if (!info) return say("no such plugin: " + name);
            }
            instrument_menu::setInstrument(main, *main->plugins, t, info, [this, t] { main->instrumentChosen(t); });
        } else if (what == "editInstrument") {
            TrackPtr t = track(rest);
            if (!t) return say("no such track");
            main->editInstrument(t);
        } else if (what == "crashPlugins") {  // (every Windows plugin's Wine host killed, as a crash)
            int killed = 0;
            for (const SlotPtr& s : main->project.pluginSlots()) {
                auto instance = std::atomic_load(&s->instance);
                int pid = instance ? plug::bridgedHostPid(*instance) : -1;
                if (pid > 0 && kill(pid, SIGKILL) == 0) ++killed;
            }
            say("crashed " + std::to_string(killed) + " Windows plugin" + (killed == 1 ? "" : "s"));
        } else if (what == "undo") main->undo();
        else if (what == "redo") main->redo();
        else if (what == "autosave") {  // write the recovery file now (and wait for its audio)
            main->autosaver->saveNow();
            std::string error = main->autosaver->wait();
            say("autosaved in " + main->autosaver->folder() + (error.empty() ? "" : ": " + error));
        } else if (what == "recover") {  // the start's question, answered: open, discard or later
            main->autosaver->answer = rest == "open" ? 0 : rest == "discard" ? 1 : 2;
            say(main->autosaver->offerRecovery() ? "recovered" : "not recovered");
            main->autosaver->answer = -1;
        } else if (what == "template") {  // File → New from template ▸ <name>
            for (auto& t : templates::list())
                if (t.name == rest) {
                    main->newFromTemplate(t.path);
                    return;
                }
            say("no such template: " + rest);
        } else if (what == "saveTemplate") {
            std::string error;
            std::string path = templates::writeTemplate(*main, rest, error);
            say(path.empty() ? "could not save the template: " + error : "saved the template " + path);
        } else if (what == "action") {  // a menu entry by its path: action Edit/Undo, action File/Open recent/Clear the list
            std::vector<std::string> names = util::split(rest, '/');
            ui::Menu* menu = nullptr;
            for (auto& m : main->menuBar->menus())
                if (stripAmp(m->title) == names[0]) menu = m.get();
            for (size_t k = 1; menu && k < names.size(); ++k) {
                if (menu->aboutToShow) menu->aboutToShow(menu);
                ui::Action* found = nullptr;
                for (auto& a : menu->actions())
                    if (!a->separator && stripAmp(a->text.substr(0, a->text.find('\t'))) == names[k]) found = a.get();
                if (!found) return say("no such menu entry: " + names[k]);
                if (k + 1 == names.size()) {
                    if (!found->enabled) return say("disabled: " + names[k]);
                    ui::Menu::activate(found);
                    return;
                }
                menu = found->submenu;
            }
            say("no such menu: " + rest);
        } else if (what == "menu") {
            int i = atoi(rest.c_str());
            auto& menus = main->menuBar->menus();
            if (i < 0 || i >= (int)menus.size()) return say("no such menu");
            // (below the bar, as a click on its title would open it)
            menus[i]->popup(main->menuBar->mapToGlobal(Point(0, main->menuBar->height())), main->menuBar);
        } else {
            say("unknown call: " + what);
        }
    }
    void master_volume(int db) { main->master->volume->setValue(db); }
    static std::string stripAmp(const std::string& t) {
        std::string out;
        for (char c : t)
            if (c != '&') out += c;
        return out;
    }

    // one command; false: wait before the next
    void run(const std::string& line) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd.empty() || cmd[0] == '#') return;
        say(line);
        if (cmd == "wait") {
            int ms = 0;
            in >> ms;
            resumeAt = plat::now() + ms / 1000.0;
        } else if (cmd == "frames") {
            in >> framesLeft;
        } else if (cmd == "target") {
            std::getline(in, target);
            while (!target.empty() && target.front() == ' ') target.erase(target.begin());
        } else if (cmd == "glide") {  // glide x0 y0 x1 y1 ms: a drag at a hand's pace (a move every millisecond)
            ui::Window* w = targetWindow();
            if (!w) return say("no target window");
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0, ms = 500;
            in >> x0 >> y0 >> x1 >> y1 >> ms;
            mouse(w, plat::Event::MouseMove, x0, y0, 0, 0);
            mouse(w, plat::Event::MouseDown, x0, y0, plat::LeftButton, plat::LeftButton);
            double start = plat::now(), length = std::max(1, ms) / 1000.0;
            resumeAt = start + length + 0.1;
            auto timer = std::make_shared<int>(-1);
            *timer = App::get().every(1, [this, w, x0, y0, x1, y1, start, length, timer] {
                if (!ui::Widget::alive(w)) return App::get().cancelTimer(*timer);
                double f = std::min(1.0, (plat::now() - start) / length);
                int x = (int)std::lround(x0 + (x1 - x0) * f), y = (int)std::lround(y0 + (y1 - y0) * f);
                mouse(w, plat::Event::MouseMove, x, y, 0, plat::LeftButton);
                if (f >= 1) {
                    mouse(w, plat::Event::MouseUp, x1, y1, plat::LeftButton, 0);
                    App::get().cancelTimer(*timer);
                }
            });
        } else if (cmd == "move" || cmd == "click" || cmd == "drag" || cmd == "wheel") {
            ui::Window* w = targetWindow();
            if (!w) return say("no target window");
            int x = 0, y = 0;
            in >> x >> y;
            if (cmd == "move") {
                mouse(w, plat::Event::MouseMove, x, y, 0, 0);
            } else if (cmd == "click") {
                std::string how;
                in >> how;
                int b = how == "right" ? plat::RightButton : plat::LeftButton;
                mouse(w, plat::Event::MouseMove, x, y, 0, 0);
                if (!ui::Widget::alive(w)) return;
                mouse(w, plat::Event::MouseDown, x, y, b, b);
                if (!ui::Widget::alive(w)) return;
                mouse(w, plat::Event::MouseUp, x, y, b, 0);
                if (how == "double" && ui::Widget::alive(w)) {
                    mouse(w, plat::Event::MouseDown, x, y, b, b);
                    if (ui::Widget::alive(w)) mouse(w, plat::Event::MouseUp, x, y, b, 0);
                }
            } else if (cmd == "drag") {
                int x1 = 0, y1 = 0;
                in >> x1 >> y1;
                mouse(w, plat::Event::MouseMove, x, y, 0, 0);
                mouse(w, plat::Event::MouseDown, x, y, plat::LeftButton, plat::LeftButton);
                for (int k = 1; k <= 8 && ui::Widget::alive(w); ++k)
                    mouse(w, plat::Event::MouseMove, x + (x1 - x) * k / 8, y + (y1 - y) * k / 8, 0, plat::LeftButton);
                if (ui::Widget::alive(w)) mouse(w, plat::Event::MouseUp, x1, y1, plat::LeftButton, 0);
            } else {
                float n = 1;
                std::string mod;
                in >> n >> mod;
                plat::Event e;
                e.type = plat::Event::Wheel;
                e.x = dev(w, x);
                e.y = dev(w, y);
                e.dy = n;
                e.mods = mod == "ctrl" ? plat::ModCtrl : mod == "shift" ? plat::ModShift : mod == "alt" ? plat::ModAlt : 0;
                send(w, e);
            }
        } else if (cmd == "key") {
            std::string keys;
            in >> keys;
            ui::KeySequence ks = ui::KeySequence::parse(keys);
            std::string text;
            if (!(ks.mods & (plat::ModCtrl | plat::ModAlt)) && ks.key > 0 && ks.key < 0x80) {
                char c = (char)ks.key;
                if (c >= 'A' && c <= 'Z' && !(ks.mods & plat::ModShift)) c = (char)(c - 'A' + 'a');
                text = std::string(1, c);
            }
            key(keyWindow(), ks, text);
        } else if (cmd == "type") {
            std::string text;
            std::getline(in, text);
            if (!text.empty() && text.front() == ' ') text.erase(text.begin());
            for (char c : text) {
                int k = (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : (unsigned char)c;
                key(keyWindow(), ui::KeySequence(k, 0), std::string(1, c));
            }
        } else if (cmd == "shot") {
            std::string path;
            in >> path;
            ui::Window* w = targetWindow();
            if (!w) return say("no target window");
            shot(w, path);
        } else if (cmd == "call") {
            call(in);
        } else if (cmd == "print") {
            print();
        } else if (cmd == "quit") {
            done = true;
            main->askBeforeClosing = false;
            App::get().closeAllPopups();
            // (modal dialogs first: their nested loops must end before the program can)
            for (int k = 0; k < 10; ++k) {
                ui::Window* m = App::get().modalWindow();
                if (!m) break;
                if (auto* d = dynamic_cast<ui::Dialog*>(m)) d->reject();
                else m->hideWindow();
            }
            main->close();
        } else if (cmd == "closewin") {  // closes the target window (as its title bar's ✕)
            if (ui::Window* w = targetWindow()) w->close();
        } else if (cmd == "close") {
            main->close();
        } else {
            say("unknown command: " + cmd);
        }
    }

    void tick() {
        if (done) return;
        if (framesLeft > 0 || plat::now() < resumeAt) return;
        // run commands until one asks to wait; each runs posted (it may open a modal dialog, whose
        // nested loop keeps this timer going)
        if (next >= lines.size()) {
            done = true;
            say("end of script");
            return;
        }
        std::string line = lines[next++];
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "wait" || cmd == "frames" || cmd == "target" || cmd.empty() || cmd[0] == '#') {
            run(line);
            return;
        }
        resumeAt = plat::now() + 0.05;  // (a moment for what it posts, opens and paints)
        plat::postToMain([this, line] { run(line); });
    }
};

Runner* runner = nullptr;

}  // namespace

void startFromEnvironment(MainWindow* window) {
    std::vector<std::string> lines;
    if (const char* shotPath = getenv("TINYDAW_SCREENSHOT")) {
        const char* n = getenv("TINYDAW_SCREENSHOT_FRAMES");
        lines = {"frames " + std::string(n ? n : "60"), std::string("shot ") + shotPath, "quit"};
    } else if (const char* script = getenv("TINYDAW_SCRIPT")) {
        std::ifstream f(script);
        if (!f) {
            fprintf(stderr, "Tiny DAW: can't read the script %s\n", script);
            return;
        }
        std::string line;
        while (std::getline(f, line)) lines.push_back(line);
    } else {
        return;
    }
    runner = new Runner{window, lines};
    runner->timer = App::get().every(10, [] { runner->tick(); });
    runner->frame = App::get().onFrame([] {
        if (runner->framesLeft > 0) --runner->framesLeft;
    });
}

}  // namespace test_script
