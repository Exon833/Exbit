// Loading plugins and opening their windows, for the GUI (plugin_host.py).
#include "ui/plugin_host.h"
#include "ui/param_window.h"

#include <algorithm>

#include "audio/automation.h"
#include "audio/modulation.h"
#include "audio/engine.h"
#include "audio/plugin_audio.h"
#include "core/util.h"
#include "effects/effects.h"
#include "plugins/wine.h"
#include "ui/effect_window.h"
#include "ui/plugin_editor.h"
#include "ui/plugin_pictures.h"
#include "ui/plugin_windows.h"
#include "ui/toolkit/dialogs.h"

using namespace plugin_host;

namespace {
// (a plugin being loaded: its "please wait" window lets frames run, which mustn't reload others meanwhile)
struct Loading {
    bool& flag;
    bool was;
    explicit Loading(bool& f) : flag(f), was(f) { flag = true; }
    ~Loading() { flag = was; }
};
}  // namespace

PluginHost::PluginHost(ui::Window* window_, Project& project_, audio::Engine& engine_,
                       std::function<std::vector<std::string>()> vst2Folders_)
    : window(window_), project(project_), engine(engine_), vst2Folders(std::move(vst2Folders_)) {}

PluginHost::~PluginHost() {
    *alive_ = false;
    closeEditor();
    stopFindingKinds();
}

// the VST3 folders: Linux's, and those of the Wine prefixes chosen (Options → Wine prefixes)
static std::vector<std::string> standardFolders() {
    auto f = plug::defaultFolders();
    for (auto& w : wine::vst3Folders(wine::prefixes())) f.push_back(w);
    return f;
}

plug::PluginLibrary& PluginHost::library() {
    if (!library_)  // listing the plugin folders is quick, but only done when needed
        library_ = std::make_unique<plug::PluginLibrary>(folders.empty() ? standardFolders() : folders, cachePath,
                                                         vst2Folders ? vst2Folders() : std::vector<std::string>{});
    return *library_;
}

plug::PluginLibrary& PluginHost::rescan() {
    bool running = finder != nullptr;
    stopFindingKinds();
    library_.reset();
    if (running || !library().unknownFiles().empty()) findKinds(!ui::App::get().headless());
    return library();
}

void PluginHost::status(const std::string& text, int ms) {
    if (showStatus) showStatus(text, ms);
    else if (statusBar) statusBar->showMessage(text, ms);
}

void PluginHost::findKinds(bool useMainLoop) {
    if (finder) return;
    finder = std::make_unique<plug::KindFinder>(library(), spawner);
    plug::KindFinder* f = finder.get();
    int total = f->total();
    if (total)
        status(util::format("Finding out the types of %d plugin%s…", total, total == 1 ? "" : "s"), 4000);
    f->progress = [this](int done, int all) {
        status(util::format("Finding out the types of plugins: %d of %d…", done, all), 4000);
    };
    auto alive = alive_;
    f->finished = [this, f, total, alive](int found) {
        if (!*alive) return;
        if (total) status(util::format("Found the types of %d of %d plugins", found, total), 5000);
        if (finder.get() == f) {
            // (the finder may only be destroyed after it returns from here)
            std::shared_ptr<plug::KindFinder> dead(finder.release());
            plat::postToMain([dead] {});
        }
    };
    f->start(useMainLoop);
}

void PluginHost::stopFindingKinds() {
    if (finder) {
        finder->cancel();
        finder.reset();
    }
}

std::unique_ptr<ui::ProgressDialog> PluginHost::busy(const std::string& text) {
    auto dialog = std::make_unique<ui::ProgressDialog>(window, "Loading plugin", text, false);
    dialog->setRange(0, 0);
    dialog->setModal(true);
    dialog->open();
    for (int i = 0; i < 5; ++i) ui::App::get().processEvents();  // (painted before the main thread blocks)
    return dialog;
}

std::string PluginHost::expected(const plug::PluginInfo& info) {
    double seconds = library().lastLoadSeconds(info);
    if (seconds < 0) return "Loading it for the first time can take a little longer.";
    return seconds >= 0.95 ? util::format("Last time it took %.1f s.", seconds) : "Last time it took under a second.";
}

SlotPtr PluginHost::load(const plug::PluginInfo& info) {
    Loading loading(loading_);
    auto dialog = busy("Loading " + info.name + "…\n" + expected(info));
    std::unique_ptr<plug::Plugin> instance;
    try {
        instance = library().load(info, engine.sr);
    } catch (const plug::PluginChoiceNeeded& choice) {
        dialog.reset();
        int index = 0;
        if (!ui::getItem(window, "Choose a plugin", info.name + " holds several plugins:", choice.names, index) ||
            index < 0 || index >= (int)choice.names.size())
            return nullptr;
        const std::string& name = choice.names[index];
        return load(plug::PluginInfo{info.path, name, "", name});
    } catch (const std::exception& e) {
        dialog.reset();
        ui::msg::warning(window, "Cannot load plugin", info.name + "\n" + info.path + "\n\n" + e.what());
        return nullptr;
    }
    dialog.reset();
    if (!instance) {
        ui::msg::warning(window, "Cannot load plugin", info.name + "\n" + info.path);
        return nullptr;
    }
    auto slot = std::make_shared<PluginSlot>();
    slot->path = info.path;
    slot->name = info.name;
    slot->kind = instance->isInstrument() ? plug::INSTRUMENT : plug::EFFECT;
    slot->pluginName = info.pluginName;
    slot->instance = std::shared_ptr<plug::Plugin>(std::move(instance));  // (nobody else sees the slot yet)
    return slot;
}

std::vector<std::string> PluginHost::restore(const std::vector<SlotPtr>& slots) {
    std::vector<std::string> failed;
    Loading loading(loading_);
    auto dialog = busy("Loading plugins of the project…");
    int i = 0;
    for (const SlotPtr& slot : slots) {
        ++i;
        plug::PluginInfo info{slot->path, slot->name, slot->kind, slot->pluginName};
        dialog->setLabel(util::format("Loading plugins of the project (%d of %d): %s…\n", i, (int)slots.size(),
                                      slot->name.c_str()) + expected(info));
        ui::App::get().processEvents();
        try {
            std::unique_ptr<plug::Plugin> loaded = library().load(info, engine.sr);
            if (!loaded) throw std::runtime_error("not loaded");
            if (!slot->state.empty()) plug::setState(*loaded, slot->state);
            std::shared_ptr<plug::Plugin> instance(std::move(loaded));
            std::atomic_store(&slot->instance, instance);  // only now can the audio thread use it
        } catch (const std::exception&) {
            std::atomic_store(&slot->instance, std::shared_ptr<plug::Plugin>());
            failed.push_back(slot->name);
        }
    }
    dialog.reset();
    return failed;
}

void PluginHost::idle(const std::vector<SlotPtr>& slots) {
    for (const SlotPtr& slot : slots) {
        auto instance = aload(slot->instance);
        if (instance && !editors.count(slot.get())) instance->idle();
    }
    if (!loading_) noticeStopped(slots);
}

// --- stopped Windows plugins ---

bool PluginHost::canReload(const SlotPtr& slot) const {
    auto instance = slot ? aload(slot->instance) : nullptr;
    if (!instance || !slot->failed.load(std::memory_order_acquire)) return false;
    return reloadable ? reloadable(*instance) : plug::isBridged(*instance);
}

std::string PluginHost::whereOf(const SlotPtr& slot) const {
    for (const TrackPtr& t : project.allTracks())
        if (aload(t->instrument) == slot || t->rack.contains(slot) || t->effects.contains(slot)) return t->name;
    return project.masterEffects.contains(slot) ? "Master" : "";
}

void PluginHost::noticeStopped(const std::vector<SlotPtr>& slots) {
    // (said once a stop; forgotten once it plays again, or its slot is gone)
    stopped_.erase(std::remove_if(stopped_.begin(), stopped_.end(),
                                  [](const std::weak_ptr<PluginSlot>& w) {
                                      SlotPtr s = w.lock();
                                      return !s || !s->failed.load();
                                  }),
                   stopped_.end());
    for (const SlotPtr& slot : slots) {
        auto instance = aload(slot->instance);
        if (!instance || !slot->failed.load(std::memory_order_acquire)) continue;
        bool known = false;
        for (auto& w : stopped_) known = known || w.lock() == slot;
        if (known) continue;
        bool canBe = canReload(slot);
        bool crashed = canBe && (reloadable || plug::bridgedHostEnded(*instance));  // (tests: any stop)
        // (not by itself while a modal window is up, an export or a question: once it has gone)
        if (autoReload && crashed && ui::App::get().modalWindow()) continue;
        stopped_.push_back(slot);
        slotChanged.emit(slot);
        if (!canBe) {
            status(slot->name + " stopped after an error: " + plugin_audio::whyStopped(*slot), 8000);
        } else if (!autoReload || !crashed) {
            status(slot->name + (plug::bridgedHostEnded(*instance) ? " stopped: its " + std::string(wine::HOST_NAME) + " crashed"
                                                                   : " stopped (" + plugin_audio::whyStopped(*slot) + ")") +
                       ". Reload it from its effects window or its menu.",
                   8000);
        } else if (!autoReloadAllowed(slot, ui::App::get().now())) {
            status(util::format("%s crashed again: it stays stopped (it was reloaded %d times within a minute). "
                                "Reload it from its effects window or its menu.",
                                slot->name.c_str(), AUTO_RELOADS),
                   10000);
        } else {
            std::string why;
            if (!reload(slot, why)) status("Cannot reload " + slot->name + ": " + why, 10000);
        }
    }
}

bool PluginHost::autoReloadAllowed(const SlotPtr& slot, double now) {
    autoReloads_.erase(std::remove_if(autoReloads_.begin(), autoReloads_.end(),
                                      [](const auto& e) { return e.first.expired(); }),
                       autoReloads_.end());
    std::vector<double>* times = nullptr;
    for (auto& [weak, list] : autoReloads_)
        if (weak.lock() == slot) times = &list;
    if (!times) {
        autoReloads_.push_back({slot, {}});
        times = &autoReloads_.back().second;
    }
    times->erase(std::remove_if(times->begin(), times->end(), [now](double t) { return now - t >= AUTO_RELOAD_SECONDS; }),
                 times->end());
    if ((int)times->size() >= AUTO_RELOADS) return false;
    times->push_back(now);
    return true;
}

bool PluginHost::reload(const SlotPtr& slot, std::string& why) {
    if (!canReload(slot)) {
        why = slot ? slot->name + " hasn't stopped, or isn't a Windows plugin (only those can be started afresh)"
                   : "no plugin";
        return false;
    }
    if (loading_) {
        why = "another plugin is being loaded";
        return false;
    }
    Loading loading(loading_);
    auto old = aload(slot->instance);
    bool crashed = plug::bridgedHostEnded(*old);
    std::string stoppedBy = slot->error;
    // (one stopped while its host lives on, late or hung: that host ends first, so nothing waits
    // for it. A Windows plugin that has stopped gives the settings it last gave, or was given:
    // bridged.cpp)
    if (!crashed) plug::endBridgedHost(*old);
    std::string state = slot->currentState();
    plug::PluginInfo info{slot->path, slot->name, slot->kind, slot->pluginName};
    std::unique_ptr<plug::Plugin> loaded;
    {
        auto dialog = busy("Reloading " + slot->name + "…\n" + expected(info));
        try {
            loaded = library().load(info, engine.sr);
            if (!loaded) throw std::runtime_error("it didn't load");
            if (!state.empty()) plug::setState(*loaded, state);
        } catch (const std::exception& e) {
            why = e.what();
            loaded.reset();
        }
    }
    if (!loaded) return false;  // (its window, if open, still says it stopped)
    // (its stopped window goes, to open again on the new instance below)
    bool wasOpen = editors.count(slot.get()) > 0;
    if (wasOpen) closeEditor({slot});
    std::shared_ptr<plug::Plugin> fresh(std::move(loaded));
    // (the audio thread skips the slot while `failed`: it takes the new instance once that is
    // cleared, and never the old one again; the error goes first, as fail() sets it first)
    std::atomic_store(&slot->instance, fresh);
    slot->error.clear();
    slot->failed.store(false, std::memory_order_release);
    project.retire(old);           // (destroyed on this thread, once the audio thread let go of it)
    automation::resolve(project);  // (the slot's automation lanes find the new instance)
    modulation::resolve(project);  // (and the modulation cables' parameters)
    stopped_.erase(std::remove_if(stopped_.begin(), stopped_.end(),
                                  [&](const std::weak_ptr<PluginSlot>& w) { return w.lock() == slot; }),
                   stopped_.end());
    slotChanged.emit(slot);
    status("Reloaded " + slot->name + (crashed ? " (its " + std::string(wine::HOST_NAME) + " had crashed)" : " (it had stopped: " + stoppedBy + ")"),
           6000);
    if (wasOpen) edit(slot, whereOf(slot));
    return true;
}

void PluginHost::reloadStopped(const SlotPtr& slot) {
    std::string why;
    if (slot && !reload(slot, why)) ui::msg::warning(window, "Cannot reload plugin", slot->name + "\n\n" + why);
}

// --- windows ---

std::optional<Point> PluginHost::windowPosition(const SlotPtr& slot) const {
    for (auto& [weak, pos] : positions_)
        if (weak.lock() == slot) return pos;
    return std::nullopt;
}

void PluginHost::notePosition(const SlotPtr& slot, Point pos) {
    positions_.erase(std::remove_if(positions_.begin(), positions_.end(),
                                    [&](const auto& e) { return e.first.expired() || e.first.lock() == slot; }),
                     positions_.end());
    positions_.push_back({slot, pos});
}

namespace {
// what every kind of plugin window has, for the host: fn(EditorWindow*), fn(EffectWindow*) or fn(ParamWindow*)
template <class F>
void withWindow(ui::Window* w, F fn) {
    if (auto* e = dynamic_cast<EditorWindow*>(w)) fn(e);
    else if (auto* x = dynamic_cast<EffectWindow*>(w)) fn(x);
    else if (auto* l = dynamic_cast<ParamWindow*>(w)) fn(l);
}
}  // namespace

EditorWindow* PluginHost::editorOf(const SlotPtr& slot) const {
    auto it = slot ? editors.find(slot.get()) : editors.end();
    return it == editors.end() ? nullptr : dynamic_cast<EditorWindow*>(it->second);
}

EffectWindow* PluginHost::effectWindowOf(const SlotPtr& slot) const {
    auto it = slot ? editors.find(slot.get()) : editors.end();
    return it == editors.end() ? nullptr : dynamic_cast<EffectWindow*>(it->second);
}

void PluginHost::edit(const SlotPtr& slot, const std::string& where) {
    if (!slot || !plugin_audio::usable(slot)) return;
    auto it = editors.find(slot.get());
    if (it != editors.end()) {
        it->second->showWindow();
        it->second->raiseWindow();
        return;
    }
    std::string title = where.empty() ? slot->name : slot->name + " — " + where;
    ui::Window* opened = nullptr;
    auto setUp = [&](auto* w) {
        if (!w->attach()) {
            delete w;
            return false;
        }
        w->place(window, windowPosition(slot));
        editors[slot.get()] = w;
        edited = true;
        w->watcher = std::make_unique<automation::TweakWatcher>(slot, project);
        w->picture = Image();
        w->photographed = ui::App::get().now();
        w->tickTimer = ui::App::get().every(TWEAK_MS, [this, w] { watch(w); });
        w->editorClosed.connect([this](SlotPtr s) { editorClosed(s); });
        opened = w;
        return true;
    };
    auto none = [&] { ui::msg::information(window, "No plugin window", slot->name + " has no window of its own."); };
    if (effects::isBuiltin(slot->path)) {
        if (!setUp(new EffectWindow(window, slot, title))) none();
    } else {
        auto* e = new EditorWindow(window, slot, title);
        auto instance = aload(slot->instance);
        e->reloadable = instance && (reloadable ? reloadable(*instance) : plug::isBridged(*instance));
        auto alive = alive_;
        e->reloadRequested.connect([this, alive](SlotPtr s) {
            if (*alive) reloadStopped(s);
        });
        // (none of its own here, or no window system: its parameters as a list, param_window.h)
        if (!setUp(e) && !setUp(new ParamWindow(window, slot, title))) none();
    }
    if (!opened) return;
    opened->showWindow();
    opened->raiseWindow();
}

Image PluginHost::pictureOf(ui::Window* w) {
    if (auto* e = dynamic_cast<EffectWindow*>(w)) return e->snapshot();
    if (dynamic_cast<ParamWindow*>(w)) return Image();  // (a list: no picture for the patch panel)
    Image captured, picture;
    if (plugin_windows::capture(w, captured) && plugin_pictures::fromCapture(captured, picture)) return picture;
    return Image();
}

void PluginHost::watch(ui::Window* win) {
    // now and then while a window is open: the parameters changed in it, and its picture
    withWindow(win, [&](auto* w) {
        if (w->watcher) w->watcher->poll();
        double now = ui::App::get().now();
        if (now - w->photographed >= PHOTO_EVERY_S && w->isWindowShown()) {
            w->photographed = now;
            Image picture = pictureOf(w);
            if (!picture.empty()) w->picture = std::move(picture);
        }
        notePosition(w->slot, w->position());
    });
}

void PluginHost::editorClosed(const SlotPtr& slot) {
    auto it = editors.find(slot.get());
    if (it == editors.end()) return;
    ui::Window* win = it->second;
    editors.erase(it);
    withWindow(win, [&](auto* w) {
        if (w->tickTimer >= 0) {
            ui::App::get().cancelTimer(w->tickTimer);
            w->tickTimer = -1;
        }
        notePosition(slot, w->position());
        keepPicture(slot, w->picture);
    });
}

void PluginHost::closeEditor() {
    std::vector<ui::Window*> open;
    for (auto& kv : editors) open.push_back(kv.second);
    for (ui::Window* w : open) w->close();
}

void PluginHost::closeEditor(const std::vector<SlotPtr>& slots) {
    std::vector<ui::Window*> open;
    for (auto& kv : editors)
        for (auto& s : slots)
            if (s.get() == kv.first) open.push_back(kv.second);
    for (ui::Window* w : open) w->close();
}

void PluginHost::keepPicture(const SlotPtr& slot, const Image& image) {
    if (!image.empty() && plugin_pictures::save(*slot, image)) pictureTaken.emit(slot);
}

bool PluginHost::photograph(const SlotPtr& slot) {
    if (!slot || !plugin_audio::usable(slot) || editors.count(slot.get())) return false;
    if (effects::isBuiltin(slot->path)) {  // (its window is drawn by the program: no need to show it)
        auto* w = new EffectWindow(window, slot, slot->name, true);
        Image image = w->snapshot();
        delete w;
        keepPicture(slot, image);
        return !image.empty();
    }
    if (!plugin_windows::available()) return false;
    auto* w = new EditorWindow(window, slot, slot->name, true);
    if (!w->attach()) {
        delete w;
        return false;
    }
    w->showWindow();
    ui::App& app = ui::App::get();
    double shown = app.now();
    Image taken;
    bool done = false;
    int timer = app.every(PHOTO_TRY_MS, [&] {
        double now = app.now();
        if (done || now - shown < PHOTO_WAIT_S) return;
        Image captured, picture;
        bool ok = plugin_windows::capture(w, captured) && plugin_pictures::fromCapture(captured, picture);
        if (ok || now - shown > PHOTO_GIVE_UP_S) {
            if (ok) taken = std::move(picture);
            done = true;
        }
    });
    app.runUntil([&] { return done; });
    app.cancelTimer(timer);
    w->close();
    keepPicture(slot, taken);
    return !taken.empty();
}
