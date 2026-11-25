// Start Exbit (main.py):  exbit [project.json | audio and MIDI files to add...]
//
//   exbit --tui [...]          the same in the terminal, by the keyboard alone (tui/app.h): no window
//                                system needed
//   exbit --probe <listfile>   the background plugin probe's child process (plugins/probe.h): finds
//                                out the kinds of the plugins listed, without touching the display
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/util.h"
#include "platform/platform.h"
#include "plugins/probe.h"
#include "ui/autosave.h"
#include "ui/main_window.h"
#include "ui/preferences.h"
#include "ui/plugin_host.h"
#include "ui/test_script.h"
#include "ui/timeline.h"
#include "ui/toolkit/widget.h"
#include "tui/app.h"

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--probe") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: exbit --probe <listfile>\n");
            return 2;
        }
        return plug::runProbe(argv[2]);
    }
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        printf("usage: exbit [--tui] [project.json | audio and MIDI files to add...]\n"
               "  --tui  run in the terminal, by the keyboard alone (no window system needed)\n");
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--tui") == 0) return tui::run(std::vector<std::string>(argv + 2, argv + argc));
    if (util::debug()) setenv("EXBIT_WINE_TRACE", "1", 0);  // (the Wine host's diagnostics too)

    ui::App& app = ui::App::get();
    app.init(argc, argv);
    if (!plat::hasDisplay()) return 1;  // (x11.cpp said why)
    // Options → Interface size: everything drawn this much bigger or smaller (plugins' windows aside)
    int size = prefs::interfaceScale();
    app.setScale(size / 100.f);
    // Options → Window frames: Exbit's own (drawn as the skin says) or the window manager's
    app.setOwnFrames(prefs::ownWindowFrames());
    // Options → Window frames → Transparency: see-through windows while a compositing manager runs
    app.setTransparency(prefs::transparentWindows());
    int rc;
    {
        // the skin is applied by the main window, before it builds anything
        auto* window = new MainWindow();
        // its first size (1280 x 800 at the interface size: 1920 x 1200 at 150 %) bigger than the screen: made to
        // fit it, room left for a frame, so that nothing of it is off the screen or under the taskbar
        Rect screen = app.screenAt(app.cursorPos());
        int fitW = std::min(window->width(), screen.w * 19 / 20), fitH = std::min(window->height(), screen.h * 19 / 20);
        if (!screen.empty() && (fitW != window->width() || fitH != window->height())) {
            util::debugf("main window: %dx%d made %dx%d to fit the screen %dx%d\n", window->width(), window->height(),
                         fitW, fitH, screen.w, screen.h);
            window->resizeWindow(fitW, fitH);
        }
        window->showWindow();
        if (!window->interfaceSizeFits(size))  // (another, smaller screen than it was chosen on)
            window->showStatus("At " + std::to_string(size) +
                                   " % the window is wider than this screen: choose a smaller size in Options → "
                                   "Interface size",
                               20000);
        for (int i = 1; i < argc; ++i) {
            std::string path = argv[i];
            if (util::extension(path) == ".json") window->openProjectFile(path);
            else window->timeline->addFile(path);
        }
        // work kept by a run that didn't close normally
        app.singleShot(0, [window] {
            if (!window->closing) window->autosaver->offerRecovery();
        });
        // plugins whose kind isn't known yet: in the background
        app.singleShot(1500, [window] {
            if (!window->closing) window->plugins->findKinds();
        });
        test_script::startFromEnvironment(window);
        rc = app.exec();
        delete window;
    }
    app.collectGarbage();
    plat::shutdown();
    return rc;
}
