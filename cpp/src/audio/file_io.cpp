#include "audio/file_io.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "audio/sound_file.h"
#include "core/util.h"
#include "model/timing.h"

namespace file_io {

const std::vector<std::string> AUDIO_EXTENSIONS = {".wav", ".flac", ".ogg", ".mp3", ".aif", ".aiff"};
const std::vector<std::string> MIDI_EXTENSIONS = {".mid", ".midi"};

static bool has(const std::vector<std::string>& list, const std::string& ext) {
    return std::find(list.begin(), list.end(), ext) != list.end();
}

bool isMidi(const std::string& path) { return has(MIDI_EXTENSIONS, util::extension(path)); }

bool isSample(const std::string& path) {
    std::string ext = util::extension(path);
    return has(AUDIO_EXTENSIONS, ext) || has(MIDI_EXTENSIONS, ext);
}

bool loadAudio(const std::string& path, AudioPtr& data, int& sampleRate, std::string& error) {
    SoundData sound;
    if (!readSoundFile(path, sound, error)) return false;
    data = dsp::toStereo(sound.samples.data(), sound.frames(), sound.channels);
    sampleRate = sound.sampleRate;
    return true;
}

bool loadMidi(const std::string& path, std::vector<Note>& notes, double& lengthSeconds, std::string& error) {
    std::vector<MidiFileNote> found;
    if (!readMidiFile(path, found, lengthSeconds, error)) return false;
    notes.clear();
    notes.reserve(found.size());
    for (auto& n : found) notes.push_back(Note{n.start, n.end, (double)n.pitch, (double)n.velocity});
    return true;
}

bool loadMidiBeats(const std::string& path, std::vector<Note>& notes, double& lengthBeats, std::string& error) {
    std::vector<MidiFileNote> found;
    if (!readMidiFileBeats(path, found, lengthBeats, error)) return false;
    notes.clear();
    notes.reserve(found.size());
    for (auto& n : found) notes.push_back(Note{n.start, n.end, (double)n.pitch, (double)n.velocity});
    return true;
}

bool saveMidi(const std::string& path, const std::vector<MidiPart>& parts, const Timing& timing, double startS,
              std::string& error) {
    TempoMapPtr map = timing.map();
    double beat0 = map->beatAt(startS);
    auto tick = [&](double seconds) {
        return std::max<int64_t>(0, (int64_t)std::nearbyint((map->beatAt(startS + seconds) - beat0) * MIDI_TICKS));
    };
    std::vector<MidiOutTrack> tracks;
    MidiOutTrack tempo{"Tempo", {}};
    const std::vector<Segment>& segments = map->segments();
    for (size_t i = 0; i < segments.size(); i++) {
        const Segment& s = segments[i];
        double ends = i + 1 < segments.size() ? segments[i + 1].seconds : std::numeric_limits<double>::infinity();
        if (ends <= startS) continue;  // (over before the part written begins)
        int64_t at = s.seconds <= startS ? 0 : (int64_t)std::nearbyint((s.beat - beat0) * MIDI_TICKS);
        tempo.events.push_back(midi_out::tempo(at, s.bpm));
        tempo.events.push_back(midi_out::timeSignature(at, s.beatsPerBar, 4));
    }
    tracks.push_back(tempo);
    for (auto& part : parts) {
        MidiOutTrack track{part.name, {}};
        for (auto& n : part.notes) {
            int pitch = (int)std::min(127.0, std::max(0.0, std::nearbyint(n.pitch)));
            int velocity = (int)std::min(127.0, std::max(1.0, std::nearbyint(n.velocity)));
            int64_t on = tick(n.start), off = tick(n.end);
            track.events.push_back(midi_out::noteOn(on, pitch, velocity));
            track.events.push_back(midi_out::noteOff(std::max(off, on + 1), pitch));
        }
        tracks.push_back(track);
    }
    return writeMidiFile(path, MIDI_TICKS, tracks, error);
}

bool loadPlayable(const std::string& path, int sr, AudioPtr& out, std::string& error) {
    if (isMidi(path)) {
        std::vector<Note> notes;
        double length = 0;
        if (!loadMidi(path, notes, length, error)) return false;
        out = synth::render(notes, sr, length);
        return true;
    }
    AudioPtr data;
    int sourceSr = 0;
    if (!loadAudio(path, data, sourceSr, error)) return false;
    out = dsp::resample(data, sourceSr, sr);
    return true;
}

}  // namespace file_io
