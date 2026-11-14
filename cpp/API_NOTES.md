# APIs of the finished modules (for the screen ports)

Read the headers for details; this is the map.

## Engine (`audio/engine.h`, namespace audio) — port of engine.py
- `audio::Engine(Project&)`; `bool open(driverType, device, int blocksize /*<0 keeps*/, bool exclusive, std::string& error)`,
  `close()`, `isOpen()`, `deviceName()`, `outputLatency()`, `inputCount()`.
- `play()`, `stop()`, `seek(frame)`, `heardPos()`; `setLoop(start, end)` / `clearLoop()` / `std::optional<LoopRange> loop()`.
- `preview(AudioPtr, start=0, loop=false, gain=0.7)`, `stopPreview()`, `previewVoice()`.
- `liveNotes(const Track*)` → LiveNotes (`noteOn(pitch, vel, sr, builtin, lengthS=-1)`, `noteOff`).
- `setMetronome(std::optional<MetronomeSettings>)`; `startRecording(channels, countIn, AudioPtr clicks, error)`,
  `finishRecording()` → Recorder (`Recorder::audio(out)` gives the timeline frame).
- `lastError()`, `idle()` (call EVERY FRAME on the GUI thread), RAII `Engine::PluginsPaused` (renders, syncState).
- Fields: `sr, channels, blocksize, driverType, exclusive, inputs`; atomics `pos, playing, countIn, masterGain,
  masterMute, latency, pluginsPaused`; `metering` (shared_ptr, replaced on open), `cpu`.
- GUI duties: `patch::ensureStates(project)` after adding tracks / changing cables, effects, racks;
  `metering->forgetGone(*project.tracks)` when tracks are removed; replace `PluginSlot::instance` with
  `std::atomic_store` (the audio thread atomic_loads it, and `frozen`, `instrument`, `clip->track`).
- render (`audio/render.h`): `renderTrack(project, track, sr, progress)` → optional `Rendered{start, interleaved stereo}`
  (throws "X stopped: err"); `renderMix(project, start, end, sr, masterDb, tail, progress)` → optional audio;
  `clearPatchStates(project)`; progress returning false cancels.
- Metering: `takePeaks(master, map<const Track*, vector<float>>)`, `updateLoudness()`, `read(key, m, s, i)`
  (`MASTER = nullptr`), `forgetGone(tracks)`, `levelsOn` / `loudnessOn`. `CpuMeter::read(avgSeconds, cur, avg)`; `late`.
- automation: `resolve(project)` every frame, `parameters(slot)`, `TweakWatcher(slot, project).poll()`, `prune`,
  `valueAt`, `snap`. plugin_audio: `syncState(slot, sr)`, `trackLatency`, `Parameter`.
- metronome: `MetronomeSettings`, `Click`, `renderClick`, `renderClicks`.

## Projects (`model/project_io.h`, namespace project_io)
- `MasterSettings { double volumeDb; bool mute; }`
- `Json describe(const Project&, int sr, const MasterSettings&, AudioFileFn audioFile = nullptr, bool states = true)`;
  unsaved-change snapshot: `describe(p, sr, m, nullptr, false)` compared with `==`.
- `bool saveProject(path, Project&, sr, const MasterSettings&, std::string& error)`;
  `bool loadProject(path, Project&, sr, MasterSettings& out, std::string& error)` (old plugins retired; then load
  `project.pluginSlots()`). `audioFolder(path)`.

## Files (`audio/file_io.h`, namespace file_io)
- `AUDIO_EXTENSIONS`, `MIDI_EXTENSIONS`, `isMidi`, `isSample`, `loadAudio(path, AudioPtr&, int& sr, err)`,
  `loadMidi(path, vector<Note>&, double& len, err)`, `loadPlayable(path, sr, AudioPtr&, err)`.
- `audio/sound_file.h`: `writeWav`, `writeFlac` (render to file).

## Plugins (`plugins/plugin.h`, `plugins/library.h`, `plugins/probe.h`, namespace plug)
- `PluginInfo { path, name, kind /* "" unknown */, pluginName; label(); }`; `PluginChoiceNeeded : runtime_error { names }`.
- `PluginLibrary(folders = defaultFolders(), cachePath = "", vst2Folders = {})`: `plugins`, `scan()`, `scanVst2()`,
  `unknownFiles()`, `probeFailedBefore(f)`, `learn`, `probeFailed`, `ofKind(kind)`,
  `std::unique_ptr<Plugin> load(const PluginInfo&, double sr)` (throws), `lastLoadSeconds(info)` (-1 never),
  `saveCache`. Free: `defaultFolders()`, `vst2UsualFolders()`, `stateOf(Plugin&)`, `setState(Plugin&, blob)` (throws),
  `unloadUnusedModules()`.
- `int runProbe(listFile)` (main.cpp: `--probe <list>`); `KindFinder(PluginLibrary&)`: `progress(done,total)`,
  `finished(found)` callbacks, `start()`, `cancel()`, `working()`. Destroy it from `finished` only via `plat::postToMain`.
- Plugin editors: `Plugin::openEditor(parentXWindow, display, scale, w, h)`, `closeEditor()`, `idle()` every frame,
  `requestedSize`, `editorResized`. Platform window: `plat::Window::nativeHandle()/nativeDisplay()/capture()`;
  `WindowOptions::offscreen` for invisible photographs.

## Platform additions
- `plat::hasDisplay()`, `plat::setNativeEventFilter()`, `plat::callLater()`; `audio::Stream::lost`.
