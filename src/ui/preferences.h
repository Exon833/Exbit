// Options from the Options menu, remembered between runs (preferences.py). Most of them trade
// detail for lower CPU use; see MainWindow::buildOptionsMenu(). Stored in the settings as
// "options/<name>", as the Python program's QSettings keys.
#pragma once
#include <map>
#include <string>

#include "core/json.h"
#include <vector>

namespace prefs {
// frames per audio buffer; 0 = the driver's own setting
const std::vector<int>& bufferSizes();
// screen updates per second for the playhead and meters
const std::vector<int>& refreshRates();
// % (Options → Interface size; main() applies it at the start)
const std::vector<int>& interfaceSizes();
// the interface size chosen (%, one of interfaceSizes()), read before the program's window exists
int interfaceScale();
}  // namespace prefs

struct Preferences {
    std::string driverType = "PulseAudio";  // audio/devices.h driverTypes(); a missing type falls back to another
    std::string deviceName;                 // output device to open; "" or a missing one = the type's default
    bool wasapiExclusive = false;           // (kept for the settings' sake: "exclusive" where a driver has it)
    int bufferSize = 512;
    bool levelMeters = true;
    bool loudnessMeters = true;
    int refreshFps = 60;
    bool metronome = false;
    std::string skin = "Dark";
    int overviewH = 44;
    bool randomTrackColors = false;
    bool clipsFollowTrack = false;
    bool roundedCorners = true;
    std::string recordInput = "1";          // "1" (mono) or "1/2" (a pair)
    int countInBars = 0;
    std::string vst2Folders;                // separated by "|"
    std::string winePrefixes;               // the Wine prefixes Windows plugins are found in, separated by "|"
                                            // (not set yet: the default prefix, ~/.wine, if there is one)
    std::string winePrograms;               // …the wine program chosen for some of them: JSON {"prefix": "program"}
                                            // (the others: automatic, wine::winePathFor)
    std::string midiInput;                  // the MIDI keyboard played into the selected MIDI track; "" = none
    bool computerKeyboard = false;          // the computer keyboard plays the selected MIDI track
    int keyboardOctave = 4;                 // …its A key plays C of this octave (C4 = MIDI note 60)
    int keyboardVelocity = 100;             // …at this velocity (1…127)
    std::string recentProjects;             // the last projects opened or saved, newest first (a JSON list; templates.h)
    std::string defaultTemplate;            // the template new projects start from; "" = an empty project
    std::string renderFormat = "WAV, 24-bit (*.wav)";  // File → Render…: the format chosen last (mix_export)
    std::string renderNormalize = "Off: as it is";     // …and the normalizing
    std::string favouritePlugins;           // the effect chooser's favourites, "path|plugin name" a line
    std::string recentPlugins;              // …and the plugins chosen last, newest first
    std::string recordOffsets;              // Options → Recording offset: JSON {"driver|device": ms}
    int interfaceScale = 100;               // % the whole interface is drawn at (from the next start: main.cpp)
    bool nativeWayland = true;              // (the Python program's Qt-on-Wayland choice; kept for its settings)
    std::string ghostNotes = "all";         // the MIDI editor shows other clips' notes: "all" tracks, this "track", or "off"

    std::vector<std::string> vst2List() const;
    void setVst2List(const std::vector<std::string>& folders);
    std::vector<std::string> wineList() const;
    void setWineList(const std::vector<std::string>& prefixes);
    std::map<std::string, std::string> wineProgramMap() const;
    void setWineProgramMap(const std::map<std::string, std::string>& programs);
    static Preferences load();
    void save() const;  // (writes only the values changed since load() or the last save())

private:
    mutable std::map<std::string, Json> known_;  // key (without "options/") -> the value in the settings
};
