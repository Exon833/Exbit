// Loading plugins and opening their windows, for the GUI (plugin_host.py).
#include "ui/plugin_host.h"

#include <algorithm>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/plugin_audio.h"
#include "core/util.h"
#include "effects/effects.h"
#include "ui/effect_window.h"
#include "ui/plugin_editor.h"
#include "ui/plugin_pictures.h"
#include "ui/plugin_windows.h"
#include "ui/toolkit/dialogs.h"

using namespace plugin_host;

PluginHost::PluginHost(ui::Window* window_, Project& project_, audio::Engine& engine_,
                       std::function<std::vector<std::string>()> vst2Folders_)
    : window(window_), project(project_), engine(engine_), vst2Folders(std::move(vst2Folders_)) {}

PluginHost::~PluginHost() {
    *alive_ = false;
    closeEditor();
    stopFindingKinds();
}

plug::PluginLibrary& PluginHost::library() {
    if (!library_)  // listing the plugin folders is quick, but only done when needed
        library_ = std::make_unique<plug::PluginLibrary>(folders.empty() ? plug::defaultFolders() : folders, cachePath,
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
// what both kinds of plugin window have, for the host: fn(EditorWindow*) or fn(EffectWindow*)
template <class F>
void withWindow(ui::Window* w, F fn) {
    if (auto* e = dynamic_cast<EditorWindow*>(w)) fn(e);
    else if (auto* x = dynamic_cast<EffectWindow*>(w)) fn(x);
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
            ui::msg::information(window, "No plugin window", slot->name + " has no window of its own.");
            return;
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
    };
    if (effects::isBuiltin(slot->path)) setUp(new EffectWindow(window, slot, title));
    else setUp(new EditorWindow(window, slot, title));
    if (!opened) return;
    opened->showWindow();
    opened->raiseWindow();
}

Image PluginHost::pictureOf(ui::Window* w) {
    if (auto* e = dynamic_cast<EffectWindow*>(w)) return e->snapshot();
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
