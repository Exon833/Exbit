// The song's key and scale, and its swing, as the menus offer them (the transport's Key ▾ and Swing ▾, a MIDI
// clip's Scale ▸, a MIDI track's Swing ▸, a Steps row's swing in the MIDI editor). The menus are the
// window's and the terminal's alike (the terminal draws the same menus).
//
// KEY. The song has one key and scale (Timing::key / scale: the MIDI editor shades the rows outside it,
// snaps notes to it, folds to it, fits notes to it). A MIDI clip may have its own (Clip::key / scale), which
// its editor then uses instead; "The song's" gives it back the song's. "Detect" ranks the keys of the notes
// (key_detect.h) and offers the best few: from the selected MIDI clips (none selected: every MIDI clip), or
// from the clip's own notes.
//
// SWING (midi_effects.h SWING). Swing ▾ sets the song's amount and whether the 8ths or the 16ths swing; a
// track's Swing ▸ gives it an amount of its own (or the song's again); a Steps row's menu gives one pitch
// an amount of its own, kept in the track's Groove effect (one is added, straight, when it has none).
// Notes are swung as they are prepared, so every MIDI clip they touch is prepared again at once, on every
// processor core (reprepare). "Commit swing" writes the swing into the notes (what plays stays the same)
// and makes it straight again: one undo step.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "model/model.h"
#include "ui/toolkit/menu.h"

namespace song_scale {

// the amounts the swing menus offer (%), and how one is shown ("straight", "16 %")
const std::vector<double>& swingChoices();
std::string swingText(double amount);
// "C major", "A minor", "D Dorian"; "no scale"
std::string scaleName(int key, const std::string& scale);
// the key and scale a clip's editor uses: its own, or the song's
int keyOf(const Project& p, const Clip& c);
std::string scaleOf(const Project& p, const Clip& c);

// every MIDI clip on `tracks` (empty: on every track) prepared again at `sr`, the clips spread over every
// processor core; returns how many
int reprepare(Project& p, int sr, const std::vector<TrackPtr>& tracks = {});
// The swing written into the notes of every MIDI clip on `tracks` (empty: the whole song), which then
// play straight: the song's swing 0 % with every track back to the song's, or (some tracks) those tracks'
// own swing 0 %; their rows' own amounts go too. Returns the clips changed.
int commitSwing(Project& p, int sr, const std::vector<TrackPtr>& tracks = {});
// called after commitSwing changed notes (the main window: its open MIDI editors take them again)
extern std::function<void()> notesCommitted;
// set the song's swing (amount < 0: unchanged) and grid (< 0: unchanged), the clips prepared again
void setSongSwing(Project& p, int sr, double amount, int grid = -1);
// a track's own swing (< 0: the song's)
void setTrackSwing(Project& p, int sr, const TrackPtr& t, double amount);
// a row's own swing on a MIDI track (< 0: none), kept in its first Groove effect (added if it has none)
void setRowSwing(Project& p, int sr, const TrackPtr& t, int pitch, double amount);

// --- the menus (`changed`: after a choice, so the caller shows it) ---
// Key ▾: the key, the scale, and "Detect from <from>" (candidates from `notes`)
void fillKeyMenu(ui::Menu& menu, Project& p, const std::vector<Note>& notes, const std::string& from,
                 const std::function<void()>& changed);
// Swing ▾: the song's amount, 8ths or 16ths, Commit swing
void fillSwingMenu(ui::Menu& menu, Project& p, int sr, const std::function<void()>& changed);
// a MIDI clip's Scale ▸ (the clip menu): the song's, its own key and scale, detected from its notes
void addClipScaleMenu(ui::Menu& menu, Project& p, const std::vector<ClipPtr>& midiClips,
                      const std::function<void()>& changed = nullptr);
// a MIDI track's Swing ▸ (its row's menu): the song's, its own amount, Commit its swing
void addTrackSwingMenu(ui::Menu& menu, Project& p, int sr, const TrackPtr& t,
                       const std::function<void()>& changed = nullptr);
// a Steps row's swing: the track's, or an amount of its own
void addRowSwingMenu(ui::Menu& menu, Project& p, int sr, const TrackPtr& t, int pitch,
                     const std::function<void()>& changed = nullptr);

}  // namespace song_scale
