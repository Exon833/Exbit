// File → Render selection… / Render project…: the mix, rendered to an audio file (render::renderMix),
// with a progress window (mix_export.py).
//
// Render selection renders exactly the selected time range (drag across the lanes); Render project
// renders from 0 s to the end of the last clip, plus the tail while effects ring out. Both render
// every audible track, as you hear it, not only the lanes the selection covers. A small window first
// asks the format, WAV (24-bit, 16-bit or 32-bit float), FLAC (24-bit), MP3 (320 kbps: our own
// encoder, audio/mp3_encoder.h) or OGG Vorbis (audio/vorbis_encoder.h), at the engine's sample
// rate, and whether to normalize: the peaks to -0.3 dB, or the loudness (integrated, EBU R128:
// audio/loudness.h) to a target, never letting the peaks go over -1 dB (it says when that held it
// back). Both choices are remembered (the settings' "options/render_format" and
// "options/render_normalize", the successor's Preferences keys, read and written here directly).
//
// File → Export stems… renders each track to a file of its own (24-bit WAV; its sound after its
// fader and pan, with what it sends to returns left out; a return's stem is what the tracks send it,
// through its effects), all from 0 s to the same end, so they line up when put together elsewhere.
// Mute and solo, the master's effects and volume are left out.
//
// Rendering calls the plugins from the main thread, so playback stops and the engine leaves plugins
// alone meanwhile (Engine::PluginsPaused), as when freezing a track.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "audio/sound_file.h"
#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

namespace audio { class Engine; }

namespace mix_export {
enum class Codec { Wav, Flac, Mp3, Ogg };
struct Format {
    const char* filter;      // the file dialog's filter: "WAV, 24-bit (*.wav)"
    const char* extension;   // ".wav"
    bool flac;               // FLAC (kind == Codec::Flac; kept for older code)
    SampleFormat sampleFormat;  // WAV / FLAC
    Codec kind = Codec::Wav;
    std::string shown() const;  // "WAV, 24-bit" (the filter without its pattern)
};
const std::vector<Format>& formats();  // FORMATS, WAV 24-bit first
// the format chosen by its filter text (the first when unknown)
const Format& formatOf(const std::string& filter);

// NORMALIZE: (shown, how)
struct Normalize {
    enum How { Off, Peak, Loudness };
    const char* text;        // "Peaks to -0.3 dB"
    How how;
    double lufs;             // the target (Loudness)
};
const std::vector<Normalize>& normalizeChoices();
const Normalize& normalizeOf(const std::string& text);  // (the first, Off, when unknown)
constexpr double PEAK_DB = -0.3;           // where peak normalizing puts the peaks
constexpr double LOUDNESS_PEAK_DB = -1.0;  // the most loudness normalizing allows
constexpr int MP3_KBPS = 320;
constexpr double VORBIS_QUALITY = 8.0;     // oggenc -q (libsndfile's compression level 0.2)
constexpr const char* FORMAT_KEY = "options/render_format";
constexpr const char* NORMALIZE_KEY = "options/render_normalize";
// the choices remembered (the settings, read fresh): a format's filter, a Normalize text
std::string savedFormat();
std::string savedNormalize();
void saveChoices(const std::string& filter, const std::string& normalize);

// "/songs/Song (mix).wav" for /songs/Song.json; "Untitled (mix).wav" when not saved (extension: the format's)
std::string suggestedPath(const std::string& projectPath, const std::string& suffix, const std::string& extension = ".wav");
// path with the format's extension (replacing another one)
std::string withExtension(const std::string& path, const Format& f);
// writes stereo interleaved audio in that format; false and `error` if it can't. `note`: what was
// changed on the way ("made 48 kHz for the MP3 (it can't be 96 kHz)"), or "".
bool writeAudio(const std::string& path, const Format& f, const std::vector<float>& stereo, int sr, std::string& error,
                std::string* note = nullptr);
// the loudness (LUFS) of audio shorter than a gating block (400 ms): its K-weighted mean power as one block
double shortLoudness(const std::vector<float>& stereo, int sr);
// the integrated loudness of stereo audio (LUFS; -inf when silent), measured as normalizing does
double loudnessOf(const std::vector<float>& stereo, int sr);
double peakOf(const std::vector<float>& stereo);  // largest |sample|
// make `stereo` louder or quieter as `how` says; returns what was done ("peaks to -0.3 dB (+2.1 dB)"), or ""
std::string normalize(std::vector<float>& stereo, int sr, const Normalize& how);
// a track's name as a file name: runs of characters other than letters, digits, _ - and space become "_"
std::string stemName(const std::string& name);
}  // namespace mix_export

// The format and normalizing, before the file is chosen (remembered).
class RenderDialog : public ui::Dialog {
public:
    RenderDialog(ui::Window* parent, const std::string& format, const std::string& normalize);
    ui::ComboBox* format;      // data: the format's filter
    ui::ComboBox* normalize;   // data: the Normalize text
};

// What MixExporter needs from the main window and the timeline.
struct MixExportHost {
    ui::Window* window = nullptr;
    std::function<void()> stop;                                 // MainWindow::stop
    std::function<void(const std::string&, int)> showMessage;   // statusBar().showMessage(text, ms)
    std::function<std::string()> projectPath;                   // "" while not saved
    std::function<double()> masterDb;                           // master.volume.value()
    std::function<bool(int64_t& start, int64_t& end)> selection;  // timeline.selection.range; false: none
    // (tests) the render window answered without showing it: a format's filter and a Normalize text
    std::function<bool(std::string& filter, std::string& normalize)> askFormat;
    // (tests) the file chosen instead of asking: path (and, from older tests, the format's filter text)
    std::function<bool(std::string& path, std::string& filter)> askFile;
    // (tests) the stems' folder chosen instead of asking
    std::function<std::string()> askFolder;
};

class MixExporter {
public:
    MixExporter(MixExportHost host, audio::Engine& engine);
    std::string renderSelection();  // the path written, "" if none
    std::string renderProject();
    // render frames start…end of the mix and write it to a file the user picks; returns its path
    std::string exportMix(int64_t start, int64_t end, bool tail, const std::string& suffix);
    // File → Export stems…: each track to its own 24-bit WAV in a folder the user picks; returns the paths
    std::vector<std::string> exportStems();
    std::string lastNote;  // what the last render's normalizing / converting did ("" if nothing)
    MixExportHost host;
private:
    audio::Engine& engine_;
    bool askFile(const std::string& suffix, std::string& path, const mix_export::Format*& format,
                 const mix_export::Normalize*& how);
};
