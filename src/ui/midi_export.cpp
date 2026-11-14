// Export MIDI clips as a MIDI file (midi_export.py).
#include "ui/midi_export.h"

#include <algorithm>
#include <cctype>

#include "core/util.h"
#include "ui/toolkit/dialogs.h"

namespace midi_export {

Parts partsOf(const std::vector<ClipPtr>& clips, int sr, const Timing& timing) {
    Parts out;
    if (clips.empty()) return out;
    int64_t earliest = clips[0]->start;
    for (const ClipPtr& c : clips) earliest = std::min(earliest, c->start);
    out.start = timing.barSeconds(timing.barAt((double)earliest / sr));
    std::vector<ClipPtr> byStart = clips;
    std::stable_sort(byStart.begin(), byStart.end(), [](const ClipPtr& a, const ClipPtr& b) { return a->start < b->start; });
    std::vector<std::pair<const Track*, std::string>> keys;  // (the track, its name): one part each
    for (const ClipPtr& clip : byStart) {
        std::vector<Note> notes = clip->heardNotes(sr);
        double shift = (double)clip->start / sr - out.start;
        for (Note& n : notes) {
            n.start += shift;
            n.end += shift;
        }
        TrackPtr track = std::atomic_load(&clip->track);
        std::pair<const Track*, std::string> key(track.get(), track ? track->name : clip->name);
        auto it = std::find(keys.begin(), keys.end(), key);
        if (it == keys.end()) {
            keys.push_back(key);
            out.parts.push_back({key.second, {}});
            it = keys.end() - 1;
        }
        auto& part = out.parts[it - keys.begin()].notes;
        part.insert(part.end(), notes.begin(), notes.end());
    }
    return out;
}

std::string fileName(const std::vector<ClipPtr>& clips, const std::string& projectPath) {
    std::string stem = clips.size() == 1 ? clips[0]->name
                                         : (!projectPath.empty() ? util::stem(projectPath) : std::string("MIDI clips"));
    size_t dot = stem.rfind('.');  // (os.path.splitext)
    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
    std::string clean;  // re.sub(r"[^\w\- ]+", "_", …): a run of other characters becomes one "_"
    bool inRun = false;
    for (unsigned char c : stem) {
        bool word = std::isalnum(c) || c == '_' || c == '-' || c == ' ' || c >= 0x80;
        if (word) {
            clean += (char)c;
            inRun = false;
        } else if (!inRun) {
            clean += '_';
            inRun = true;
        }
    }
    size_t a = clean.find_first_not_of(" \t\n\r"), b = clean.find_last_not_of(" \t\n\r");
    clean = a == std::string::npos ? "" : clean.substr(a, b - a + 1);
    return (clean.empty() ? "MIDI" : clean) + ".mid";
}

bool writeFile(const std::string& path, const std::vector<ClipPtr>& clips, int sr, const Timing& timing,
               std::string& error, size_t* notes) {
    Parts parts = partsOf(clips, sr, timing);
    if (notes) {
        *notes = 0;
        for (auto& p : parts.parts) *notes += p.notes.size();
    }
    return file_io::saveMidi(path, parts.parts, timing, parts.start, error);
}

std::string exportMidi(ui::Window* window, const std::vector<ClipPtr>& clips_, const std::string& projectPath,
                       const Project& project, int sr, std::function<void(const std::string&, int)> status) {
    std::vector<ClipPtr> clips;
    for (const ClipPtr& c : clips_)
        if (c->kind == Kind::Midi) clips.push_back(c);
    if (clips.empty()) {
        ui::msg::information(window, "Export as MIDI file",
                             "Select the MIDI clips to export first (click one; Ctrl+click for more).");
        return "";
    }
    std::string folder = projectPath.empty() ? "" : util::dirName(projectPath);
    std::string suggested = folder.empty() ? fileName(clips, projectPath) : folder + "/" + fileName(clips, projectPath);
    std::string path = ui::files::saveFile(window, "Export as MIDI file", suggested, "MIDI file (*.mid)");
    if (path.empty()) return "";
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    auto ends = [&](const std::string& e) { return lower.size() >= e.size() && lower.compare(lower.size() - e.size(), e.size(), e) == 0; };
    if (!ends(".mid") && !ends(".midi")) path += ".mid";
    std::string error;
    size_t notes = 0;
    if (!writeFile(path, clips, sr, project.timing, error, &notes)) {
        ui::msg::warning(window, "Cannot export", path + "\n\n" + error);
        return "";
    }
    if (status) status("Exported " + std::to_string(notes) + " note" + (notes != 1 ? "s" : "") + " to " + path, 6000);
    return path;
}

}  // namespace midi_export
