#include "audio/engine.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <thread>

#include "audio/midi_effects.h"
#include "audio/patch.h"
#include "audio/sampler.h"
#include "config.h"
#include "core/util.h"

namespace audio {

namespace {
double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

// --- Recorder ---

Recorder::Recorder(std::vector<int> ch, double ioLatencyS, int sr, double ringSeconds, double offset, const Track* t)
    : channels(std::move(ch)), track(t), ioFrames((int64_t)std::nearbyint(ioLatencyS * sr)), offsetMs(offset),
      offsetFrames((int64_t)std::nearbyint(offset / 1000 * sr)), late(ioFrames),
      ring_((size_t)std::max(1.0, ringSeconds * sr) * std::max<size_t>(1, channels.size())) {}

void Recorder::begin(int64_t s, int latency) {
    if (start.load() == NONE) {
        late = ioFrames + latency;
        start = s;
    }
}

void Recorder::take(const float* in, int inChannels, int frames) {
    size_t w = channels.size();
    if (!in || !w) return;
    uint64_t head = head_.load(std::memory_order_relaxed);
    uint64_t tail = tail_.load(std::memory_order_acquire);
    size_t room = ring_.size() - (size_t)(head - tail);
    int n = (int)std::min<size_t>(frames, room / w);
    if (n < frames) dropped.fetch_add(frames - n, std::memory_order_relaxed);
    size_t size = ring_.size();
    for (int i = 0; i < n; i++)
        for (size_t c = 0; c < w; c++) {
            int ch = channels[c];
            ring_[(head++) % size] = ch >= 0 && ch < inChannels ? in[(size_t)i * inChannels + ch] : 0.0f;
        }
    head_.store(head, std::memory_order_release);
}

void Recorder::collect() {
    uint64_t head = head_.load(std::memory_order_acquire);
    uint64_t tail = tail_.load(std::memory_order_relaxed);
    size_t size = ring_.size();
    while (tail < head) {
        size_t inBlock = size_ % BLOCK;
        if (inBlock == 0 && size_ / BLOCK == blocks_.size()) blocks_.emplace_back(new float[BLOCK]);
        float* block = blocks_[size_ / BLOCK].get();
        size_t n = (size_t)std::min<uint64_t>(head - tail, BLOCK - inBlock);
        for (size_t i = 0; i < n; i++) block[inBlock + i] = ring_[(tail + i) % size];
        tail += n;
        size_ += n;
    }
    tail_.store(tail, std::memory_order_release);
}

int64_t Recorder::timelineStart() const {
    int64_t s = start.load();
    return (s == NONE ? 0 : s) - late.load() - offsetFrames;  // (the device's own error: record_offset.h)
}

int64_t Recorder::audio(std::vector<float>& out) {
    collect();
    out.resize(size_);
    for (size_t i = 0; i < size_; i++) out[i] = at(i);
    return timelineStart();
}

void Recorder::stereo(int64_t from, int64_t to, std::vector<float>& out) const {
    size_t w = channels.size();
    from = std::max<int64_t>(0, from);
    to = std::min(to, frames());
    out.assign((size_t)std::max<int64_t>(0, to - from) * 2, 0.0f);
    for (int64_t f = from; f < to; f++) {
        size_t i = (size_t)f * w;
        float* o = out.data() + (size_t)(f - from) * 2;
        o[0] = at(i);
        o[1] = w > 1 ? at(i + 1) : o[0];  // a mono input plays on both sides
    }
}

size_t Recorder::memoryBytes() const { return (ring_.capacity() + blocks_.size() * BLOCK) * sizeof(float); }

// --- MIDI takes and takes ---

void MidiTake::event(bool on, int pitch, int velocity, double heard) {
    double seconds = std::max((double)start, heard >= 0 ? heard : (double)start) / sr;
    if (on && velocity > 0) {
        held_[pitch] = {seconds, velocity};
    } else {
        auto it = held_.find(pitch);
        if (it == held_.end()) return;
        double t = it->second.first;
        notes_.push_back({t, std::max(seconds, t + 0.01), (double)pitch, (double)it->second.second});
        held_.erase(it);
    }
}

std::vector<Note> MidiTake::finish(double endHeard, double& lengthS) {
    double origin = (double)start / sr;
    double end = std::max(origin, (endHeard >= 0 ? endHeard : (double)start) / sr);
    std::vector<Note> all = notes_;
    for (auto& h : held_) all.push_back({h.second.first, std::max(end, h.second.first + 0.01), (double)h.first,
                                         (double)h.second.second});
    held_.clear();
    notes_.clear();
    for (auto& n : all) n.start -= origin, n.end -= origin;
    std::sort(all.begin(), all.end(), [](const Note& a, const Note& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.end != b.end) return a.end < b.end;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.velocity < b.velocity;
    });
    lengthS = end - origin;
    for (auto& n : all) lengthS = std::max(lengthS, n.end);
    return all;
}

bool keepTake(Track& track, const ClipPtr& clip) {
    ClipPtr copy = copyClip(clip);
    copy->track = nullptr;  // (a back pointer would be a shared_ptr cycle)
    track.takes.append(copy);
    if (track.takes.size() > 1 && !track.showTakes) {
        track.showTakes = true;
        return true;
    }
    return false;
}

// --- Engine ---

class Engine::Tap : public mixer::TrackTap {
public:
    Metering* m = nullptr;
    bool metered = false;   // (meters on: else only the tracks' work is taken)
    TrackCpu* cpu = nullptr;
    void silent(const Track* t, int frames) override {
        if (metered) m->tapSilence(t, frames);
    }
    void sound(const Track* t, const float* const ch[2], int channels, int first, int frames) override {
        if (metered) m->tap(t, ch, channels, first, frames);
    }
    void work(const Track* t, double seconds) override { cpu->add(t, seconds); }
};

Engine::Engine(Project& p) : project(p), sr(config::FALLBACK_SAMPLE_RATE), tap_(new Tap) {
    metering = std::make_shared<Metering>(sr, channels);
    parts_.reserve(64);
    midiParts_.reserve(64);
    beats_.reserve(256);
    sampler::install();                   // (MIDI clips on a sampler track render through it)
    midi_effects::install(&project.timing);  // (and through their track's MIDI effects, on this project's beats)
    project.timing.publish();
    live_ = std::make_shared<const live::LiveMap>();
    mix_.reserve(4096, 64);
}

Engine::~Engine() { close(); }

void Engine::keep(std::shared_ptr<const void> p) {
    if (p) kept_.emplace_back(std::chrono::steady_clock::now(), std::move(p));
}

void Engine::idle() {
    auto limit = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    kept_.erase(std::remove_if(kept_.begin(), kept_.end(), [&](const auto& k) { return k.first < limit; }), kept_.end());
    project.timing.publish();  // (the audio thread hears a change of tempo at once)
    outLatency_ = outputLatency();  // (for MIDI out, read by the audio thread: never asked of the driver there)
    if (auto set = recorders())
        for (auto& rec : set->recorders) rec->collect();
    if (auto map = std::atomic_load(&live_))
        for (auto& kv : *map) kv.second->collect();
    // dropouts the audio thread counted (it can't print): at most a line a second
    int late = cpu.late.load();
    if (late < toldLate_) toldLate_ = 0;  // (counted afresh: playback started again)
    if (late > toldLate_ && now() - toldLateAt_ >= 1) {
        util::debugf("audio: %d dropout(s) (%d since playback started)\n", late - toldLate_, late);
        toldLate_ = late;
        toldLateAt_ = now();
    }
}

void Engine::setLoop(int64_t start, int64_t end) {
    auto old = std::atomic_load(&loop_);
    std::atomic_store(&loop_, std::shared_ptr<const LoopRange>(std::make_shared<const LoopRange>(LoopRange{start, end})));
    keep(old);
}

void Engine::clearLoop() {
    keep(std::atomic_load(&loop_));
    std::atomic_store(&loop_, std::shared_ptr<const LoopRange>());
}

std::optional<LoopRange> Engine::loop() const {
    auto l = std::atomic_load(&loop_);
    if (!l) return std::nullopt;
    return *l;
}

bool Engine::open(const std::string& type, const std::string& device, int bs, bool excl, std::string& error) {
    close();
    if (bs >= 0) blocksize = bs;
    DeviceInfo info;
    if (!deviceInfo(type, device, info)) {
        error = "No such audio device: " + device;
        return false;
    }
    StreamConfig cfg;
    cfg.driverType = type;
    cfg.outputDevice = device;
    cfg.sampleRate = info.defaultSampleRate;
    cfg.outChannels = 0;  // all its outputs
    cfg.blocksize = blocksize;
    cfg.exclusive = excl;
    // ALSA: its inputs come with the outputs (for recording), as the Python program does with ASIO
    bool duplex = false;
    if (type == "ALSA") {
        DeviceInfo in;
        if (deviceInfo(type, device, in, true) && in.maxInputs > 0) {
            cfg.inputDevice = device;
            cfg.inChannels = in.maxInputs;
            duplex = true;
        }
    }
    auto cb = [this](const float* in, float* out, int frames) { callback(in, out, frames); };
    std::string err;
    std::unique_ptr<Stream> stream = Stream::open(cfg, cb, err);
    if (!stream && duplex) {  // (the driver won't open its inputs: play without them)
        cfg.inputDevice.clear();
        cfg.inChannels = 0;
        stream = Stream::open(cfg, cb, err);
    }
    if (!stream) {
        error = err.empty() ? "The audio device can't be opened" : err;
        util::debugf("audio: %s \"%s\" can't be opened: %s\n", type.c_str(), device.c_str(), error.c_str());
        return false;
    }
    int newSr = stream->sampleRate() > 0 ? stream->sampleRate() : info.defaultSampleRate;
    askedLatency_ = blocksize > 0 ? (double)blocksize / newSr : 0.0;
    if (newSr != sr) {
        pos = pos.load() * newSr / sr;
        project.changeSampleRate(sr, newSr);
        sr = newSr;
        setMetronome(metronome_);  // its clicks at the new rate
    }
    channels = std::max(1, stream->outChannels());
    inputs = stream->inChannels();
    bool levels = metering->levelsOn, loud = metering->loudnessOn;
    metering = std::make_shared<Metering>(sr, channels);
    metering->levelsOn = levels, metering->loudnessOn = loud;
    driverType = type;
    deviceId = device;
    exclusive = excl;
    int block = std::max(stream->blocksize(), blocksize);
    patch::setBlockHint(std::max(block, 1024));
    mix_.pool = nullptr;  // (no stream runs here: close() stopped it)
    pool_ = std::make_unique<workers::Pool>(workers::workersFor(cores));
    mix_.pool = pool_.get();
    mix_.reserve(std::max(block, 1024), (int)project.tracks.size() + 16);
    patch::ensureStates(project);
    cpu.reset();
    clockReset_ = true;
    lastUnderflows_ = 0;
    stream_ = std::move(stream);
    if (!stream_->start()) {
        stream_.reset();
        error = "The audio device can't be started";
        util::debugf("audio: %s \"%s\" can't be started\n", type.c_str(), device.c_str());
        return false;
    }
    util::debugf("audio: %s \"%s\" (%s) open: %d Hz, %d outputs, %d inputs, %d frames a block (asked %d), %.1f ms out\n",
                 type.c_str(), device.c_str(), stream_->deviceName().c_str(), sr, channels, inputs, stream_->blocksize(),
                 blocksize, outputLatency() * 1000);
    return true;
}

void Engine::close() {
    stop();
    monitoring_ = false;
    closeInput();
    if (stream_) {
        stream_->stop();
        stream_.reset();
    }
    mix_.pool = nullptr;  // (the stream is stopped: nothing mixes now)
    pool_.reset();
}

std::string Engine::deviceName() const { return stream_ ? stream_->deviceName() : ""; }

double Engine::outputLatency() const {
    return stream_ ? plausibleLatency(stream_->outputLatency(), askedLatency_) : 0.0;
}

int Engine::inputCount() const {
    if (inputs) return inputs;
    if (driverType.empty()) return 0;
    std::string dev = defaultInput(driverType);
    DeviceInfo info;
    return !dev.empty() && deviceInfo(driverType, dev, info, true) ? info.maxInputs : 0;
}

std::shared_ptr<Recorder> Engine::recorder() const {
    auto set = recorders();
    return set && !set->recorders.empty() ? set->recorders.front() : nullptr;
}

void Engine::setRecorders(std::shared_ptr<const RecordSet> set) {
    keep(std::atomic_load(&recorders_));  // (the audio thread may still hold it for this buffer)
    std::atomic_store(&recorders_, std::move(set));
}

bool Engine::openInput(int want, std::string& error) {
    // the default input of a driver without a duplex stream (PulseAudio, JACK: a second client), with all its
    // channels, for recording and monitoring; its buffers go to the takes and, while monitoring, to the ring
    if (inputStream_ && inputStreamChannels_ >= want) return true;
    closeInput();
    std::string dev = defaultInput(driverType);
    if (dev.empty()) {
        error = driverLabel(driverType) + " has no input device";
        return false;
    }
    StreamConfig cfg;
    cfg.driverType = driverType;
    cfg.inputDevice = dev;
    cfg.sampleRate = sr;
    cfg.inChannels = std::max({1, want, inputCount()});
    cfg.blocksize = blocksize;
    std::string err;
    auto in = Stream::open(cfg, [this](const float* i, float*, int frames) { inputCallback(i, frames); }, err);
    if (!in) {
        error = err.empty() ? "The input device can't be opened" : err;
        util::debugf("audio: the input \"%s\" can't be opened: %s\n", dev.c_str(), error.c_str());
        return false;
    }
    inputStreamChannels_ = in->inChannels() > 0 ? in->inChannels() : cfg.inChannels;
    auto ring = std::make_shared<InputRing>();  // (half a second: what monitoring may lag at most)
    ring->channels = inputStreamChannels_;
    ring->data.assign((size_t)std::max(sr / 2, 8192) * (size_t)ring->channels, 0.0f);
    ring->scratch.assign((size_t)8192 * (size_t)ring->channels, 0.0f);
    std::atomic_store(&inputRing_, ring);
    inputStream_ = std::move(in);
    if (!inputStream_->start()) {
        inputStream_.reset();
        error = "The input device can't be started";
        return false;
    }
    util::debugf("audio: the input \"%s\" open for recording and monitoring: %d channels\n", dev.c_str(),
                 inputStreamChannels_);
    return true;
}

void Engine::closeInput() {
    if (inputStream_) {
        inputStream_->stop();
        inputStream_.reset();
        util::debugf("audio: the separate input closed\n");
    }
    keep(std::atomic_load(&inputRing_));
    std::atomic_store(&inputRing_, std::shared_ptr<InputRing>());
}

bool Engine::setMonitoring(bool on, std::string& error) {
    if (on == monitoring_.load() && (!on || inputs || inputStream_ || offline)) return true;
    if (!on) {
        monitoring_ = false;
        if (!recordingAudio()) closeInput();
        util::debugf("audio: monitoring off\n");
        return true;
    }
    if (!stream_ && !offline) {
        error = "No audio device is open (Options → Audio properties).";
        return false;
    }
    if (!inputs && !offline && !openInput(1, error)) return false;
    monitoring_ = true;
    util::debugf("audio: monitoring on (%s)\n", inputs ? "the duplex stream's inputs" : "the separate input");
    return true;
}

bool Engine::startRecording(const std::vector<int>& chans, int64_t count, AudioPtr clicks, std::string& error) {
    return startRecording(std::vector<RecordInput>{RecordInput{nullptr, chans}}, count, clicks, error);
}

bool Engine::startRecording(const std::vector<RecordInput>& takes, int64_t count, AudioPtr clicks, std::string& error) {
    if ((!stream_ && !offline) || playing || takes.empty()) {
        error = "The audio device isn't ready";
        return false;
    }
    int want = 1;
    for (auto& t : takes)
        for (int c : t.channels) want = std::max(want, c + 1);
    double io = 0;
    if (!prepareInput(want, io, error)) return false;
    auto set = std::make_shared<RecordSet>();
    for (auto& t : takes) {
        set->recorders.push_back(std::make_shared<Recorder>(t.channels, io, sr, 20.0, recordOffsetMs, t.track));
        set->tracks.push_back(t.track);
    }
    setRecorders(set);
    util::debugf("audio: recording %zu take(s) at once, %.1f ms in and out\n", takes.size(), io * 1000);
    if (clicks && count) preview(clicks, 0, false, 1.0);  // (as loud as the metronome)
    countIn = count;
    play();
    return true;
}

bool Engine::prepareInput(int want, double& io, std::string& error) {
    // the inputs the takes come from, and how late they arrive after what was heard (input and output delay)
    if (!stream_) {
        io = 0;  // (offline: the tests feed callback() by hand)
    } else if (inputs) {
        io = plausibleLatency(stream_->inputLatency(), askedLatency_) + outputLatency();
    } else {  // other drivers (JACK: a second client): their default input device, for the takes
        if (!openInput(want, error)) return false;
        io = plausibleLatency(inputStream_->inputLatency(), blocksize > 0 ? (double)blocksize / sr : 0.0) + outputLatency();
    }
    recordIo_ = io;
    return true;
}

std::shared_ptr<Recorder> Engine::addRecorder(const RecordInput& take, std::string& error) {
    if (!playing) {
        error = "not recording";
        return nullptr;
    }
    auto old = recorders();
    int want = 1;
    for (int c : take.channels) want = std::max(want, c + 1);
    double io = recordIo_;
    if ((!old || (stream_ && !inputs)) && !prepareInput(want, io, error)) return nullptr;  // (MIDI alone so far)
    auto set = old ? std::make_shared<RecordSet>(*old) : std::make_shared<RecordSet>();
    auto rec = std::make_shared<Recorder>(take.channels, recordIo_, sr, 20.0, recordOffsetMs, take.track);
    set->recorders.push_back(rec);
    set->tracks.push_back(take.track);
    setRecorders(set);
    util::debugf("audio: punched in: a take more (%zu now)\n", set->recorders.size());
    return rec;
}

std::vector<std::shared_ptr<Recorder>> Engine::removeRecorders(const std::vector<const Track*>& tracks) {
    std::vector<std::shared_ptr<Recorder>> gone;
    auto old = recorders();
    if (!old) return gone;
    auto set = std::make_shared<RecordSet>();
    for (size_t i = 0; i < old->recorders.size(); i++) {
        if (std::find(tracks.begin(), tracks.end(), old->tracks[i]) != tracks.end()) {
            gone.push_back(old->recorders[i]);
            continue;
        }
        set->recorders.push_back(old->recorders[i]);
        set->tracks.push_back(old->tracks[i]);
    }
    if (gone.empty()) return gone;
    // (an empty set still records: the transport goes on, and a track armed again punches in)
    setRecorders(set);
    if (!offline) std::this_thread::sleep_for(std::chrono::milliseconds(50));  // (a callback may still be adding to them)
    for (auto& rec : gone) rec->collect();
    util::debugf("audio: punched out %zu take(s), %zu go on\n", gone.size(), set->recorders.size());
    return gone;
}

std::shared_ptr<Recorder> Engine::finishRecording() {
    auto all = finishRecordings();
    return all.empty() ? nullptr : all.front();
}

std::vector<std::shared_ptr<Recorder>> Engine::finishRecordings() {
    auto set = recorders();
    stop();
    countIn = 0;
    if (!monitoring_) closeInput();
    if (!offline) std::this_thread::sleep_for(std::chrono::milliseconds(50));  // a callback that is running may still be adding its buffer
    keep(set);
    std::atomic_store(&recorders_, std::shared_ptr<const RecordSet>());
    if (!set) return {};
    for (auto& rec : set->recorders) rec->collect();
    return set->recorders;
}

bool Engine::startMidiRecording(int64_t count, AudioPtr clicks, std::string& error) {
    if ((!stream_ && !offline) || playing) {
        error = "The audio device isn't ready";
        return false;
    }
    recordingMidi = true;
    if (clicks && count) preview(clicks, 0, false, 1.0);  // (as loud as the metronome)
    countIn = count;
    play();
    return true;
}

void Engine::finishMidiRecording() {
    stop();
    countIn = 0;
    recordingMidi = false;
}

bool Engine::heardNow(double& frame) const {
    if (!playing || !haveClock_.load()) return false;
    double made = clockMade_.load();
    int64_t at = clockFrame_.load();
    frame = (double)at + (now() - made - outputLatency()) * sr - latency.load();
    return true;
}

void Engine::inputCallback(const float* in, int frames) {
    // the separate input device: into each take once it has begun, and into the ring while monitoring
    if (auto set = std::atomic_load(&recorders_); set && playing)
        for (auto& rec : set->recorders)
            if (rec->start.load() != Recorder::NONE) rec->take(in, inputStreamChannels_, frames);
    auto ring = std::atomic_load(&inputRing_);
    if (!ring || !monitoring_.load() || !in) return;
    size_t w = (size_t)ring->channels, size = ring->data.size();
    uint64_t head = ring->head.load(std::memory_order_relaxed), tail = ring->tail.load(std::memory_order_acquire);
    size_t n = std::min((size_t)frames * w, size - (size_t)(head - tail));  // (full: the newest is dropped)
    for (size_t i = 0; i < n; i++) ring->data[(head + i) % size] = in[i];
    ring->head.store(head + n, std::memory_order_release);
}

void Engine::seek(int64_t frame) {
    std::lock_guard<std::mutex> g(posLock_);
    frame = std::max<int64_t>(0, frame);
    pos = frame;
    playFrom_ = frame;
}

int64_t Engine::heardPos() const {
    std::lock_guard<std::mutex> g(posLock_);
    return playing ? std::max<int64_t>(playFrom_, pos - latency) : pos.load();
}

void Engine::play() {
    if (playing || (!stream_ && !offline)) return;
    metering->resetLoudness();  // integrated LUFS covers this playback
    metering->resetPeaks();     // (the meters' loudest peaks and clip lights start again)
    cpu.late = 0;
    clockReset_ = true;
    // (Plugins are not reset here: the audio thread keeps using them even while stopped. Instruments
    //  get "all notes off" instead.)
    std::lock_guard<std::mutex> g(posLock_);
    playFrom_ = pos.load();
    playing = true;
}

void Engine::stop() {
    countIn = 0;
    if (!playing) return;
    std::lock_guard<std::mutex> g(posLock_);
    pos = std::max<int64_t>(playFrom_, pos - latency);  // the playback cursor stays where the sound stopped
    playing = false;
}

void Engine::preview(AudioPtr data, int64_t start, bool loop, double gain) {
    auto voice = std::make_shared<PreviewVoice>();
    voice->data = std::move(data);
    voice->gain = (float)gain;
    voice->pos = start;
    voice->loop = loop;
    keep(guiPreview_);
    guiPreview_ = voice;
    std::atomic_store(&preview_, voice);
}

void Engine::stopPreview() {
    keep(guiPreview_);
    guiPreview_.reset();
    std::atomic_store(&preview_, std::shared_ptr<PreviewVoice>());
}

std::shared_ptr<live::LiveNotes> Engine::liveNotes(const Track* track) {
    auto map = std::atomic_load(&live_);
    live::NoteRender render;  // (its built-in notes: the track's sampler, or the synth)
    for (auto& t : project.tracks)
        if (t.get() == track) render = sampler::noteRenderer(*t);
    for (auto& kv : *map)
        if (kv.first == track) {
            kv.second->setRenderer(render);
            return kv.second;
        }
    auto notes = std::make_shared<live::LiveNotes>();
    notes->setRenderer(render);
    auto next = std::make_shared<live::LiveMap>();
    for (auto& kv : *map)
        for (auto& t : project.tracks)
            if (t.get() == kv.first) {
                next->push_back(kv);
                break;
            }
    next->emplace_back(track, notes);
    keep(map);
    std::atomic_store(&live_, live::LiveMapPtr(next));
    return notes;
}

void Engine::setMetronome(const std::optional<metronome::MetronomeSettings>& settings) {
    metronome_ = settings;
    std::shared_ptr<const metronome::Clicks> clicks;
    if (settings) clicks = std::make_shared<const metronome::Clicks>(metronome::renderClicks(*settings, sr));
    keep(std::atomic_load(&clicks_));
    std::atomic_store(&clicks_, clicks);
}

std::string Engine::lastError() const {
    std::lock_guard<std::mutex> g(errorLock_);
    return lastError_;
}

void Engine::setError(const char* what) {
    if (errorLock_.try_lock()) {  // (never waits: the GUI holds it for a moment at most)
        try {
            lastError_ = what;
        } catch (...) {
        }
        errorLock_.unlock();
    }
}

Engine::PluginsPaused::PluginsPaused(Engine& e) : e_(e) {
    e_.pluginsPaused = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let a callback that is already running finish with its plugins
}

Engine::PluginsPaused::~PluginsPaused() {
    e_.restart_ = true;
    e_.pluginsPaused = false;
}

bool Engine::hasPlugins() const {
    auto tracks = project.tracks.get();
    for (auto& t : *tracks) {
        if (auto s = aload(t->instrument))
            if (aload(s->instance)) return true;
        auto rack = t->rack.get();
        for (auto& s : *rack)
            if (aload(s->instance)) return true;
        auto fx = t->effects.get();
        for (auto& s : *fx)
            if (aload(s->instance)) return true;
        int depth = 0;  // (the buses of its groups: model.h GROUP BUSES)
        for (Group* g = t->group.get(); g && depth < 4096; g = aload(g->parent).get(), depth++) {
            auto busFx = g->bus->effects.get();
            for (auto& s : *busFx)
                if (aload(s->instance)) return true;
        }
    }
    auto master = project.masterEffects.get();
    for (auto& s : *master)
        if (aload(s->instance)) return true;
    return false;
}

void Engine::checkDropout(double t, int frames, double budget, bool underflow) {
    // Count likely clicks for the CPU monitor: the driver reported an underflow, or the audio
    // produced has fallen behind real time by more than the device had queued, so it ran dry.
    // Comparing totals rather than the gap between two callbacks copes with drivers that call in
    // bursts or with uneven buffer sizes.
    if (clockReset_.exchange(false) || !clockValid_ || t - clockStart_ > CLOCK_REANCHOR_S) {
        // (time, frames produced) since a reference point; renewed now and then so the slight drift
        // between the audio device's clock and the PC's never adds up to a false alarm
        clockStart_ = t, clockFrames_ = 0, clockValid_ = true;
    }
    double behind = (t - clockStart_) - clockFrames_ / sr;
    if (underflow || behind > std::max(LATE_FACTOR * budget, outputLatency())) {
        cpu.late++;
        clockStart_ = t, clockFrames_ = 0;  // the device has re-synchronised; measure from here
    }
    clockFrames_ += frames;
}

void Engine::advance(int frames, std::vector<Part>& parts) {
    parts.clear();
    int offset = 0;
    int64_t p = pos.load();
    std::shared_ptr<const LoopRange> lp =
        (recordingMidi || std::atomic_load(&recorders_)) && !loopRecording.load() ? nullptr : std::atomic_load(&loop_);
    int64_t ci = countIn.load();
    if (ci > 0) {
        offset = (int)std::min<int64_t>(ci, frames);
        countIn = ci - offset;
        parts.push_back({0, offset, false, 0});
        if (offset == frames) return;
    }
    // (<=: a loop's end right at the end of this buffer jumps too; otherwise the next buffer would
    // start at the end, "after" the loop, and play on: loops whose length is a whole number of buffers)
    while (lp && p < lp->end && lp->end <= p + frames - offset) {
        int n = (int)(lp->end - p);
        parts.push_back({offset, n, true, p});
        offset += n;
        p = lp->start;
        playFrom_ = p;  // (the playback cursor never shows the time before the jump)
    }
    if (offset < frames) parts.push_back({offset, frames - offset, true, p});
    pos = p + frames - offset;
}

void Engine::playPreview(float* out, int frames, const std::shared_ptr<PreviewVoice>& voice) {
    // add the next buffer of the preview to outputs 1/2; forget it once done (a looping one starts over)
    int width = std::min(2, channels);
    const AudioBuffer* data = voice->data.get();
    int64_t total = data ? data->frames() : 0;
    int64_t p = voice->pos;
    int done = 0;
    float g = voice->gain;
    while (done < frames) {
        int64_t chunk = std::max<int64_t>(0, std::min<int64_t>(frames - done, total - p));
        for (int64_t i = 0; i < chunk; i++) {
            const float* f = data->frame(p + i);
            float* o = out + (size_t)(done + i) * channels;
            for (int c = 0; c < width; c++) o[c] += f[c] * g;
        }
        done += (int)chunk;
        p += chunk;
        if (p < total) continue;
        if (voice->loop && total) {
            p = 0;
        } else {
            std::shared_ptr<PreviewVoice> expected = voice;
            std::atomic_compare_exchange_strong(&preview_, &expected, std::shared_ptr<PreviewVoice>());
            break;
        }
    }
    voice->pos = p;
}

void Engine::callback(const float* in, float* out, int frames) {
    double started = now();
    double budget = (double)frames / sr;
    input_ = in;
    inputChannels_ = inputs;
    bool underflow = false;
    if (stream_) {
        int u = stream_->underflows.load();
        underflow = u != lastUnderflows_;
        lastUnderflows_ = u;
    }
    bool isPlaying = playing;
    if (isPlaying && stream_) checkDropout(started, frames, budget, underflow);
    std::memset(out, 0, sizeof(float) * (size_t)frames * channels);
    bool plugins = !pluginsPaused;
    // Plugins keep running while stopped too, like in any DAW: synths stay playable from their own
    // keyboard, reverb tails ring out, and plugins don't report "suspended".
    auto voice = std::atomic_load(&preview_);
    auto live = std::atomic_load(&live_);
    bool playingLive = false;
    if (live)
        for (auto& kv : *live)
            if (kv.second->active()) playingLive = true;
    // MONITORING: the inputs heard through the tracks that monitor them: the duplex stream's, or the separate
    // input's latest buffer from its ring (kept at most a few buffers behind)
    const float* heardIn = nullptr;
    int heardChannels = 0;
    if (monitoring_.load()) {
        if (inputs && in) {
            heardIn = in, heardChannels = inputs;
        } else if (auto ring = std::atomic_load(&inputRing_)) {
            size_t w = (size_t)ring->channels, size = ring->data.size();
            if (w && (size_t)frames * w <= ring->scratch.size()) {
                uint64_t head = ring->head.load(std::memory_order_acquire), tail = ring->tail.load(std::memory_order_relaxed);
                size_t avail = (size_t)(head - tail) / w;
                if (avail > (size_t)frames * 3) tail = head - (uint64_t)frames * w, avail = (size_t)frames;  // (fallen behind)
                size_t n = std::min(avail, (size_t)frames);
                float* dst = ring->scratch.data();
                for (size_t i = 0; i < n * w; i++) dst[i] = ring->data[(tail + i) % size];
                std::memset(dst + n * w, 0, sizeof(float) * ((size_t)frames - n) * w);
                ring->tail.store(tail + n * w, std::memory_order_release);
                heardIn = dst, heardChannels = (int)w;
            }
        }
    }
    bool mixed = false;
    if (isPlaying || voice || playingLive || heardIn || (plugins && hasPlugins())) {
        mixed = true;
        parts_.clear();
        if (isPlaying) {
            std::lock_guard<std::mutex> g(posLock_);
            if (playing)
                advance(frames, parts_);
            else
                isPlaying = false;  // (stopped just now)
        }
        if (!isPlaying) parts_.push_back({0, frames, false, 0});  // transport stopped: no clips, only the plugins
        // (the timeline frame at this buffer's start, for heardNow)
        bool clock = false;
        for (const Part& part : parts_)
            if (part.hasStart && isPlaying) {
                haveClock_ = false;
                clockMade_ = started;
                clockFrame_ = part.start - part.offset;
                clock = true;
                break;
            }
        haveClock_ = clock;
        Metering* m = metering.get();
        bool metered = m->active();
        tap_->m = m;
        tap_->metered = metered;
        tap_->cpu = &trackCpu;
        auto recs = std::atomic_load(&recorders_);
        mix_.recording = recs && !recs->tracks.empty() ? recs->tracks.data() : nullptr;
        mix_.recordingCount = recs ? (int)recs->tracks.size() : 0;
        mix_.inputChannels = heardChannels;
        for (const Part& part : parts_) {  // (more parts where the loop jumps back, or a count-in ends)
            bool moving = part.hasStart;  // (not while counting in)
            // all notes off when playback starts, stops, the playhead is moved or loops, or plugins were paused
            bool restart = restart_ || moving != wasPlaying_ || (moving && part.start != nextPos_);
            if (plugins) restart_ = false;
            wasPlaying_ = moving;
            nextPos_ = moving ? part.start + part.frames : 0;
            if (recs && moving)
                for (auto& rec : recs->recorders) {
                    rec->begin(part.start, latency);
                    if (inputs && input_) rec->take(input_ + (size_t)part.offset * inputChannels_, inputChannels_, part.frames);
                }
            mix_.input = heardIn ? heardIn + (size_t)part.offset * heardChannels : nullptr;
            float* partOut = out + (size_t)part.offset * channels;
            try {
                latency = mixer::mixClips(project, partOut, channels, part.frames, moving, part.start, mix_,
                                          tap_.get(), sr, restart, plugins, live.get());
            } catch (const std::exception& e) {  // never let an error stop the audio stream: play silence, remember why
                std::memset(partOut, 0, sizeof(float) * (size_t)part.frames * channels);
                setError(e.what());
            } catch (...) {
                std::memset(partOut, 0, sizeof(float) * (size_t)part.frames * channels);
                setError("unknown error");
            }
        }
        if (voice) playPreview(out, frames, voice);
        float gain = masterMute ? 0.0f : (float)masterGain.load();
        size_t total = (size_t)frames * channels;
        if (gain != 1.0f)
            for (size_t i = 0; i < total; i++) out[i] *= gain;
        if (metered) m->tapInterleaved(MASTER, out, channels, 0, frames);
        auto clicks = std::atomic_load(&clicks_);
        if (isPlaying && clicks) {
            // in time with what is heard (delay compensation), never before where playback started; on the
            // tempo map's beats
            int lat = latency;
            TempoMapPtr map = project.timing.audioMap();
            for (const Part& part : parts_)
                if (part.hasStart)
                    metronome::addClicks(out + (size_t)part.offset * channels, channels, part.frames, part.start - lat,
                                         *clicks, *map, sr, gain, playFrom_, beats_);
        }
    }
    if (auto mo = std::atomic_load(&midiOut_)) {  // MIDI out (midi_out.h): clock, Start/Stop, External synths' notes
        if (!mixed) parts_.assign(1, {0, frames, false, 0});  // (nothing mixed: stopped; no allocation: it has room)
        midiParts_.clear();
        for (const Part& part : parts_) midiParts_.push_back({part.offset, part.frames, part.hasStart && isPlaying, part.start});
        midi_out::Buffer mb;
        mb.now = started;
        mb.outLatency = outLatency_.load();
        mb.latency = latency;
        mb.frames = frames;
        mb.sr = sr;
        mb.in = heardIn;  // (the inputs as monitoring hears them: External synths' Measure)
        mb.inChannels = heardChannels;
        TempoMapPtr map = project.timing.audioMap();
        mb.map = map.get();
        auto tracks = project.tracks.get();
        auto clips = project.clips.get();
        mb.tracks = tracks.get();
        mb.clips = clips.get();
        std::shared_ptr<const LoopRange> lp =
            recordingMidi || (recorder() && !loopRecording.load()) ? nullptr : std::atomic_load(&loop_);
        if (lp) mb.loops = true, mb.loopStart = lp->start, mb.loopEnd = lp->end;
        mo->process(mb, midiParts_.data(), (int)midiParts_.size());
    }
    cpu.add(now() - started, budget);
    trackCpu.addBudget(budget);
}

void Engine::setMidiOut(std::shared_ptr<midi_out::Out> out) {
    keep(std::atomic_load(&midiOut_));  // (the audio thread may hold it this very moment: never freed there)
    std::atomic_store(&midiOut_, out);
}

}  // namespace audio
