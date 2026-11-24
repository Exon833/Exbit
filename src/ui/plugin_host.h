// Loading plugins and opening their windows, for the GUI (plugin_host.py).
//
// Both happen on the main thread. Most plugins load in well under a second; meanwhile a small
// "please wait" window shows, saying how long the plugin took last time. Other plugins keep playing
// meanwhile. A plugin's own window (plugin_editor.h) doesn't block anything: several can be open,
// and the program keeps running as usual. It opens titled with the plugin's and track's names, over
// the main window or where it was last, fully on screen. While it is open, the parameters changed
// in it are noted (automation::TweakWatcher) and it is photographed now and then, for the patch
// panel in the effects window (plugin_pictures.h); photograph() takes a picture without showing
// the window. A built-in effect (effects::isBuiltin(slot->path)) opens its own window
// (effect_window.h: EffectWindow) instead, and its picture is simply what that window shows.
//
// The KindFinder (plugins/probe.h) finds out in the background whether the plugins not known yet
// are instruments or effects (in a process of its own, one plugin after the other); the menus and
// lists show what it found as soon as they open again. The status bar says how it goes.
//
// A Windows plugin whose Wine host crashed is shown stopped (the status bar says so once); reload()
// starts it afresh in a new host, at the engine's rate, with the settings it last gave
// (plugins/bridged.cpp keeps them), swapped in for the stopped one as instances always are
// (std::atomic_store, the old one retired); its window opens again if it was open, and automation
// finds the new instance (automation::resolve). The effects windows' Reload button, their units'
// menus, the instrument menu, a stopped plugin's own window and the track's menu offer it. With
// `autoReload` (Options → Reload crashed Windows plugins automatically) idle() reloads them by
// itself, at most AUTO_RELOADS times a slot within AUTO_RELOAD_SECONDS; after that it stays stopped.
// (Plugins of Linux run in the program's own process: they can't be started afresh.)
//
// THE MAIN WINDOW: makes one PluginHost(this, project, engine, [prefs] { return prefs.vst2List(); }),
// sets `statusBar` and `autoReload` (Options), calls idle(project.pluginSlots()) every frame, findKinds() 1.5 s after start,
// closeEditor() / stopFindingKinds() when closing, restore(slots) after opening a project,
// and asks changed() for its unsaved-changes check, saved() once saved.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/model.h"
#include "plugins/library.h"
#include "plugins/probe.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/widget.h"

namespace audio { class Engine; }
class EditorWindow;
class EffectWindow;

namespace plugin_host {
constexpr int TWEAK_MS = 150;          // how often to ask an open window which parameters were changed in it
constexpr double PHOTO_EVERY_S = 1.5;  // how often an open window is photographed (the last picture is kept when it closes)
constexpr double PHOTO_WAIT_S = 0.8;   // photograph(): how long the invisible window gets to draw itself
constexpr double PHOTO_GIVE_UP_S = 4;  // …and how long to keep trying while it shows nothing yet
constexpr int PHOTO_TRY_MS = 100;
constexpr int AUTO_RELOADS = 3;              // automatic reloads of a slot at most…
constexpr double AUTO_RELOAD_SECONDS = 60;   // …within this long
}  // namespace plugin_host

class PluginHost {
public:
    // window: the main window (parent of the messages and plugin windows); vst2Folders: the folders
    // chosen in Options → VST2 plugin folders (asked whenever the library is made)
    PluginHost(ui::Window* window, Project& project, audio::Engine& engine,
               std::function<std::vector<std::string>()> vst2Folders = {});
    ~PluginHost();

    ui::Window* window;
    Project& project;
    audio::Engine& engine;
    std::function<std::vector<std::string>()> vst2Folders;
    ui::StatusBar* statusBar = nullptr;           // where the kind finder says how it goes (may stay null)
    std::function<void(const std::string& text, int ms)> showStatus;  // (instead of statusBar, if set)
    // slot -> its open window: an EditorWindow (a plugin's own), or an EffectWindow (a built-in effect)
    std::map<PluginSlot*, ui::Window*> editors;
    bool edited = false;                          // a plugin's settings changed since saving (in its window, a preset, MIDI learn…)
    ui::Signal<SlotPtr> pictureTaken;             // a new picture of a plugin was kept (the effects windows show it)
    std::unique_ptr<plug::KindFinder> finder;     // the KindFinder at work, if any
    plug::KindFinder::Spawner spawner;            // (tests: how the probe process is started; empty = this program)
    std::string cachePath;                        // (tests: the library's cache file; "" = the default)
    std::vector<std::string> folders;             // (tests: the VST3 folders; empty = plug::defaultFolders())
    bool autoReload = false;                      // Options → Reload crashed Windows plugins automatically
    ui::Signal<SlotPtr> slotChanged;              // a plugin stopped, or was reloaded (the effects windows show it)
    std::function<bool(const plug::Plugin&)> reloadable;  // (tests: which can be reloaded; empty = plug::isBridged)

    // the plugins on this computer (listing the folders is quick, but only done when needed)
    plug::PluginLibrary& library();
    // list the plugins afresh (the VST2 folders changed), and find out the kinds of new ones
    plug::PluginLibrary& rescan();
    // find out in the background whether the plugins not known yet are instruments or effects
    // (useMainLoop false: tests drive finder->poll() themselves)
    void findKinds(bool useMainLoop = true);
    void stopFindingKinds();
    // how long loading this plugin should take, from last time
    std::string expected(const plug::PluginInfo& info);
    // a new PluginSlot with the plugin loaded, or null (after saying why)
    SlotPtr load(const plug::PluginInfo& info);
    // load the plugins of an opened project with their saved settings; returns the names of those
    // that could not be loaded (they keep their settings, so saving again loses nothing)
    std::vector<std::string> restore(const std::vector<SlotPtr>& slots);
    // give loaded plugins time on the main thread (call every frame); those with a window open get
    // it from there. Notices those that stopped (and reloads them, with autoReload).
    void idle(const std::vector<SlotPtr>& slots);

    // --- stopped Windows plugins (see the top) ---
    // has the slot's plugin stopped, and can it be started afresh (a Windows plugin)?
    bool canReload(const SlotPtr& slot) const;
    // start it afresh with the settings it last gave; false (and why) if it couldn't: it stays stopped
    bool reload(const SlotPtr& slot, std::string& why);
    // (buttons and menus) reload(), or a message saying why it couldn't
    void reloadStopped(const SlotPtr& slot);
    // the track (its name) or "Master" holding this slot, for its window's title ("" if none)
    std::string whereOf(const SlotPtr& slot) const;

    // --- windows ---
    // open the plugin's own window (`where`: the track's name, for its title), or bring it forward
    void edit(const SlotPtr& slot, const std::string& where = "");
    // close the open plugin windows (of `slots`, when given)
    void closeEditor();
    void closeEditor(const std::vector<SlotPtr>& slots);
    // take a picture of the plugin's window without showing it: opened offscreen, photographed once
    // it has drawn itself (about a second), closed again. True if a picture was kept.
    bool photograph(const SlotPtr& slot);
    void keepPicture(const SlotPtr& slot, const Image& image);
    // where a plugin's window was last (forgotten with the slot)
    std::optional<Point> windowPosition(const SlotPtr& slot) const;
    // a picture of a plugin's window as it is now; empty if none can be taken
    Image pictureOf(ui::Window* w);
    // the open window of a slot as either kind (null if it has none, or it is the other kind)
    EditorWindow* editorOf(const SlotPtr& slot) const;
    EffectWindow* effectWindowOf(const SlotPtr& slot) const;
    // have plugins' settings changed since saved()? `edited`, or an open window's plugin whose settings
    // differ from those it had when its window opened (or when saved, if later)
    bool changed();
    void saved();  // (the project was saved: its plugins' settings, as they are now, are the saved ones)

private:
    std::unique_ptr<plug::PluginLibrary> library_;
    std::vector<std::pair<std::weak_ptr<PluginSlot>, Point>> positions_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool loading_ = false;  // (a plugin is being loaded or reloaded: its "please wait" window lets frames run)
    std::vector<std::weak_ptr<PluginSlot>> stopped_;  // stopped slots already noticed (said once)
    std::map<const PluginSlot*, std::string> openedWith_;  // slots with a window open: their settings when it opened
    bool settingsMoved(const PluginSlot* slot, const std::string& opened) const;
    std::vector<std::pair<std::weak_ptr<PluginSlot>, std::vector<double>>> autoReloads_;  // when each was reloaded by itself
    void noticeStopped(const std::vector<SlotPtr>& slots);
    bool autoReloadAllowed(const SlotPtr& slot, double now);
    void notePosition(const SlotPtr& slot, Point pos);
    void watch(ui::Window* w);
    void editorClosed(const SlotPtr& slot);
    void status(const std::string& text, int ms);
    std::unique_ptr<ui::ProgressDialog> busy(const std::string& text);
};
