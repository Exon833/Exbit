#include "direct_calls.h"

#include <memory>

#include "audio/workers.h"
#include "paced_buffers.h"
#include "plugins/plugin.h"
#include "speed_settings.h"

namespace audio::detail {
void raiseThreadPriority();  // (the audio thread's priority: audio/devices.cpp, or reference_thread_priority.cpp)
}

namespace plugin_speed {

namespace {

// one track's plugins, and its sound: two stereo pairs, an effect's input and output (swapped after each effect)
struct DirectTrack {
    std::unique_ptr<plug::Plugin> instrument;
    std::vector<std::unique_ptr<plug::Plugin>> effects;
    std::vector<float> sound[4];
};

// what one buffer asks of every track
struct DirectBuffer {
    std::vector<DirectTrack>* tracks;
    const std::vector<plug::NoteMessage>* notes;
    int frames;
    int64_t position;
};

// the worker pool's job: track trackIndex's instrument, then its effects one after the other
void renderDirectTrack(void* bufferPointer, int trackIndex) {
    const DirectBuffer& buffer = *static_cast<DirectBuffer*>(bufferPointer);
    DirectTrack& track = (*buffer.tracks)[trackIndex];
    float* const first[2] = {track.sound[0].data(), track.sound[1].data()};
    float* const second[2] = {track.sound[2].data(), track.sound[3].data()};
    track.instrument->setTransport(TEMPO, 4, 4, true, buffer.position);
    track.instrument->render(*buffer.notes, first, buffer.frames);
    for (size_t i = 0; i < track.effects.size(); i++) {
        track.effects[i]->setTransport(TEMPO, 4, 4, true, buffer.position);
        track.effects[i]->process(i % 2 ? second : first, nullptr, i % 2 ? first : second, buffer.frames);
    }
}

}  // namespace

std::vector<std::vector<double>> timeDirectCalls(const std::string& speedCase, const std::string& instrumentPath,
                                                 const std::string& effectPath, std::string& error) {
    audio::detail::raiseThreadPriority();  // (as the engine's audio thread)
    std::vector<DirectTrack> tracks(speedCase == "eight" ? EIGHT_TRACKS : 1);
    for (auto& track : tracks) {
        track.instrument = plug::Plugin::load(instrumentPath, "", error);
        if (!track.instrument) return {};
        track.instrument->prepare(SAMPLE_RATE, 4096);
        for (int i = 0; speedCase == "chain" && i < CHAIN_EFFECTS; i++) {
            track.effects.push_back(plug::Plugin::load(effectPath, "", error));
            if (!track.effects.back()) return {};
            track.effects.back()->prepare(SAMPLE_RATE, 4096);
        }
        for (auto& channel : track.sound) channel.assign(4096, 0.0f);
    }
    std::vector<plug::NoteMessage> chord, none;
    for (int pitch : CHORD_PITCHES) chord.push_back({0, true, pitch, CHORD_VELOCITY, 0});
    workers::Pool pool(workers::workersFor(0));  // (the engine's, as many workers as it would have)
    std::vector<std::vector<double>> timesPerSize;
    for (int frames : FRAME_SIZES) {
        for (auto& track : tracks) track.instrument->notesOff();
        DirectBuffer buffer{&tracks, &chord, frames, 0};
        timesPerSize.push_back(timePacedBuffers(frames, SECONDS_PER_RUN, [&](int index) {
            buffer.notes = index == 0 ? &chord : &none;
            buffer.position = (int64_t)index * frames;
            pool.run((int)tracks.size(), renderDirectTrack, &buffer);
        }));
    }
    return timesPerSize;
}

}  // namespace plugin_speed
