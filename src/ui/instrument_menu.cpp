// Choosing a MIDI track's instrument, and opening its window (instrument_menu.py).
#include "ui/instrument_menu.h"

#include "audio/patch.h"
#include "audio/plugin_audio.h"
#include "core/util.h"
#include "ui/midi_ports_dialog.h"
#include "ui/pad_kit_dialog.h"
#include "ui/plugin_dialog.h"
#include "ui/plugin_host.h"
#include "ui/sampler_dialog.h"
#include "ui/synth_dialog.h"
#include "ui/toolkit/dialogs.h"

namespace instrument_menu {

bool samePlugin(const plug::PluginInfo& info, const PluginSlot* slot) {
    return slot && info.path == slot->path && info.pluginName == slot->pluginName;
}

namespace {
ui::Action* checkable(ui::Menu* menu, const std::string& text, bool checked, std::function<void()> fn) {
    ui::Action* a = menu->addAction(text, std::move(fn));
    a->checkable = true;
    a->checked = checked;
    return a;
}
}  // namespace

ui::Menu* fillInstrumentMenu(ui::Menu* menu, ui::Window* parent, PluginHost& host, const TrackPtr& track,
                             std::function<void()> chosen, OpenPanel openPanel) {
    menu->clear();
    if (aload(track->frozen)) {
        menu->addAction(track->name + " is frozen: unfreeze it to change its instrument")->setEnabled(false);
        return menu;
    }
    SlotPtr current = track->needsInstrument || track->usesRack || track->ext.on ? nullptr : aload(track->instrument);
    if (current && host.canReload(current))
        menu->addAction("Reload " + current->name + " (it stopped)", [&host, track, current] {
            ui::App::get().singleShot(0, [&host, track, current] {  // (once the menu has gone; still its instrument)
                if (aload(track->instrument) == current) host.reloadStopped(current);
            });
        });
    if (current) {
        menu->addAction("Open the " + current->name + " window", [parent, &host, track, openPanel] {
                editInstrument(parent, host, track, openPanel);
            })->setEnabled(plugin_audio::usable(current));
        menu->addSeparator();
    }
    bool builtIn = !current && !track->needsInstrument && !track->usesRack && !track->sampler && !track->padKit && !track->ext.on;
    subsynth::PatchPtr patch = std::atomic_load(&track->synth);
    bool drawn = builtIn && patch && patch->drawn;
    checkable(menu, BUILTIN, builtIn && !drawn, [parent, &host, track, chosen] {
        subsynth::PatchPtr now = std::atomic_load(&track->synth);
        if (now && now->drawn) std::atomic_store(&track->synth, subsynth::PatchPtr());  // (the classic voice again)
        setInstrument(parent, host, track, nullptr, chosen);
        if (auto* d = synth_dialog::openFor(track.get())) d->reload();  // (an open canvas: the knobs again)
    });
    if (builtIn && !drawn)  // (its patch: ui/synth_dialog.h)
        menu->addAction(SYNTH, [track] { synth_dialog::openSynth(track); })->tooltip =
            "Its oscillators, filter, envelopes, LFO, modulation and presets";
    checkable(menu, DRAWN, drawn, [parent, &host, track, chosen] { useDrawn(parent, host, track, chosen); })->tooltip =
        "A synth with nothing to set: draw one cycle of its wave, and every key plays it";
    SamplerPtr sampler = std::atomic_load(&track->sampler);
    ui::Action* sampled = checkable(
        menu, sampler && sampler->sample ? "Built-in sampler: " + sampler->sample->name + "\xe2\x80\xa6" : std::string(SAMPLER),
        track->playsSampler() && !track->needsInstrument,
        [parent, &host, track, chosen] { useSampler(parent, host, track, chosen); });
    sampled->tooltip = "Play one sample across the keyboard, tuned to each key (asks for the sample the first time)";
    PadKitPtr kit = std::atomic_load(&track->padKit);
    int used = kit ? kit->used() : 0;
    ui::Action* padded = checkable(
        menu, kit ? "Pad Kit (" + std::to_string(used) + " pad" + (used == 1 ? "" : "s") + ")\xe2\x80\xa6" : std::string(PAD_KIT),
        track->playsPadKit() && !track->needsInstrument,
        [parent, &host, track, chosen] { usePadKit(parent, host, track, chosen); });
    padded->tooltip = "Drums: a sound on each of 32 keys, each with its own settings and choke group";
    if (kit && used && track->playsPadKit() && !track->needsInstrument)  // (model/pad_tracks.h)
        menu->addAction("Pads to tracks", [track] { pad_kit_dialog::padsToTracks(track); })->tooltip =
            "Every used pad on a track of its own (its own effects, fader, pan, sends and meter), grouped with this one";
    int count = (int)track->rack.size();
    ui::Action* rack =
        checkable(menu, count ? "Instrument patch panel (" + std::to_string(count) + " instrument" + (count == 1 ? "" : "s") + ")…"
                              : std::string(RACK),
                  track->usesRack && !track->needsInstrument && !track->ext.on,
                  [parent, &host, track, chosen, openPanel] { useRack(parent, host, track, chosen, openPanel); });
    rack->tooltip = "Several instruments on this track at once, each with its own volume";
    ui::Action* external = checkable(  // (a hardware synth: ui/midi_ports_dialog.h)
        menu, track->ext.on ? "External synth: " + track->ext.port + "\xe2\x80\xa6" : std::string(EXTERNAL),
        track->ext.on && !track->needsInstrument,
        [parent, track, chosen] { midi_ports_dialog::editExternal(parent, track, chosen); });
    external->tooltip = "A hardware synth: its notes go out to a MIDI port, its sound comes back on an audio input";
    menu->addSeparator();
    std::vector<plug::PluginInfo> found = host.library().ofKind(plug::INSTRUMENT);
    if (current) {
        bool listed = false;
        for (auto& info : found)
            if (samePlugin(info, current.get())) listed = true;
        if (!listed)  // e.g. not installed any more
            checkable(menu, plugin_dialog::slotLabel(*current), true, nullptr)->setEnabled(false);
    }
    std::vector<plug::PluginInfo> known, untried;  // (never loaded, so they may be effects)
    for (auto& info : found) (info.kind.empty() ? untried : known).push_back(info);
    addEntries(menu, known, parent, host, track, current, chosen);
    if (!known.empty() && !untried.empty())  // out of the way, in a submenu
        addEntries(menu->addMenu("Not loaded before (" + std::to_string(untried.size()) + ")"), untried, parent, host,
                   track, current, chosen, false);
    else
        addEntries(menu, untried, parent, host, track, current, chosen);
    if (found.empty()) menu->addAction("(no VST3 instruments found)")->setEnabled(false);
    return menu;
}

void addEntries(ui::Menu* menu, const std::vector<plug::PluginInfo>& infos, ui::Window* parent, PluginHost& host,
                const TrackPtr& track, const SlotPtr& current, std::function<void()> chosen, bool labels) {
    for (const auto& info : infos)
        checkable(menu, labels ? info.label() : info.name, samePlugin(info, current.get()),
                  [parent, &host, track, info, chosen] { setInstrument(parent, host, track, &info, chosen); });
}

bool setInstrument(ui::Window* parent, PluginHost& host, const TrackPtr& track, const plug::PluginInfo* info,
                   std::function<void()> chosen) {
    SlotPtr old = track->needsInstrument ? nullptr : aload(track->instrument);
    SlotPtr slot;
    if (!info) {
        slot = nullptr;
    } else if (samePlugin(*info, old.get()) && aload(old->instance)) {
        slot = old;  // the one it has already
    } else {
        plug::PluginInfo wanted = *info;  // (a copy: the menu holding `info` may go away meanwhile)
        slot = host.load(wanted);
        if (!slot) return false;
        if (slot->kind != plug::INSTRUMENT) {
            ui::msg::information(parent, "Not an instrument",
                                 slot->name + " is an effect. Add it with the track's FX button instead.");
            host.project.retire(aload(slot->instance));
            return false;
        }
    }
    if (old && slot != old) host.closeEditor({old});
    std::atomic_store(&track->instrument, slot);  // loaded already, so the audio thread can use it at once
    track->needsInstrument = false;
    track->ext.on = false;  // (an External synth's settings kept, to come back to)
    if (!info) {  // (the built-in synth, rather than the sampler or Pad Kit)
        std::atomic_store(&track->sampler, SamplerPtr());
        std::atomic_store(&track->padKit, PadKitPtr());
        if (auto* d = pad_kit_dialog::openFor(track.get())) d->close();
        if (auto* d = sampler_dialog::openFor(track.get())) d->close();
    }
    if (info)  // (its patch kept, not heard)
        if (auto* d = synth_dialog::openFor(track.get())) d->close();
    if (track->usesRack) {  // (its instrument patch panel stays as it is, not heard)
        track->usesRack = false;
        host.closeEditor(track->rack.copy());
    }
    if (old && slot != old && !track->rack.contains(old))
        host.project.retire(aload(old->instance));  // (let go of on this thread, never the audio thread's)
    sampler_dialog::renderTrackClips(track);  // (the built-in synth's or sampler's rendering, as it now plays)
    if (chosen) chosen();
    return true;
}

namespace {
// the track plays its built-in instrument (sampler or Pad Kit) from now on: no plugin, no patch panel
void builtInFromNow(PluginHost& host, const TrackPtr& track) {
    SlotPtr old = aload(track->instrument);
    if (old) host.closeEditor({old});
    if (track->usesRack) host.closeEditor(track->rack.copy());
    std::atomic_store(&track->instrument, SlotPtr());
    track->usesRack = false;
    track->needsInstrument = false;
    track->ext.on = false;
    if (old && !track->rack.contains(old)) host.project.retire(aload(old->instance));
}
}  // namespace

bool useSampler(ui::Window* parent, PluginHost& host, const TrackPtr& track, std::function<void()> chosen) {
    if (!track->sampler) {
        ClipPtr sample = sampler_dialog::chooseSample(parent);
        if (!sample) return false;
        std::atomic_store(&track->sampler, sampler_dialog::newSettings(sample));
    }
    std::atomic_store(&track->padKit, PadKitPtr());  // (the sampler rather than the Pad Kit: see the header)
    if (auto* d = synth_dialog::openFor(track.get())) d->close();  // (its patch kept, not heard)
    if (auto* d = pad_kit_dialog::openFor(track.get())) d->close();
    builtInFromNow(host, track);
    sampler_dialog::renderTrackClips(track);
    if (chosen) chosen();
    sampler_dialog::openSampler(track);
    return true;
}

void useDrawn(ui::Window* parent, PluginHost& host, const TrackPtr& track, std::function<void()> chosen) {
    subsynth::PatchPtr now = std::atomic_load(&track->synth);
    if (!now || !now->drawn) std::atomic_store(&track->synth, subsynth::PatchPtr(std::make_shared<const subsynth::Patch>(subsynth::drawnPatch())));
    util::debugf("instrument: %s plays the Drawn synth\n", track->name.c_str());
    if (!setInstrument(parent, host, track, nullptr, chosen)) return;
    synth_dialog::openSynth(track);
}

void usePadKit(ui::Window* parent, PluginHost& host, const TrackPtr& track, std::function<void()> chosen) {
    if (!track->padKit) std::atomic_store(&track->padKit, PadKitPtr(std::make_shared<PadKit>()));
    if (auto* d = sampler_dialog::openFor(track.get())) d->close();  // (its settings kept, not heard)
    if (auto* d = synth_dialog::openFor(track.get())) d->close();
    builtInFromNow(host, track);
    sampler_dialog::renderTrackClips(track);
    if (chosen) chosen();
    pad_kit_dialog::openPadKit(track);
}

void useRack(ui::Window* parent, PluginHost& host, const TrackPtr& track, std::function<void()> chosen,
             OpenPanel openPanel) {
    if (!track->usesRack || track->needsInstrument || track->ext.on) {
        SlotPtr single = track->needsInstrument ? nullptr : aload(track->instrument);
        if (track->rack.empty() && single) {
            single->placed = false;
            track->rackCables = std::vector<Cable>{Cable(End::in(), End::of(single.get())),
                                                   Cable(End::of(single.get()), End::out())};
            track->rack = std::vector<SlotPtr>{single};
            patch::ensureStates(host.project);
        }
        track->usesRack = true;  // (the panel is ready first: the audio thread never finds half of it)
        track->needsInstrument = false;
        track->ext.on = false;
        SlotPtr now = aload(track->instrument);
        if (now && track->rack.contains(now)) std::atomic_store(&track->instrument, SlotPtr());
        if (chosen) chosen();
    }
    if (openPanel) openPanel(track);
}

void editInstrument(ui::Window* parent, PluginHost& host, const TrackPtr& track, OpenPanel openPanel) {
    SlotPtr slot = aload(track->instrument);
    if (aload(track->frozen)) {
        ui::msg::information(parent, "Track frozen",
                             track->name + " is frozen: it plays its rendering, so changes to its instrument wouldn't "
                                           "be heard.\nUnfreeze it first (right-click its row).");
    } else if (track->ext.on && !track->needsInstrument) {
        midi_ports_dialog::editExternal(parent, track);
    } else if (track->usesRack && !track->needsInstrument) {
        if (openPanel) openPanel(track);
    } else if (track->playsPadKit() && !track->needsInstrument) {
        pad_kit_dialog::openPadKit(track);
    } else if (track->playsSampler() && !track->needsInstrument) {
        sampler_dialog::openSampler(track);
    } else if (track->needsInstrument) {
        ui::msg::information(parent, "No instrument yet",
                             track->name + " is silent until it has an instrument: choose one with ▾ next to 🎹.");
    } else if (!slot) {
        synth_dialog::openSynth(track);  // (the built-in synth's window)
    } else if (host.canReload(slot)) {
        if (ui::msg::question(parent, "Instrument stopped",
                              slot->name + " stopped (" + slot->error + ").\n\nReload it, with the settings it last had?") ==
            ui::msg::Yes) {
            std::string why;
            if (host.reload(slot, why)) host.edit(slot, track->name);  // (then its window, as asked)
            else ui::msg::warning(parent, "Cannot reload plugin", slot->name + "\n\n" + why);
        }
    } else if (!plugin_audio::usable(slot)) {
        ui::msg::information(parent, "Instrument not loaded",
                             slot->name + " isn't loaded, so its window can't open. Choose it again with ▾ next to 🎹.");
    } else {
        host.edit(slot, track->name);
    }
}

}  // namespace instrument_menu
