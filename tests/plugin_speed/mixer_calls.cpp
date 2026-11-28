#include "mixer_calls.h"

#include "audio/mixer.h"
#include "audio/patch.h"
#include "audio/workers.h"
#include "model/model.h"
#include "benchmark_audio_thread.h"
#include "paced_buffers.h"
#include "plugins/plugin.h"
#include "speed_settings.h"

namespace plugin_speed {

namespace {

// a plugin file loaded into a slot of this kind ("instrument", "effect"), ready to play; null, with `error`, if it can't be
SlotPtr loadedSlot(const std::string& path, const char* kind, std::string& error) {
    auto plugin = plug::Plugin::load(path, "", error);
    if (!plugin) return nullptr;
    plugin->prepare(SAMPLE_RATE, 4096);
    auto slot = std::make_shared<PluginSlot>();
    slot->path = path;
    slot->name = plugin->name();
    slot->kind = kind;
    slot->instance = std::shared_ptr<plug::Plugin>(plugin.release());
    return slot;
}

}  // namespace

bool buildSpeedProject(Project& project, const std::string& speedCase, const std::string& instrumentPath,
                       const std::string& effectPath, std::string& error) {
    // (longer than a run, each played from the start; no longer: a clip renders its sound, held in memory, to show it)
    const double heldSeconds = SECONDS_PER_RUN + 1;
    std::vector<Note> chord;
    for (int pitch : CHORD_PITCHES) chord.push_back({0.0, heldSeconds, (double)pitch, (double)CHORD_VELOCITY});
    for (int i = 0; i < (speedCase == "eight" ? EIGHT_TRACKS : 1); i++) {
        auto track = project.addTrack(Kind::Midi);
        track->needsInstrument = false;
        track->instrument = loadedSlot(instrumentPath, "instrument", error);
        if (!track->instrument) return false;
        std::vector<SlotPtr> effects;
        for (int e = 0; speedCase == "chain" && e < CHAIN_EFFECTS; e++) {
            effects.push_back(loadedSlot(effectPath, "effect", error));
            if (!effects.back()) return false;
        }
        track->effects = effects;
        track->cables = chain(effects);
        auto clip = makeMidiClip("chord", chord, heldSeconds, SAMPLE_RATE);
        clip->track = track;
        clip->prepare(SAMPLE_RATE);
        project.addClip(clip);
    }
    patch::ensureStates(project);
    return true;
}

std::vector<std::vector<double>> timeMixerCalls(Project& project) {
    std::vector<std::vector<double>> timesPerSize;
    runOnAudioThread([&](workers::Pool& pool) {
        mixer::MixState state;
        state.pool = &pool;
        std::vector<float> output(4096 * 2);
        for (int frames : FRAME_SIZES)
            timesPerSize.push_back(timePacedBuffers(frames, SECONDS_PER_RUN, [&](int index) {
                // (from the start each run: the instruments get all notes off, then the chord)
                std::fill(output.begin(), output.end(), 0.0f);
                mixer::mixClips(project, output.data(), 2, frames, true, (int64_t)index * frames, state, nullptr,
                                SAMPLE_RATE, index == 0);
            }));
    });
    return timesPerSize;
}

}  // namespace plugin_speed
