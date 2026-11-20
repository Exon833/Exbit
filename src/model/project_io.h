// Saving and loading projects (project_io.py). No GUI code.
//
// A saved project is a JSON file plus a folder next to it named "<name>_audio":
//
//     MySong.json
//     MySong_audio/
//         drums.wav
//         vocals.wav
//
// The folder holds the audio of every audio clip as 32-bit float WAV, exactly what the program
// plays from (whatever format it was imported from). Clips with the same audio share one file:
// split clips, and the same sample used several times (identical audio is recognised by its
// content). Saving again reuses files already in the folder and never overwrites or deletes any.
// MIDI notes are stored in the JSON itself. Times are in seconds, so a project opens at any sample
// rate. The tempo, time signature, grid view and loop range are stored under "timing".
//
// Plugins are stored by their file path plus their settings (base64 of the plugin's packed state);
// the plugins themselves stay where they are installed. loadProject() only restores these
// PluginSlots: loading the plugins is the GUI's job, on the main thread (project.pluginSlots() lists
// them). Effects also keep their place in the patch space ("at"), and "cables" / "master_cables" list
// how they are wired: [from, to] with "in", "out", an effect's number, ["side", number] for an
// effect's sidechain input, or ["track", number] for another track (its place in "tracks") heard by
// a sidechain. Projects saved before effects were wired by hand have no cables: their effects are
// wired one after the other, as they played.
//
// Audio files are named relative to the JSON's folder, or to the folder "base" names (relative to the
// JSON's folder) when there is one: a backup sits a folder below its project and has "base": "..". A
// file can also be named by its absolute path (recovery files do that for audio already saved somewhere).
//
// A MIDI track's built-in sampler keeps its sample in the audio folder too (as clips do), its settings
// under the track's "sampler"; recorded takes are under the track's "takes" (audio in the folder). A
// Pad Kit (model.h PAD KIT) is under the track's "pad_kit": {"pads": [...]}, only the pads holding a sample
// or changed (each: "pad" its place from 0, "note", "choke", and the sampler's keys with its "file" in the
// audio folder); a track without one has no such key, so older projects are saved and opened as before.
//
// Linked MIDI clips (model.h) each keep their notes as any MIDI clip does, plus a "link" number shared by
// the clips linked together (1, 2… in the order they first appear); on loading, the clips with the same
// number share one link and the first one's notes. A clip linked to no other saved clip has no "link", so
// projects without linked clips are saved exactly as before, and older projects open unchanged.
//
// Backups: saving over a project first keeps the file being replaced in "<name>_backups" next to it,
// as "<name> <date> <time>.json" (the newest BACKUPS_KEPT). Their audio stays in the project's audio
// folder, which saving never deletes from, so a backup opens as it was.
//
// Recovery (autosave): describeRecovery() describes the project with every audio file named by an
// absolute path, and lists the audio that isn't saved anywhere yet (recordings, imported files), to be
// written into the recovery folder by writeRecovery() on a worker thread. Nothing in the project changes.
//
// The files are the Python program's: same keys, nesting and value types, so a project opens in both.
#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/json.h"
#include "model/model.h"

namespace project_io {

constexpr const char* FORMAT = "tinydaw-project";
constexpr int VERSION = 1;
constexpr int BACKUPS_KEPT = 10;  // backups kept next to a project (the oldest go)

// What a saved project keeps of the master strip ("master": {"volume_db": …, "mute": …})
struct MasterSettings {
    double volumeDb = 0;  // the master fader (the Python program's slider: whole dB)
    bool mute = false;
    Json toJson() const;
    static MasterSettings fromJson(const Json& j);  // missing values: the defaults
    bool operator==(const MasterSettings& o) const { return volumeDb == o.volumeDb && mute == o.mute; }
};

// names each audio clip's (or frozen track's rendering's) file in describe()
using AudioFileFn = std::function<std::string(const Clip&)>;

// The project as JSON. `audioFile(clip)` names each audio clip's file; by default (nullptr) it is the
// identity of its audio, which is enough to tell whether anything changed. states=false leaves out
// plugin settings (some plugins describe the same settings with different bytes each time, so they
// can't tell whether anything changed): main_window's snapshot() for unsaved-change detection is
// describe(project, sr, master, nullptr, false), compared with ==.
Json describe(const Project& project, int sr, const MasterSettings& master, AudioFileFn audioFile = nullptr,
              bool states = true);

// "<dir>/<stem>_audio" next to the project file
std::string audioFolder(const std::string& jsonPath);
// "<dir>/<stem>_backups" next to the project file
std::string backupsFolder(const std::string& jsonPath);

struct Backup {
    std::string path;
    double mtime;  // when that version was saved (seconds since the epoch)
};
// the backups of a project, newest first
std::vector<Backup> backups(const std::string& jsonPath);
// Before saving over the project at `path`: keep the file as it is now in its backups folder (unless the
// newest backup already holds the same), with "base" pointing back to the project's folder; the oldest
// beyond `kept` go. `backupPath`: the backup's path, or "" (no file there, or not a Tiny DAW project).
// False (and `error`) if it couldn't be kept. The main window calls it before saveProject() over a file
// (File → Save), and asks whether to save anyway when it fails.
bool keepBackup(const std::string& path, std::string& backupPath, std::string& error, int kept = BACKUPS_KEPT);

// Every clip holding audio a saved project keeps: audio clips, frozen tracks' renderings, the built-in
// samplers' samples, recorded audio takes. (Save as template: saveProject points them at the files it
// wrote; keep their sourcePath first and put it back afterwards to leave the project as it was.)
std::vector<ClipPtr> audioHolders(const Project& project);
// the same with the missing samples' clips too (Clip::missing: silent stand-ins, relink.h)
std::vector<ClipPtr> allAudioHolders(const Project& project);

// What autosave writes: audio still to write as a file, then the JSON
struct PendingAudio {
    AudioPtr audio;
    int sampleRate;
    std::string file;
};
// The project for a recovery file: the JSON, and the audio still to write (`toWrite`). Audio saved
// somewhere (in a project's audio folder, or by an earlier recovery: `written`, fingerprint -> path,
// which this adds to) is named by its absolute path; the rest gets a new file in `folder`.
Json describeRecovery(const Project& project, int sr, const MasterSettings& master, const std::string& folder,
                      std::map<std::string, std::string>& written, std::vector<PendingAudio>& toWrite);
// Write a recovery file: its new audio first, then the JSON (so the JSON never names a missing file).
// Any thread: it only reads the audio, which is never changed.
bool writeRecovery(const std::string& path, const Json& data, const std::vector<PendingAudio>& toWrite,
                   std::string& error);
// the JSON written via a temporary file (a crash mid-save never leaves a half-written file), indent 2
bool writeJson(const std::string& path, const Json& data, std::string& error);

// Write the JSON (via a temporary file, so a crash mid-save never leaves a half-written project)
// and any audio not yet in the project's audio folder; sets the clips' sourcePath.
// `progress` (if given): called on this thread as the audio files are written (done, of total).
bool saveProject(const std::string& path, Project& project, int sr, const MasterSettings& master,
                 std::string& error, const std::function<void(int done, int total)>& progress = nullptr);

// Fill `project` from the file at `path`, preparing clips for sample rate sr, and give its master
// settings. Everything is read before `project` is touched, so a broken or incomplete project
// returns false (with `error`) and leaves the current one as it was. Plugin instances of the
// replaced project are retired (Project::retire). Afterwards the GUI loads project.pluginSlots().
//
// MISSING SAMPLES. A sample file that isn't there doesn't stop it opening: its clips (and takes, samplers' and pads'
// samples) open silent, holding a Clip::missing (where it was expected, and the size and sound the project saved
// for it in "audio_files": {file: {"size": bytes, "sound": fingerprint}}), and saving names that file again until
// it is found (relink.h). A file that is there but can't be read is still an error.
//
// FASTER OPENING. Every sample file it names is decoded first, once per file (however many clips use it), on
// workers::chosenCores() threads at once (decodeFiles); the rest is read as before, so the project is the same
// whatever the number of threads. `progress` (if given) is called on this thread while that runs (done, of
// total files), every few hundredths of a second: the GUI's progress window keeps the window drawn.
bool loadProject(const std::string& path, Project& project, int sr, MasterSettings& master, std::string& error,
                 const std::function<void(int done, int total)>& progress = nullptr);

// One sound file decoded (file_io::loadAudio): its audio and rate, or why not
struct Decoded {
    AudioPtr audio;
    int rate = 0;
    std::string error;  // "" = read
};
// `paths` decoded at once on `threads` worker threads (-1: workers::chosenCores()'s; 0: this thread alone, in
// order) plus this one, which calls `progress` (done, total) every few hundredths of a second meanwhile.
// out[i] is paths[i]'s; the same whatever the threads.
std::vector<Decoded> decodeFiles(const std::vector<std::string>& paths, int threads = -1,
                                 const std::function<void(int done, int total)>& progress = nullptr);

// --- the parts (used by the above; public for tests and other modules) ---
Json describeSlot(const PluginSlot& slot, bool states = true);
SlotPtr readSlot(const Json& entry);
// [[from, to], …] naming the ends as above (cables from tracks that are gone are left out)
Json describeCables(const std::vector<Cable>& cables, const std::vector<SlotPtr>& effects,
                    const std::vector<TrackPtr>& tracks);
// cables from describeCables(); cables to effects or from tracks that aren't there (or the wrong
// way round) are left out. null (a project from before cables): the effects one after the other.
std::vector<Cable> readCables(const Json& entries, const std::vector<SlotPtr>& effects,
                              const std::vector<TrackPtr>& tracks = {});
// a Timing from a project's "timing" entry; missing or out-of-range values get the defaults
Timing readTiming(const Json& entry);
Json describeTiming(const Timing& timing);
// a fingerprint of a clip's audio: equal only for identical audio at the same rate (cached per buffer)
std::string fingerprint(const AudioPtr& audio, int sr);
// a file name for a clip's audio in `folder` that isn't taken yet (nor among the paths `taken`):
// "<clip name>.wav", "<clip name> (2).wav"…
std::string uniqueFileName(const std::string& folder, const std::string& clipName,
                           const std::set<std::string>& taken = {});
// the MIDI effects of a track as saved ([{"kind": …, "values": {…}}, …]) and read back (unknown kinds left out)
Json describeMidiEffects(const std::vector<MidiEffect>& effects);
std::vector<MidiEffect> readMidiEffects(const Json& entries);
// the scales the song's key and scale may name (midi_effects.SCALES; "" = none)
const std::vector<std::string>& scaleNames();

// --- MIDI controller links (model.h MidiLink, ui/midi_learn.h): the project's "midi_links", written only when
// there are any (older files and projects without links unchanged): [{"controller": name, "channel": c, "cc": n,
// "track": <its index> | "master", "plugin": "track" (its volume / pan) | "instrument" | ["rack", i] | <effect
// index>, "param": name}]. Links whose track or plugin is gone are left out; reading leaves out those that name
// nothing there. ---
Json describeMidiLinks(const Project& project);
std::vector<MidiLink> readMidiLinks(const Json& entries, const std::vector<TrackPtr>& tracks,
                                    const std::vector<SlotPtr>& masterEffects);

}  // namespace project_io
