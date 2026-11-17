// The presets' menus and questions (model/presets.h holds the files and the rest).
//
// AN EFFECT OR INSTRUMENT: its Presets menu, in a built-in effect's window (the "Presets ▾" button), in its
// unit's right-click menu in the effects window (built-in or plugin: a plugin's own window is all the
// plugin's), and in a MIDI track's right-click menu for its instrument:
//     Save as…                        its settings now, under a name (asks before replacing one)
//     <its presets>                   click one to load it
//     Rename or delete ▸              <preset> ▸ Rename… / Delete…
//     Compare: Keep this              keep its settings now, to compare with (A/B)
//     Compare: Now / Kept             switch between them (each keeps its own tweaks); a built-in effect's
//                                     window marks the knobs that differ
// A TRACK: its right-click menu: "Save track preset…", "Load track preset ▸" (asks before replacing the
// track's chain; one undo step), and the Explorer's "Presets" place lists them: drag one onto a track (or
// below the tracks: a new track), or double-click one to load it onto the selected track. A preset whose
// plugin isn't installed loads the rest; the message says which are missing (silent, settings kept).
#pragma once
#include <functional>
#include <string>

#include "model/model.h"
#include "ui/toolkit/menu.h"

class PluginHost;
namespace ui { class Window; }

namespace presets_ui {

// What the menus need from the main window (set once; tests set their own)
struct Host {
    ui::Window* window = nullptr;                           // parent of the questions and messages
    Project* project = nullptr;
    PluginHost* plugins = nullptr;                          // loads a track preset's plugins
    std::function<int()> sampleRate;                        // the engine's (48000 if unset)
    std::function<void(const std::string& text, int ms)> status;
    std::function<void(const std::string& label)> nameStep; // the next undo step's name (history.h nameNext)
    std::function<void(const TrackPtr& track)> trackChanged;  // a track preset loaded onto it: show it afresh
    std::function<TrackPtr(Kind kind, const std::string& name)> newTrack;  // a new track for a preset
    std::function<void()> presetsChanged;                   // track presets saved, renamed or deleted (Explorer)
    std::function<void(const SlotPtr& slot)> settingsChanged;  // an effect's settings changed (preset, compare)
    // (tests) answers instead of asking: a name for "Save as…" / "Rename…", Yes / No for questions
    std::function<bool(const std::string& question, std::string& answer)> ask;
};
void setHost(Host host);
Host& host();

// --- an effect or instrument ---
void fillEffectMenu(ui::Menu& menu, ui::Window* parent, const SlotPtr& slot);
bool saveEffectAs(ui::Window* parent, const SlotPtr& slot);
bool loadEffect(ui::Window* parent, const SlotPtr& slot, const std::string& path);
bool keep(ui::Window* parent, const SlotPtr& slot);
bool compare(ui::Window* parent, const SlotPtr& slot, bool kept);

// --- any preset's file ---
bool renamePreset(ui::Window* parent, const std::string& path);
bool deletePreset(ui::Window* parent, const std::string& path);

// --- a track ---
bool saveTrackAs(ui::Window* parent, const TrackPtr& track);
// put the preset on the track (ask: confirm replacing its chain first); track null: onto a new track of the
// preset's kind (named after it). True if it was loaded.
bool loadTrack(ui::Window* parent, const TrackPtr& track, const std::string& path, bool ask = true);
// the track's right-click menu: Save track preset…, Load track preset ▸, its instrument's presets, and
// linking its fader, pan or a plugin's parameter to a MIDI controller (midi_learn.h)
void fillTrackMenu(ui::Menu& menu, ui::Window* parent, const TrackPtr& track);

}  // namespace presets_ui
