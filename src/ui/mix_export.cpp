#include "ui/mix_export.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>

#include "audio/engine.h"
#include "audio/loudness.h"
#include "audio/mp3_encoder.h"
#include "audio/render.h"
#include "audio/vorbis_encoder.h"
#include "core/json.h"
#include "core/util.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/style.h"

using namespace ui;

namespace mix_export {

std::string Format::shown() const {
    std::string f = filter;
    size_t cut = f.find(" (*");
    return cut == std::string::npos ? f : f.substr(0, cut);
}

const std::vector<Format>& formats() {
    static const std::vector<Format> f = {
        {"WAV, 24-bit (*.wav)", ".wav", false, SampleFormat::Pcm24, Codec::Wav},
        {"WAV, 16-bit (*.wav)", ".wav", false, SampleFormat::Pcm16, Codec::Wav},
        {"WAV, 32-bit float (*.wav)", ".wav", false, SampleFormat::Float32, Codec::Wav},
        {"FLAC, 24-bit (*.flac)", ".flac", true, SampleFormat::Pcm24, Codec::Flac},
        {"MP3, 320 kbps (*.mp3)", ".mp3", false, SampleFormat::Pcm16, Codec::Mp3},
        {"OGG Vorbis, high quality (*.ogg)", ".ogg", false, SampleFormat::Float32, Codec::Ogg},
    };
    return f;
}

const Format& formatOf(const std::string& filter) {
    for (auto& f : formats())
        if (filter == f.filter) return f;
    return formats()[0];
}

const std::vector<Normalize>& normalizeChoices() {
    static const std::vector<Normalize> n = {
        {"Off: as it is", Normalize::Off, 0},
        {"Peaks to -0.3 dB", Normalize::Peak, 0},
        {"Loudness to -14 LUFS (streaming services)", Normalize::Loudness, -14.0},
        {"Loudness to -16 LUFS (podcasts)", Normalize::Loudness, -16.0},
        {"Loudness to -23 LUFS (broadcast, EBU R128)", Normalize::Loudness, -23.0},
    };
    return n;
}

const Normalize& normalizeOf(const std::string& text) {
    for (auto& n : normalizeChoices())
        if (text == n.text) return n;
    return normalizeChoices()[0];
}

std::string savedFormat() { return settings::get(FORMAT_KEY).toString(formats()[0].filter); }
std::string savedNormalize() { return settings::get(NORMALIZE_KEY).toString(normalizeChoices()[0].text); }
void saveChoices(const std::string& filter, const std::string& normalize) {
    settings::set(FORMAT_KEY, filter);
    settings::set(NORMALIZE_KEY, normalize);
}

std::string suggestedPath(const std::string& projectPath, const std::string& suffix, const std::string& extension) {
    std::string stem = util::stem(projectPath.empty() ? std::string("Untitled") : projectPath);
    std::string name = stem + " (" + suffix + ")" + extension;
    std::string folder = projectPath.empty() ? "" : util::dirName(projectPath);
    return folder.empty() ? name : util::joinPath(folder, name);
}

std::string withExtension(const std::string& path, const Format& f) {
    if (util::endsWith(util::lower(path), f.extension)) return path;
    std::string ext = util::extension(path);
    std::string base = util::baseName(path);
    // (os.path.splitext: a leading dot is not an extension)
    std::string cut = !ext.empty() && base.size() > ext.size() ? path.substr(0, path.size() - ext.size()) : path;
    return cut + f.extension;
}

namespace {
std::string khz(int sr) {  // "44.1", "48" (Python's :g of kHz)
    std::string s = util::fixed(sr / 1000.0, 3);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}
}  // namespace

bool writeAudio(const std::string& path, const Format& f, const std::vector<float>& stereo, int sr, std::string& error,
                std::string* note) {
    size_t frames = stereo.size() / 2;
    if (note) note->clear();
    switch (f.kind) {
    case Codec::Flac: return writeFlac(path, stereo.data(), frames, 2, sr, f.sampleFormat, error);
    case Codec::Mp3: {
        // (an MP3 at a rate it can't have is written at the nearest it can: the encoder converts it)
        int rate = mp3EncodedRate(sr);
        if (note && rate != sr) *note = "made " + khz(rate) + " kHz for the MP3 (it can't be " + khz(sr) + " kHz)";
        return encodeMp3(path, stereo.data(), frames, 2, sr, MP3_KBPS, error);
    }
    case Codec::Ogg: return encodeVorbis(path, stereo.data(), frames, 2, sr, VORBIS_QUALITY, error);
    default: return writeWav(path, stereo.data(), frames, 2, sr, f.sampleFormat, error);
    }
}

double peakOf(const std::vector<float>& stereo) {
    double peak = 0;
    for (float v : stereo) peak = std::max(peak, (double)std::fabs(v));
    return peak;
}

double shortLoudness(const std::vector<float>& stereo, int sr) {
    auto sos = loudness::kWeighting(sr);
    double sum = 0;
    size_t frames = stereo.size() / 2;
    if (!frames) return -std::numeric_limits<double>::infinity();
    for (int ch = 0; ch < 2; ++ch) {
        double z[2][2] = {{0, 0}, {0, 0}};  // (transposed direct form II, per section)
        for (size_t i = 0; i < frames; ++i) {
            double x = stereo[i * 2 + ch];
            for (int k = 0; k < 2; ++k) {
                const loudness::Biquad& b = sos[k];
                double y = b.b0 * x + z[k][0];
                z[k][0] = b.b1 * x - b.a1 * y + z[k][1];
                z[k][1] = b.b2 * x - b.a2 * y;
                x = y;
            }
            sum += x * x;
        }
    }
    return loudness::toLufs(sum / frames);  // (the channels' powers added, as the meter does)
}

double loudnessOf(const std::vector<float>& stereo, int sr) {
    loudness::LoudnessMeter meter(sr, 2);
    const size_t block = 65536;
    size_t frames = stereo.size() / 2;
    for (size_t i = 0; i < frames; i += block)
        meter.add(stereo.data() + i * 2, (int)std::min(block, frames - i));
    double m, s, integrated;
    meter.read(m, s, integrated);
    if (!std::isfinite(integrated) && frames < (size_t)(0.4 * sr))  // (shorter than one gating block: measured as one)
        integrated = shortLoudness(stereo, sr);
    return integrated;
}

std::string normalize(std::vector<float>& stereo, int sr, const Normalize& how) {
    double peak = peakOf(stereo);
    if (how.how == Normalize::Off || peak <= 0) return "";
    auto apply = [&](double gainDb) {
        double g = std::pow(10.0, gainDb / 20);
        for (float& v : stereo) v = (float)(v * g);
    };
    auto signedDb = [](double db) { return (db >= 0 ? "+" : "") + util::fixed(db, 1); };
    if (how.how == Normalize::Peak) {
        double gainDb = PEAK_DB - 20 * std::log10(peak);
        apply(gainDb);
        return "peaks to -0.3 dB (" + signedDb(gainDb) + " dB)";
    }
    double lufs = loudnessOf(stereo, sr);
    if (!std::isfinite(lufs)) return "too quiet to measure its loudness: left as it is";
    double gainDb = how.lufs - lufs;
    bool held = 20 * std::log10(peak) + gainDb > LOUDNESS_PEAK_DB;
    if (held) gainDb = LOUDNESS_PEAK_DB - 20 * std::log10(peak);  // (never louder than the peaks allow: no clipping)
    std::string text = "loudness " + util::fixed(lufs + gainDb, 1) + " LUFS (" + signedDb(gainDb) + " dB)";
    if (held)
        text += ": held back from " + util::format("%g", how.lufs) + " LUFS to keep the peaks under " +
                util::format("%g", LOUDNESS_PEAK_DB) + " dB";
    apply(gainDb);
    return text;
}

std::string stemName(const std::string& name) {
    // re.sub(r"[^\w\- ]+", "_", name).strip() or "track" (UTF-8 letters count as word characters)
    std::string out;
    bool inRun = false;
    for (unsigned char ch : name) {
        bool word = isalnum(ch) || ch == '_' || ch == '-' || ch == ' ' || ch >= 0x80;
        if (word) {
            out += (char)ch;
            inRun = false;
        } else if (!inRun) {
            out += '_';
            inRun = true;
        }
    }
    out = util::trim(out);
    return out.empty() ? std::string("track") : out;
}

}  // namespace mix_export

using namespace mix_export;

// ---------------------------------------------------------------------------------------------

RenderDialog::RenderDialog(Window* parent, const std::string& chosenFormat, const std::string& chosenNormalize)
    : Dialog(parent, "Render", 420, 200) {
    setStyleType("RenderDialog");
    FormLayout* f = styleRoom(form(this, 11, 6), computedStyle());
    setMinimumSize(std::max(0, style::px(computedStyle(), "min-width", 420)), 0);
    format = add<ComboBox>();
    for (auto& fm : formats()) format->addItem(fm.shown(), fm.filter);
    format->setCurrentIndexSilently(std::max(0, format->findData(chosenFormat)));
    f->addRow("Format", format);
    normalize = add<ComboBox>();
    for (auto& n : normalizeChoices()) normalize->addItem(n.text, n.text);
    normalize->setCurrentIndexSilently(std::max(0, normalize->findData(chosenNormalize)));
    f->addRow("Normalize", normalize);
    Label* hint = add<Label>("Normalizing makes the whole render louder or quieter (never over -1 dB for the loudness "
                             "targets). MP3 and OGG are smaller files for sharing; WAV and FLAC keep every detail.");
    hint->setWordWrap(true);
    hint->setProperty("hint", "true");
    f->addRow(hint);
    auto* bb = add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel);
    bb->button(ButtonBox::Ok)->setText("Choose the file…");
    bb->accepted.connect([this] { accept(); });
    bb->rejected.connect([this] { reject(); });
    f->addRow(bb);
    adjustSize();
}

// ---------------------------------------------------------------------------------------------


MixExporter::MixExporter(MixExportHost h, audio::Engine& engine) : host(std::move(h)), engine_(engine) {}

std::string MixExporter::renderSelection() {
    int64_t start = 0, end = 0;
    if (!host.selection || !host.selection(start, end)) {
        msg::information(host.window, "Render selection",
                         "Select a time range first: drag across the lanes where it should start and end.\n\n"
                         "Or use File → Render project… for the whole arrangement.");
        return "";
    }
    return exportMix(start, end, false, "selection");
}

std::string MixExporter::renderProject() {
    int64_t end = engine_.project.endFrame();
    if (end == 0) {
        msg::information(host.window, "Render project", "There is nothing to render yet: the project has no clips.");
        return "";
    }
    return exportMix(0, end, true, "mix");
}

bool MixExporter::askFile(const std::string& suffix, std::string& path, const Format*& format, const Normalize*& how) {
    // the format and normalizing first (remembered), then the file
    std::string filter = savedFormat(), normalizing = savedNormalize();
    if (host.askFormat) {
        if (!host.askFormat(filter, normalizing)) return false;
    } else if (!host.askFile) {
        RenderDialog dialog(host.window, filter, normalizing);
        if (dialog.exec() != Dialog::Accepted) return false;
        filter = dialog.format->currentData();
        normalizing = dialog.normalize->currentData();
    }
    format = &formatOf(filter);
    how = &normalizeOf(normalizing);
    saveChoices(format->filter, how->text);
    std::string projectPath = host.projectPath ? host.projectPath() : "";
    if (host.askFile) {
        std::string chosen;
        if (!host.askFile(path, chosen)) return false;
        if (!chosen.empty()) format = &formatOf(chosen);  // (older tests choose the format with the file)
    } else {
        path = files::saveFile(host.window, "Render to file", suggestedPath(projectPath, suffix, format->extension),
                               format->filter);
    }
    if (path.empty()) return false;
    path = withExtension(path, *format);
    return true;
}

std::string MixExporter::exportMix(int64_t start, int64_t end, bool tail, const std::string& suffix) {
    std::string path;
    const Format* format = nullptr;
    const Normalize* how = nullptr;
    lastNote.clear();
    if (!askFile(suffix, path, format, how)) return "";
    if (host.stop) host.stop();
    ProgressDialog dialog(host.window, "Rendering", "Rendering the mix…", true);
    dialog.setRange(0, 100);
    double shownFrom = App::get().now() + 0.3;  // (QProgressDialog's minimum duration: 300 ms)
    auto progress = [&](double done) {
        if (dialog.isWindowShown() || App::get().now() >= shownFrom) dialog.setValue((int)(done * 100));
        else App::get().processEvents();
        return !dialog.wasCanceled();
    };
    int sr = engine_.sr;
    std::optional<std::vector<float>> audio;
    std::string error, note;
    try {
        {
            audio::Engine::PluginsPaused paused(engine_);  // this thread calls the plugins now
            audio = render::renderMix(engine_.project, start, end, sr, host.masterDb ? host.masterDb() : 0.0, tail,
                                      progress);
        }
        if (audio) {
            note = normalize(*audio, sr, *how);
            std::string converted;
            if (!writeAudio(path, *format, *audio, sr, error, &converted)) throw std::runtime_error(error);
            if (!converted.empty()) note += (note.empty() ? "" : "; ") + converted;
        }
    } catch (const std::exception& e) {
        if (dialog.isWindowShown()) dialog.hideWindow();
        msg::warning(host.window, "Cannot render", path + "\n\n" + e.what());
        return "";
    }
    if (dialog.isWindowShown()) dialog.hideWindow();
    if (!audio) return "";  // cancelled
    lastNote = note;
    if (host.showMessage)
        host.showMessage("Rendered " + util::fixed((double)(audio->size() / 2) / sr, 1) + " s to " + path +
                             (note.empty() ? "" : "; " + note),
                         10000);
    return path;
}

std::vector<std::string> MixExporter::exportStems() {
    Project& project = engine_.project;
    int64_t end = project.endFrame();
    std::vector<TrackPtr> tracks;
    for (auto& t : project.tracks)
        if (!t->needsInstrument) tracks.push_back(t);
    if (end == 0 || tracks.empty()) {
        msg::information(host.window, "Export stems", "There is nothing to export yet: the project has no clips.");
        return {};
    }
    std::string projectPath = host.projectPath ? host.projectPath() : "";
    std::string folder = host.askFolder ? host.askFolder()
                                        : files::folder(host.window, "Folder for the stems", util::dirName(projectPath));
    if (folder.empty()) return {};
    if (host.stop) host.stop();
    ProgressDialog dialog(host.window, "Export stems", "Rendering the stems…", true);
    dialog.setRange(0, 100 * (int)tracks.size());
    double shownFrom = App::get().now() + 0.3;
    int sr = engine_.sr;
    std::vector<std::pair<TrackPtr, std::vector<float>>> rendered;
    try {
        for (size_t i = 0; i < tracks.size(); ++i) {
            auto progress = [&, i](double done) {
                if (dialog.isWindowShown() || App::get().now() >= shownFrom) dialog.setValue((int)((i + done) * 100));
                else App::get().processEvents();
                return !dialog.wasCanceled();
            };
            std::optional<std::vector<float>> audio;
            {
                audio::Engine::PluginsPaused paused(engine_);
                // one track's own sound (a stem): no master effects or volume
                audio = render::renderMix(project, 0, end, sr, 0.0, true, progress, tracks[i].get());
            }
            if (!audio) {
                if (dialog.isWindowShown()) dialog.hideWindow();
                return {};
            }
            rendered.emplace_back(tracks[i], std::move(*audio));
        }
    } catch (const std::exception& e) {
        if (dialog.isWindowShown()) dialog.hideWindow();
        msg::warning(host.window, "Cannot export stems", e.what());
        return {};
    }
    size_t length = 0;  // (all as long as the longest tail: they line up)
    for (auto& r : rendered) length = std::max(length, r.second.size() / 2);
    std::vector<std::string> paths;
    for (size_t i = 0; i < rendered.size(); ++i) {
        std::string name = stemName(rendered[i].first->name);
        std::string number = util::format("%02d", (int)i + 1);
        std::string path = util::joinPath(folder, number + " " + name + ".wav");
        for (int n = 2; util::fileExists(path); ++n)
            path = util::joinPath(folder, number + " " + name + " (" + std::to_string(n) + ").wav");
        std::vector<float>& audio = rendered[i].second;
        audio.resize(length * 2, 0.0f);
        std::string error;
        if (!writeWav(path, audio.data(), length, 2, sr, SampleFormat::Pcm24, error)) {
            if (dialog.isWindowShown()) dialog.hideWindow();
            msg::warning(host.window, "Cannot export stems", path + "\n\n" + error);
            return paths;
        }
        paths.push_back(path);
    }
    if (dialog.isWindowShown()) dialog.hideWindow();
    if (host.showMessage)
        host.showMessage("Exported " + std::to_string(paths.size()) + " stems (" + util::fixed((double)length / sr, 1) +
                             " s each) to " + folder,
                         8000);
    return paths;
}
