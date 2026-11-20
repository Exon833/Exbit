// Choosing a MIDI track's instrument, and opening its window to configure it (instrument_menu.py).
//
// The menu belongs to the "Choose instrument" button that fills a new MIDI track's row, and to the
// ▾ of the instrument button (🎹) on its row once it has one: the built-in synth, or a VST
// instrument found on this computer (plugins/library.h), loaded with the usual "please wait" window
// (plugin_host.h). The track is silent until its first instrument is chosen and loaded. Clicking
// 🎹 itself opens the instrument's own window (editInstrument).
//
// "Built-in sampler…" plays one sample across the keyboard (audio/sampler; its window: sampler_dialog.h),
// asking for the sample the first time; 🎹 then opens its window. Choosing another instrument keeps its
// settings, to come back to (the built-in synth forgets them).
//
// "Pad Kit…" plays a sample on each of 32 keys (drums: audio/padkit.h; its window: pad_kit_dialog.h),
// starting with an empty kit to drop sounds on; 🎹 then opens its window. Another instrument plugin keeps
// it, to come back to; the built-in synth and the built-in sampler forget it (undo brings it back). "Pads to tracks"
// (once it has pads) gives every used pad a track of its own (model/pad_tracks.h).
//
// "Instrument patch panel…" makes the track play several instruments at once instead, from its
// instrument patch panel (InstrumentPanelDialog; 🎹 then opens it). Its one instrument goes in it
// first, wired, so it keeps sounding. Choosing a single instrument again leaves the panel as it is
// (not heard), to go back to.
//
// openPanel(track): the main window's open_instrument_panel (Python: parent.open_instrument_panel).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "model/model.h"
#include "plugins/library.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

class PluginHost;

namespace instrument_menu {

constexpr const char* BUILTIN = "Built-in synth";
constexpr const char* RACK = "Instrument patch panel…";
constexpr const char* SAMPLER = "Built-in sampler…";
constexpr const char* PAD_KIT = "Pad Kit…";
using OpenPanel = std::function<void(const TrackPtr&)>;

bool samePlugin(const plug::PluginInfo& info, const PluginSlot* slot);
// Fill `menu` with the instruments to choose from (the track's own ticked) and, once it has a VST
// instrument, a way to open its window. chosen() is called after the track's instrument changed.
ui::Menu* fillInstrumentMenu(ui::Menu* menu, ui::Window* parent, PluginHost& host, const TrackPtr& track,
                             std::function<void()> chosen, OpenPanel openPanel);
// one entry per plugin (labels: with "type not known yet" for those never loaded)
void addEntries(ui::Menu* menu, const std::vector<plug::PluginInfo>& infos, ui::Window* parent, PluginHost& host,
                const TrackPtr& track, const SlotPtr& current, std::function<void()> chosen, bool labels = true);
// load `info` (null: the built-in synth) as the track's instrument; false if it couldn't be loaded
// (the track keeps what it had)
bool setInstrument(ui::Window* parent, PluginHost& host, const TrackPtr& track, const plug::PluginInfo* info,
                   std::function<void()> chosen);
// make the track play its built-in sampler (asking for a sample if it has none yet), and open its window;
// false if no sample was chosen
bool useSampler(ui::Window* parent, PluginHost& host, const TrackPtr& track, std::function<void()> chosen);
// make the track play its Pad Kit (an empty one the first time), and open its window
void usePadKit(ui::Window* parent, PluginHost& host, const TrackPtr& track, std::function<void()> chosen);
// make the track play its instrument patch panel, and open it; the first time, the instrument it
// has goes in it, wired, so it keeps sounding
void useRack(ui::Window* parent, PluginHost& host, const TrackPtr& track, std::function<void()> chosen,
             OpenPanel openPanel);
// open the track's VST instrument's own window (or its instrument patch panel); the built-in synth
// has no settings
void editInstrument(ui::Window* parent, PluginHost& host, const TrackPtr& track, OpenPanel openPanel);

}  // namespace instrument_menu
