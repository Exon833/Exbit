// A manual check of plugin windows (not built by default): opens a plugin's own window (its editor)
// in a window of the program's platform layer, as the program does, and keeps it running until
// the window is closed. Resizing the window resizes the editor (if it allows); the editor asking
// for another size resizes the window. Parameters changed in the editor are printed.
//
//   make plugins   # the test plugins, if those are to be checked
//   g++ -std=c++17 -O1 -g -Isrc -pthread tools/plugin_editor_check.cpp \
//       build/src/plugins/*.o build/src/core/*.o build/src/platform/*.o build/src/gfx/*.o \
//       -o build/plugin_editor_check -lX11 -lXext -lXrandr -lXcursor -lXi -ldl -lm
//   build/plugin_editor_check build/test-plugins/TinyTestFx.vst3 ["Tiny Test Fx"] [scale]
//   build/plugin_editor_check build/test-plugins/TinyTestSynth.so
//
// The test plugins' windows: Tiny Test Fx — click switches Duck, right-click asks for a bigger
// window; Tiny Test Synth — click sets the volume, right-click asks for a bigger window.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "platform/platform.h"
#include "plugins/plugin.h"

namespace {

struct Check : plat::WindowListener {
    plug::Plugin* plugin = nullptr;
    plat::Window* window = nullptr;
    bool settingSize = false;

    void onEvent(plat::Window* w, const plat::Event& e) override {
        if (e.type == plat::Event::Close) {
            plat::quit();
        } else if (e.type == plat::Event::Resize && plugin && plugin->editorOpen() && !settingSize) {
            int width = e.w, height = e.h;
            plugin->editorResized(width, height);
            if (width != e.w || height != e.h) {  // (the editor takes another size: the window follows)
                settingSize = true;
                w->resize(width, height);
                settingSize = false;
            }
            printf("window %d x %d, editor %d x %d\n", e.w, e.h, width, height);
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <plugin.vst3 | plugin.so> [plugin name] [scale]\n", argv[0]);
        return 2;
    }
    plat::init(argc, argv);
    std::string error;
    auto plugin = plug::Plugin::load(argv[1], argc > 2 ? argv[2] : "", error);
    if (!plugin) {
        fprintf(stderr, "cannot load %s: %s\n", argv[1], error.c_str());
        return 1;
    }
    double scale = argc > 3 ? atof(argv[3]) : 1.0;
    printf("%s (%s) by %s: %s, %d parameters, latency %d\n", plugin->name().c_str(), plugin->isVst2() ? "VST2" : "VST3",
           plugin->vendor().c_str(), plugin->isInstrument() ? "instrument" : "effect", (int)plugin->parameters().size(),
           plugin->latency());
    if (!plugin->hasEditor()) {
        fprintf(stderr, "it has no window\n");
        return 1;
    }
    Check check;
    check.plugin = plugin.get();
    plat::WindowOptions opts;
    opts.title = plugin->name() + " - plugin_editor_check";
    opts.w = 400;
    opts.h = 300;
    auto window = plat::Window::create(opts, &check);
    check.window = window.get();
    window->show();
    plat::processEvents(50);
    int w = 0, h = 0;
    if (!plugin->openEditor(window->nativeHandle(), window->nativeDisplay(), scale, w, h)) {
        fprintf(stderr, "its window didn't open\n");
        return 1;
    }
    printf("editor open: %d x %d, resizable: %s\n", w, h, plugin->editorResizable() ? "yes" : "no");
    window->setResizable(plugin->editorResizable());
    check.settingSize = true;
    window->resize(w, h);
    check.settingSize = false;
    plugin->prepare(48000, 512);
    plat::addTimer(16, [&] {
        plugin->idle();
        int rw, rh;
        if (plugin->requestedSize(rw, rh)) {
            printf("the editor asks for %d x %d\n", rw, rh);
            check.settingSize = true;
            window->resize(rw, rh);
            check.settingSize = false;
        }
        for (uint32_t id : plugin->edited())
            printf("edited parameter %u: %s\n", id, plugin->paramText(id, plugin->getParam(id)).c_str());
    });
    plat::run();
    plugin->closeEditor();
    plugin.reset();
    window.reset();
    plat::shutdown();
    return 0;
}
