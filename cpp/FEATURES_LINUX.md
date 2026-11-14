# Porting the features of Tiny-DAW-Linux into the C++ program

The C++ program (`cpp/`) is a port of Tiny DAW 0.4 (`daw/` in this repository). The project went on as
**Tiny-DAW-Linux** (rolling release), with ~30 feature commits and ~16 000 changed lines. A clone is at:

    /tmp/claude-1000/-home-martins-Desktop-Tiny-DAW--claude-worktrees-project-recreation-cpp-7c1273/d3237c99-b885-495e-8335-30a949e2fd2b/scratchpad/TinyDAW-Linux

(call it `$LINUX`). The 0.4 code we ported is commit `5cae65b` there. **The spec for each feature is the
diff**: `git -C $LINUX show <sha>` for one commit, `git -C $LINUX diff 5cae65b HEAD -- <file>` for everything
that changed in one file, and the new `$LINUX/GUIDE.md` for what the user sees. Read the Python as it is
at HEAD for the complete picture. Never edit `$LINUX`.

Rules as before: `cpp/PORTING.md`, `cpp/UI_PORTING.md`, `cpp/API_NOTES.md`, `cpp/INTEGRATION.md`. No
third-party libraries and nothing downloaded: system libraries may be used only as the existing code does
(X11, ALSA, PulseAudio, libdl). JACK: `libjack.so.0` is loaded at run time with our own declarations of
its C API (as `plugins/vst2.h` declares VST2), never linked. Undo is snapshot-based (history.py), so editing
code doesn't need hooks: the main window checks for changes while no mouse button is held.

## Not ported (they don't apply to a compiled, self-drawn program)
- `start.py` / git self-update (the update notice compares the build's commit with GitHub instead).
- numba, Qt's native-Wayland mode, `x11_window.py`, `resize_repaint.py` (Qt-specific). Interface size
  75–200 % is ported (our toolkit scales).
- `native_engine.py` / Python-vs-C++ engine switch: our engine is already C++; port the *behaviour* of
  `native/engine.cpp` and `native/effects.cpp` into `src/audio/`.

## Phase A (now, in parallel)
- **A core model** — config.py, model.py, timing.py (tempo map, time-signature changes, fixed grid,
  snapping on/off), edits.py, history.py (undo/redo), song_time.py (insert/delete time), paths.py,
  project_io.py (all new fields, backups `<name>_backups`, templates' loading helpers), synth.py,
  file_io.py (MIDI file writing). Owns `src/model/*`, `src/audio/synth.*`, `src/audio/file_io.*`,
  `src/audio/sound_file.*` (MIDI writer).
- **D1 encoders + stretch** — our own MP3 (320 kbps CBR) and Ogg Vorbis encoders for File → Render; the
  phase vocoder of stretch.py. Owns `src/audio/mp3_encoder.*`, `vorbis_encoder.*`, `stretch.*`.
- **D2 built-in effects** — effect.py, effects.py, kernels.py, effects/*.py and native/effects.cpp:
  EQ, Chorus, Compressor, Multi-band compressor, Gate, Reverb, Limiter, Saturation, Bit-crusher, Delay,
  Utility, as `plug::Plugin` implementations (so they live in slots, patches, automation, states like
  any plugin), and effect_window.py (their windows). Owns `src/effects/*`, `src/ui/effect_window.*`.
- **D3 JACK + MIDI input** — the JACK driver type (dlopen libjack.so.0; PipeWire's JACK latency fix),
  the ALSA shared-device 80 ms minimum, and MIDI keyboard input (ALSA sequencer; device list, events with
  timestamps, sustain pedal handling as midi_input.py). Owns `src/audio/devices_jack.cpp`,
  `src/audio/jack_api.h`, `src/audio/midi_in.*`.

## Phase B (after A)
- B1 engine (mixer/engine/patch/render/live/automation/metering + native/engine.cpp behaviour: returns
  and sends, pan, track delay, clip gain and fades, looping clips, stretch/pitch/reverse playback, the
  sampler, MIDI effects, volume/pan automation, takes, MIDI recording, record offset).
- B2 timeline, lane panel, overview, clip painter, waveform cache, transport bar, meters, master strip.
- B3 MIDI editor, MIDI effects dialog, sampler dialog, computer keyboard, capture, MIDI input UI, MIDI export.
- B4 plugins/effects UI (chooser, Mix fader, patch panel), render/stems/normalize, recording offset,
  recording, track render, tempo and stretch dialogs.
- B5 main window and project features (Edit menu, undo/redo, recent, templates, backups, autosave and
  recovery, interface size, high contrast skin, update notice), Ableton Live import.

## Phase A results (APIs for phase B)

### D2 built-in effects (`src/effects/effects.h`, `effects/mix.h`, `ui/effect_window.h`)
- Ids = the successor's script names: eq, chorus, compressor, multiband_compressor, gate, reverb, limiter,
  saturation, bitcrusher, delay, utility; saved paths `"builtin:<id>"`. `effects::list()` → `Info{id, name,
  category, path(), label()}` sorted by name; `BUILTIN_CATEGORIES`; `create(idOrPath[, error])` (prepared at 48 kHz),
  `find`, `categoryOf`, `isBuiltin`, `asEffect(plugin*)`. They are plug::Plugin (state "TDV3" + JSON, automation,
  sidechain, latency). `Plugin::load("builtin:…")` and `PluginLibrary::load` create them; `scan()` does NOT list
  them (the chooser must add `effects::list()` itself, first).
- Mix (dry/wet) for EVERY effect node incl. VST: `effects::DryWet` — main thread `prepare(maxBlock,
  plugin->latency())`; audio thread when `DryWet::active(mix)`: `keep(in, n)` before process(), `blend(out, n,
  plugin->latency(), mix)` after; `clear()` on seek / patch rebuild. Slot field `PluginSlot::mix` (model).
- Windows: for `effects::isBuiltin(slot->path)` open `EffectWindow(ui::Window* parent, SlotPtr slot, title, bool
  invisible=false)` instead of EditorWindow (same members: attach(), place, position, editorClosed, snapshot(),
  watcher, picture, photographed, tickTimer).

### D3 JACK + MIDI input (`src/audio/devices.h`, `audio/midi_in.h`)
- Driver type "JACK" offered first when a server runs (none on this machine: untested live). Additions:
  `minLatency(type, device)`, `SHARED_ALSA_MIN_LATENCY` 0.08, `plausibleLatency()`. For recording with JACK the
  engine opens a second client on `defaultInput("JACK")` per take (duplex would be simpler).
- MIDI: `midi_in::ports()` (names as the Python saved them, "Client:Port 20:0"), `stableName()`,
  `Input::open(name)`, `poll(events)` every frame on the GUI thread, each event through ONE `Sustain::handle`
  (the computer keyboard goes through the same one); results → recorder, Capture log, the selected track's
  `engine.liveNotes(track)`. `Sustain::releaseAll` on close / port switch. `lost()` when unplugged; `setNotify`
  posts a wake-up; `setCallback` runs on the MIDI thread.

### A core model (`model/model.h`, `timing.h`, `history.h`, `song_time.h`, `edits.h`, `project_io.h`, `paths.h`)
- Kind::Return; Track: `delayMs`, `pan`, `sends` (List<Send{TrackPtr track; double db}>), `midiEffects`
  (List<MidiEffect>; `MidiEffect::make(kind, values, out)`, `with(key, v)`, `kinds()`), `sampler` (SamplerPtr),
  `takes` (List<ClipPtr>, their track null), `showTakes`; a fixed `controls` slot (kind "controls"; automation
  param "Volume" / "Pan"; not in pluginSlots(), never loaded); `playsPlugins()`, `playsSampler()`.
- Clip: `gainDb`, `fadeIn/Out/Lead/Tail` (frames), `loop`, `stretch`, `pitch`, `sourceBpm`, `reverse`, `loopStart`,
  `loopLength`, `baseFrames`, `mutedNotes`, `played`; `fades()`, `level(pos)`, `hasLevels()`, `fadeParts()`,
  `repeated()`, `heardNotes()`, `fitWindow()`, `prepare(sr, reach=-1)`, free `looped()`. Hooks phase B sets:
  `clip_hooks::midiEffects`, `clip_hooks::renderSampler` (until set: notes unchanged, built-in synth).
- Project: `markers` (List<Marker>, `setMarkers()`), `returns()` (always last; audible when others are soloed),
  `tracksMade["return"]`. PluginSlot `mix`.
- Timing: `changes` (TempoChange{bar, bpm, beatsPerBar}), `grid`, `snapOn`, `key`, `scale`; TempoMap (`beatAt`,
  `secondsAt`, `barSeconds`, `barAt`, `bpmAt`, `beatSAt`, `beatsPerBarAt`, `beatTimes`…; Timing has them too, plus
  `setChange`, `removeChange`, `changeAtBar`); grid: `snaps(alt)`, `fixedStep`, `gridStep(pps, seconds, drawing)`,
  `stepGrid`, `gridName`, `snap(s, pps, floor)`, `lines`, `position`. Audio thread: `timing.audioMap()`;
  `project.timing.publish()` must be called in Engine::idle().
- `history::History(project, sr)`: `check(sr)` while no mouse button is held, `undo(sr)`, `redo(sr)`,
  `canUndo/canRedo`, `reset(sr)`. Keep removed plugins' `slot->instance` alive (undo brings back the same plugin).
- `song_time::insertTime(p, at, len, sr, note, error)` / `deleteTime(...)`.
- edits: `cutPiece`, `restretched`, `reversedClip`, `consolidated` (join), `crossfaded`.
- project_io: every successor key; `keepBackup(path, backupPath, error)` before saveProject; `backups()`,
  `backupsFolder()`, `BACKUPS_KEPT`; `audioHolders()` (templates); `describeRecovery`, `writeRecovery`,
  `writeJson`, `readMidiEffects`, `scaleNames()`. `paths::dataDir(sub)`, `makeDataDir(sub)`.
- `file_io::saveMidi(path, parts, timing, startS, error)`, `loadMidiBeats`. `MASTER_FOOTER_H` is now 14.
- D1 (encoders, may still be in progress): `encodeMp3(path, interleaved, frames, channels, sr, kbps, error,
  progress)`, `encodeVorbis(path, …, quality, …)` in `audio/mp3_encoder.h` / `vorbis_encoder.h`;
  `stretch::stretched` in `audio/stretch.h`.

## Phase B — who owns which files (edit only yours; others' headers: code against them)
- **B1 engine**: `src/audio/{engine,mixer,patch,render,live,automation,metering,metronome,plugin_audio,cpu}.*`,
  new `src/audio/{sampler,midi_effects,record_offset}.*` (DSP/logic; sets the clip_hooks).
- **B2 timeline & panels**: `src/ui/{timeline*,clip_painter,waveform_cache,overview,master_lane,lane_panel,widgets,
  level_meter,loudness_display,master_strip,transport_bar,cpu_monitor,lanes,colors,formatting,paint_helpers,
  update_notice,selection}.*`, `src/update_check.*`, `src/version.h`, new `src/ui/{clipboard,tempo_dialog,
  stretch_dialog,track_dialogs}.*`.
- **B3 MIDI**: `src/ui/{midi_editor,instrument_menu}.*`, new `src/ui/{midi_effects_dialog,sampler_dialog,
  computer_keyboard,capture,midi_input,midi_export}.*`.
- **B4 plugins & render UI**: `src/ui/{patch_panel,plugin_dialog,plugin_host,plugin_editor,plugin_pictures,
  plugin_windows,automation_dialog,plugin_folders_dialog,mix_export,recording,track_render,audio_dialog,
  color_picker,skin,skin_dialog,metronome_dialog}.*`, new `src/ui/{plugin_chooser,record_offset_dialog}.*`,
  `data/skins/high_contrast.json`.
- **B5 main window**: `src/ui/{main_window,preferences,theme,test_script}.*`, `src/main.cpp`, new
  `src/ui/{autosave,templates}.*`, `data/templates/*.json`, and the interface size (may edit `src/gfx/*` and
  `src/ui/toolkit/*` for scaling, carefully: everyone uses them).
- **B6 Ableton import**: new `src/importers/*`, `src/core/xml.*`, `src/ui/ableton_import.*`.
