// Presets: the settings of one effect or instrument, or a whole track's chain, kept in files of their
// own to bring back on any track of any project. No GUI code (the menus are ui/presets_ui.h).
//
// WHERE. In the program's data folder (paths.h: $XDG_DATA_HOME/Exbit, ~/.local/share/Exbit):
//
//     presets/effects/<plugin name>/<preset>.json   one effect's or instrument's settings
//     presets/tracks/<preset>.json                  a track's chain
//     presets/tracks/<preset>_audio/<sample>.wav    …its built-in sampler's sample, copied there
//
// AN EFFECT PRESET is {"format": "exbit-preset", "version": 1, "kind": "effect", "plugin": {"path",
// "name", "plugin_name", "kind"}, "state": <base64 of the packed state>} (the state as projects save a
// slot's: plug::packState; a built-in effect's is the JSON of its values). It is listed for every slot of
// that plugin (by its name), and loading it gives the loaded plugin those settings on the GUI thread
// (plug::setState); a modulated knob's base follows (modulation::userEdited).
//
// A TRACK PRESET is the track's chain written as a one-track project (project_io's format, plus
// "preset": "track"): its effects with their places in the patch space and their cables, its modulation
// cables, its instrument (or instrument patch panel, or built-in sampler with its sample in
// "<preset>_audio"), and its MIDI effects. Not its clips, automation, sends, volume or pan. Cables from
// other tracks (sidechains) are left out. readTrack() reads one back without touching any project;
// applyTrack() puts it on a track (its old chain retired, never destroyed under the audio thread: one
// undo step, history.h). Plugins come back unloaded: the GUI loads them (PluginHost::restore) before
// applyTrack(); one that isn't installed stays as a silent slot keeping its settings (as in projects).
//
// COMPARE (A/B). Each effect or instrument can keep a second set of settings for a moment: keep()
// stores its settings now as KEPT; switchTo(kept) changes between NOW and KEPT, each remembering its own
// tweaks. GUI thread; forgotten with the slot.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/json.h"
#include "model/model.h"

namespace presets {

constexpr const char* FORMAT = "exbit-preset";
constexpr int VERSION = 1;
constexpr const char* TRACK = "track";  // a track preset's "preset" key

// the folders (not made here)
std::string folder();                                   // <data>/presets
std::string effectsFolder(const PluginSlot& slot);      // <data>/presets/effects/<plugin name>
std::string tracksFolder();                             // <data>/presets/tracks
// the plugin name a slot's presets are kept under (its name; "" for a track's own controls)
std::string pluginName(const PluginSlot& slot);
// a name fit for a file: no '/' nor control characters, not "." or "..", trimmed; "" if nothing is left
std::string safeName(const std::string& name);

struct Entry {
    std::string name;  // the file's name without ".json"
    std::string path;
};
// the presets in a folder (*.json), in natural order by name
std::vector<Entry> list(const std::string& folder);
// a track preset's audio folder: "<dir>/<stem>_audio"
std::string audioFolderOf(const std::string& presetPath);
// is this a track preset's file (a .json in the tracks folder, or one marked "preset": "track")?
bool isTrackPresetFile(const std::string& path);

// rename a preset (a track preset's audio folder too, and the file names in it); false and `error` if not
bool rename(const std::string& path, const std::string& newName, std::string& newPath, std::string& error);
// delete a preset (and a track preset's audio folder)
bool remove(const std::string& path, std::string& error);

// --- effect presets ---
// write the slot's settings now as the preset `name` in its plugin's folder (made if missing; an existing
// one of that name is replaced); `path`: where
bool saveEffect(const PluginSlot& slot, const std::string& name, std::string& path, std::string& error);
// the packed state an effect preset holds (and its plugin's name, path); false and `error` if it isn't one
bool readEffect(const std::string& path, std::string& state, std::string& plugin, std::string& error);
// give the slot's loaded plugin these settings (GUI thread); its saved state too, and its modulated knobs'
// bases follow. False if the slot has no loaded plugin (`error`).
bool applyState(Project& project, const SlotPtr& slot, const std::string& state, std::string& error);
// readEffect + applyState
bool applyEffect(Project& project, const SlotPtr& slot, const std::string& path, std::string& error);

// --- track presets ---
// write the track's chain as the preset `name` in the tracks folder (an existing one of that name is replaced)
bool saveTrack(const Track& track, int sr, const std::string& name, std::string& path, std::string& error);

// a track preset read back (nothing of any project changed)
struct TrackPreset {
    std::string name;
    Kind kind = Kind::Audio;            // the kind of track it was made from
    TrackPtr chain;                     // a track holding the chain: effects, cables, modulation cables,
                                        // instrument / rack / sampler, MIDI effects (its plugins unloaded)
    std::vector<SlotPtr> slots() const; // every plugin of it (instrument, rack, effects)
};
bool readTrack(const std::string& path, int sr, TrackPreset& out, std::string& error);

// What applyTrack() did (for the message, and the GUI to close windows of plugins gone)
struct Applied {
    std::vector<SlotPtr> removed;        // the slots the track no longer holds
    bool instrumentSkipped = false;      // the preset's instrument part left out: not a MIDI track
};
// Put the preset's chain on `track`, replacing its effects, cables and modulation (and, on a MIDI track
// from a MIDI track's preset, its instrument, instrument patch panel, sampler and MIDI effects). The
// preset's slots are taken over (load them first). Its MIDI clips are prepared again at `sr` (their
// built-in sound follows the instrument), lanes of plugins gone are dropped (automation::prune). GUI thread.
Applied applyTrack(Project& project, const TrackPtr& track, TrackPreset& preset, int sr);

// --- compare (A/B), per slot ---
struct Compare {
    bool hasKept = false;     // "Keep this" was chosen
    bool onKept = false;      // the slot has the KEPT settings now (else NOW)
    std::string other;        // the settings of the one not shown (packed state)
    std::vector<double> otherValues;  // a built-in effect's: its parameters' values (0…1) in `other`
};
Compare* compareOf(const PluginSlot* slot);   // null: nothing kept for it
// store the slot's settings now as KEPT (and show NOW); false if it has no loaded plugin
bool keep(Project& project, const SlotPtr& slot, std::string& error);
// change to KEPT (true) or NOW (false); the settings left are remembered as that one's. False if nothing
// is kept, or it is already showing that one.
bool switchTo(Project& project, const SlotPtr& slot, bool kept, std::string& error);
void forgetCompare(const PluginSlot* slot);
// a built-in effect's parameters whose values differ between what it has now and the other (empty: none, or
// nothing kept, or not a built-in effect)
std::vector<int> differing(const PluginSlot& slot);

}  // namespace presets
