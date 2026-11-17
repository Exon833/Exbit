// The presets' menus and questions (presets_ui.h).
#include "ui/presets_ui.h"

#include "audio/plugin_audio.h"
#include "core/util.h"
#include "model/presets.h"
#include "plugins/library.h"
#include "ui/midi_learn.h"
#include "ui/plugin_host.h"
#include "ui/toolkit/dialogs.h"

namespace presets_ui {

namespace {

Host gHost;

const char* const QUOTE_L = "\xe2\x80\x9c";
const char* const QUOTE_R = "\xe2\x80\x9d";
const char* const DOTS = "\xe2\x80\xa6";

std::string quoted(const std::string& s) { return QUOTE_L + s + QUOTE_R; }

int sampleRate() { return gHost.sampleRate ? gHost.sampleRate() : 48000; }

void say(const std::string& text, int ms = 4000) {
    if (gHost.status) gHost.status(text, ms);
}

// a name (asked; `text` holds the suggestion); false if cancelled or empty
bool askName(ui::Window* parent, const std::string& title, const std::string& label, std::string& text) {
    if (gHost.ask) {
        if (!gHost.ask(title, text)) return false;
    } else if (!ui::getText(parent, title, label, text)) {
        return false;
    }
    text = presets::safeName(text);
    return !text.empty();
}

bool askYes(ui::Window* parent, const std::string& title, const std::string& text) {
    if (gHost.ask) {
        std::string unused;
        return gHost.ask(title + "\n" + text, unused);
    }
    return ui::msg::question(parent, title, text) == ui::msg::Yes;
}

void warn(ui::Window* parent, const std::string& title, const std::string& text) {
    if (gHost.ask) {  // (tests: said in the status bar instead)
        say(title + ": " + text);
        return;
    }
    ui::msg::warning(parent, title, text);
}

void settingsChanged(const SlotPtr& slot) {
    if (gHost.plugins) gHost.plugins->edited = true;  // (its settings may differ from the saved project's)
    if (gHost.settingsChanged) gHost.settingsChanged(slot);
}

}  // namespace

void setHost(Host h) { gHost = std::move(h); }
Host& host() { return gHost; }

// --- an effect or instrument ---------------------------------------------------------------------

bool saveEffectAs(ui::Window* parent, const SlotPtr& slot) {
    if (!slot || !gHost.project) return false;
    std::string name = "My " + slot->name;
    if (!askName(parent, "Save preset", "A name for this preset of " + slot->name + ":", name)) return false;
    std::string path = util::joinPath(presets::effectsFolder(*slot), name + ".json");
    if (util::fileExists(path) &&
        !askYes(parent, "Replace preset", "There is a preset called " + quoted(name) + " already. Replace it?"))
        return false;
    std::string error;
    if (!presets::saveEffect(*slot, name, path, error)) {
        warn(parent, "Cannot save preset", error);
        return false;
    }
    say("Saved the preset " + quoted(name) + " of " + slot->name);
    return true;
}

bool loadEffect(ui::Window* parent, const SlotPtr& slot, const std::string& path) {
    if (!slot || !gHost.project) return false;
    std::string error;
    if (!presets::applyEffect(*gHost.project, slot, path, error)) {
        warn(parent, "Cannot load preset", error);
        return false;
    }
    settingsChanged(slot);
    say(slot->name + ": loaded the preset " + quoted(util::stem(path)));
    return true;
}

bool keep(ui::Window* parent, const SlotPtr& slot) {
    if (!slot || !gHost.project) return false;
    std::string error;
    if (!presets::keep(*gHost.project, slot, error)) {
        warn(parent, "Compare", error);
        return false;
    }
    settingsChanged(slot);
    say(slot->name + ": these settings are kept. Change them, then switch between Now and Kept to compare");
    return true;
}

bool compare(ui::Window* parent, const SlotPtr& slot, bool kept) {
    if (!slot || !gHost.project) return false;
    std::string error;
    if (!presets::switchTo(*gHost.project, slot, kept, error)) {
        if (!error.empty()) warn(parent, "Compare", error);
        return false;
    }
    settingsChanged(slot);
    say(slot->name + (kept ? ": the kept settings" : ": the settings it had now"), 2500);
    return true;
}

bool renamePreset(ui::Window* parent, const std::string& path) {
    std::string name = util::stem(path);
    if (!askName(parent, "Rename preset", "New name for the preset " + quoted(name) + ":", name)) return false;
    std::string newPath, error;
    if (!presets::rename(path, name, newPath, error)) {
        warn(parent, "Cannot rename preset", error);
        return false;
    }
    if (gHost.presetsChanged) gHost.presetsChanged();
    say("Renamed the preset to " + quoted(util::stem(newPath)));
    return true;
}

bool deletePreset(ui::Window* parent, const std::string& path) {
    std::string name = util::stem(path);
    if (!askYes(parent, "Delete preset", "Delete the preset " + quoted(name) + "? This can't be undone.")) return false;
    std::string error;
    if (!presets::remove(path, error)) {
        warn(parent, "Cannot delete preset", error);
        return false;
    }
    if (gHost.presetsChanged) gHost.presetsChanged();
    say("Deleted the preset " + quoted(name));
    return true;
}

namespace {

// "<preset> ▸ Rename… / Delete…" for each preset of a folder
void addManageMenu(ui::Menu& menu, ui::Window* parent, const std::vector<presets::Entry>& entries) {
    if (entries.empty()) return;
    ui::Menu* manage = menu.addMenu("Rename or delete");
    for (const auto& e : entries) {
        ui::Menu* one = manage->addMenu(e.name);
        std::string path = e.path;
        one->addAction(std::string("Rename") + DOTS, [parent, path] { renamePreset(parent, path); })->tooltip =
            "Give the preset " + quoted(e.name) + " another name";
        one->addAction(std::string("Delete") + DOTS, [parent, path] { deletePreset(parent, path); })->tooltip =
            "Delete the preset " + quoted(e.name) + " (asks first)";
    }
}

}  // namespace

void fillEffectMenu(ui::Menu& menu, ui::Window* parent, const SlotPtr& slot) {
    if (!slot) return;
    bool loaded = (bool)aload(slot->instance);
    std::string folder = presets::effectsFolder(*slot);
    ui::Action* save = menu.addAction(std::string("Save as") + DOTS, [parent, slot] { saveEffectAs(parent, slot); });
    save->tooltip = "Keep " + slot->name + "'s settings now as a preset, to load on any track of any project";
    save->setEnabled(loaded);
    std::vector<presets::Entry> entries = presets::list(folder);
    menu.addSection("Presets of " + presets::pluginName(*slot));
    for (const auto& e : entries) {
        std::string path = e.path;
        ui::Action* a = menu.addAction(e.name, [parent, slot, path] { loadEffect(parent, slot, path); });
        a->tooltip = "Load this preset: " + slot->name + " takes its settings";
        a->setEnabled(loaded);
    }
    if (entries.empty()) menu.addAction("(no presets yet)")->setEnabled(false);
    addManageMenu(menu, parent, entries);
    menu.addSeparator();
    menu.addSection("Compare");
    presets::Compare* c = presets::compareOf(slot.get());
    bool kept = c && c->hasKept;
    ui::Action* k = menu.addAction("Keep this", [parent, slot] { keep(parent, slot); });
    k->tooltip = "Keep the settings it has now, then change them and switch between Now and Kept to compare (A/B)";
    k->setEnabled(loaded);
    ui::Action* now = menu.addCheckable("Now", !kept || !c->onKept, [parent, slot](bool) { compare(parent, slot, false); });
    now->group = 1;
    now->tooltip = "The settings as they are (or as they were when you switched to Kept)";
    now->setEnabled(kept && loaded);
    ui::Action* other = menu.addCheckable("Kept", kept && c->onKept, [parent, slot](bool) { compare(parent, slot, true); });
    other->group = 1;
    other->tooltip = kept ? "The settings kept with \xe2\x80\x9cKeep this\xe2\x80\x9d (a built-in effect's window marks the knobs "
                            "that differ)"
                          : "Choose \xe2\x80\x9cKeep this\xe2\x80\x9d first";
    other->setEnabled(kept && loaded);
}

// --- a track -------------------------------------------------------------------------------------

bool saveTrackAs(ui::Window* parent, const TrackPtr& track) {
    if (!track) return false;
    std::string name = track->name;
    if (!askName(parent, "Save track preset",
                 "A name for the preset of " + track->name + "'s effects, instrument and MIDI effects:", name))
        return false;
    std::string path = util::joinPath(presets::tracksFolder(), name + ".json");
    if (util::fileExists(path) &&
        !askYes(parent, "Replace preset", "There is a track preset called " + quoted(name) + " already. Replace it?"))
        return false;
    std::string error;
    if (!presets::saveTrack(*track, sampleRate(), name, path, error)) {
        warn(parent, "Cannot save track preset", error);
        return false;
    }
    if (gHost.presetsChanged) gHost.presetsChanged();
    say("Saved the track preset " + quoted(name) + " (Explorer \xe2\x86\x92 Presets)");
    return true;
}

bool loadTrack(ui::Window* parent, const TrackPtr& target, const std::string& path, bool ask) {
    if (!gHost.project) return false;
    Project& project = *gHost.project;
    presets::TrackPreset preset;
    std::string error;
    if (!presets::readTrack(path, sampleRate(), preset, error)) {
        warn(parent, "Cannot load track preset", error);
        return false;
    }
    TrackPtr track = target;
    if (track && std::atomic_load(&track->frozen)) {
        warn(parent, "Track frozen", track->name + " is frozen, so its plugins are locked. Unfreeze it first.");
        return false;
    }
    if (track && ask) {
        bool hasChain = !track->effects.empty() || track->instrument || track->usesRack || track->sampler ||
                        !track->midiEffects.empty();
        std::string what = track->kind == Kind::Midi && preset.kind == Kind::Midi
                               ? "effects, instrument and MIDI effects"
                               : "effects";
        std::string note = preset.kind == Kind::Midi && track->kind != Kind::Midi
                               ? "\n\n(Its instrument and MIDI effects are for MIDI tracks: only its effects are used.)"
                               : "";
        if (hasChain && !askYes(parent, "Load track preset",
                                "Replace the " + what + " of " + track->name + " with the track preset " +
                                    quoted(preset.name) + "? (Undo brings them back.)" + note))
            return false;
    }
    if (!track && !gHost.newTrack) return false;
    // its plugins loaded first (missing ones stay as silent slots that keep their settings)
    std::vector<std::string> missing;
    std::vector<SlotPtr> slots = preset.slots();
    if (!slots.empty()) {
        if (gHost.plugins) {
            missing = gHost.plugins->restore(slots);
        } else {
            for (auto& s : slots) {
                std::string why;
                std::unique_ptr<plug::Plugin> p = plug::Plugin::load(s->path, s->pluginName, why);
                if (p) {
                    p->prepare(sampleRate());
                    if (!s->state.empty()) {
                        try {
                            plug::setState(*p, s->state);
                        } catch (const std::exception&) {
                        }
                    }
                    std::atomic_store(&s->instance, std::shared_ptr<plug::Plugin>(std::move(p)));
                } else {
                    missing.push_back(s->name);
                }
            }
        }
    }
    if (gHost.nameStep) gHost.nameStep("Load track preset");  // (a new track for it is part of the step)
    if (!track) {
        track = gHost.newTrack(preset.kind, preset.name);
        if (!track) return false;
    }
    presets::Applied done = presets::applyTrack(project, track, preset, sampleRate());
    if (gHost.plugins && !done.removed.empty()) gHost.plugins->closeEditor(done.removed);
    if (gHost.trackChanged) gHost.trackChanged(track);
    std::string text = "Loaded the track preset " + quoted(preset.name) + " onto " + track->name;
    if (done.instrumentSkipped) text += " (its instrument and MIDI effects are for MIDI tracks: left out)";
    say(text, 5000);
    if (!missing.empty()) {
        std::string names;
        for (auto& m : missing) names += "\n" + m;
        warn(parent, "Plugins missing",
             "These plugins of the preset " + quoted(preset.name) + " aren't installed here, so they are silent:\n" +
                 names + "\n\nThe rest of the preset is loaded; their settings are kept.");
    }
    return true;
}

void fillTrackMenu(ui::Menu& menu, ui::Window* parent, const TrackPtr& track) {
    if (!track) return;
    menu.addSeparator();
    ui::Action* save = menu.addAction(std::string("Save track preset") + DOTS, [parent, track] { saveTrackAs(parent, track); });
    save->tooltip = "Keep this track's effects (wired as they are, with their modulation), instrument and MIDI effects "
                    "as a preset, to load onto any track";
    std::vector<presets::Entry> entries = presets::list(presets::tracksFolder());
    ui::Menu* load = menu.addMenu("Load track preset");
    if (track->frozen) {
        load->addAction(track->name + " is frozen: unfreeze it first")->setEnabled(false);
    } else {
        for (const auto& e : entries) {
            std::string path = e.path;
            load->addAction(e.name, [parent, track, path] { loadTrack(parent, track, path); })->tooltip =
                "Replace this track's effects (and a MIDI track's instrument and MIDI effects) with this preset's "
                "(asks first; undo brings them back)";
        }
        if (entries.empty()) load->addAction("(no track presets yet)")->setEnabled(false);
        addManageMenu(*load, parent, entries);
    }
    // the instrument plugin's own presets
    SlotPtr inst = track->kind == Kind::Midi && !track->needsInstrument && !track->usesRack ? aload(track->instrument)
                                                                                            : nullptr;
    if (inst && !track->frozen) {
        ui::Menu* sub = menu.addMenu(inst->name + " presets");
        fillEffectMenu(*sub, parent, inst);
    }
    // MIDI learn: the fader, the pan knob, a plugin's parameter
    if (midi_learn::active()) {
        ui::Menu* link = menu.addMenu("Link to a MIDI controller");
        midi_learn::fillParamMenu(*link, track, track->controls, "Volume", std::string("Volume fader") + DOTS);
        midi_learn::fillParamMenu(*link, track, track->controls, "Pan", std::string("Pan knob") + DOTS);
        link->addSeparator();
        auto plugins = track->instruments();
        for (auto& s : *track->effects) plugins.push_back(s);
        for (auto& s : plugins) {
            if (!aload(s->instance)) continue;
            link->addAction("A knob of " + s->name + DOTS, [parent, track, s] {
                    midi_learn::chooseAndLearn(parent, track, s);
                })->tooltip = "Choose one of " + s->name + "'s parameters, then turn a knob on your MIDI controller";
        }
    }
}

}  // namespace presets_ui
