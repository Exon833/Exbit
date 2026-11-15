// A plugin's own window (plugin_editor.py): an ordinary top-level window whose native (X11) window
// the plugin's view is embedded in (plug::Plugin::openEditor). Several can be open at once and
// nothing waits for them: the program keeps running as usual.
//
// The plugin decides the size (it may change it, e.g. when its own window grows a panel); plugins
// that allow it can be resized by dragging the window's edge. The window stays above the main
// window and is named after the plugin and its track. The plugin gets idle() every frame (values
// from playback, size changes both ways: the window's new size is passed to it there, once a frame).
//
// A Windows plugin that stops while its window is open (its Wine host crashed: its view went with
// it) leaves the window saying so, with a Reload button (`reloadable`: PluginHost sets it for
// Windows plugins; the button emits reloadRequested).
//
// invisible=true: the window is mapped offscreen (WindowOptions::offscreen), off the taskbar and
// without taking the focus, for a picture of it (PluginHost::photograph).
#pragma once
#include <memory>
#include <optional>
#include <string>

#include "gfx/image.h"
#include "model/model.h"
#include "ui/toolkit/widget.h"

namespace automation { class TweakWatcher; }

class EditorWindow : public ui::Window {
public:
    EditorWindow(ui::Window* parent, SlotPtr slot, const std::string& title, bool invisible = false);
    ~EditorWindow() override;

    SlotPtr slot;
    bool invisible;
    bool resizable = false;
    bool fitting = false;  // (resizing to the plugin's own size: not passed back to it)

    // let the plugin draw in this window; false if it has no window (or none for this system)
    bool attach();
    // take the plugin's size (pixels)
    void fit(int width, int height);
    // every frame: the plugin's time, and the size it asks for
    void idle();
    // over the main window (a third of the way down), or where it was last, fully on the screen
    void place(ui::Window* main, std::optional<Point> last = {});
    void close() override;
    void resized() override;
    // the computer keyboard's keys play notes here too (also while the plugin has the keys: idle())
    void onEvent(plat::Window* w, const plat::Event& e) override;
    // the window's top-left on the screen (where it is remembered)
    Point position() const;

    ui::Signal<SlotPtr> editorClosed;  // the slot, once the window is closed (Python: closed)
    ui::Signal<SlotPtr> reloadRequested;  // its Reload button (the plugin stopped)
    bool reloadable = false;           // a stopped plugin offers Reload (a Windows plugin)
    // the plugin stopped: say so in the window (idle() does once it sees the slot failed)
    void showStopped();
    bool stoppedShown() const { return stoppedShown_; }

    // kept by PluginHost while it is open
    std::unique_ptr<automation::TweakWatcher> watcher;
    Image picture;           // the latest photograph (empty: none yet)
    double photographed = 0; // when (App::now())
    int tickTimer = -1;      // PluginHost's timer asking the watcher (every TWEAK_MS)

private:
    int frameId_ = -1;
    bool closing_ = false;
    bool stoppedShown_ = false;
    bool sizeChanged_ = false;  // (resized by the user: the plugin is told in idle())
    static plat::WindowOptions options(ui::Window* parent, const std::string& title, bool invisible);
};
