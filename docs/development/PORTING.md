# Exbit in C++ — how the port is organised (read this first)

The Python program (repository root: `main.py`, `daw/`, `native/`) is the specification. The C++
program in `cpp/` does everything it does, with **no third-party libraries and nothing fetched**:
only a C++17 compiler and the system's own libraries (X11, ALSA, PulseAudio, libdl, pthreads).
Everything else is our own code: the GUI toolkit, the 2D renderer and font rasterizer, SVG and
PNG, WAV/AIFF/FLAC/MP3/Ogg Vorbis, MIDI files, JSON, VST2 and VST3 hosting (our own declaration
of the VST3 interfaces — no Steinberg SDK). One `make` in `cpp/` builds the whole program.

## Rules for everyone working on it

1. **Port faithfully.** Read the Python module(s) you port completely and reproduce their
   behaviour, numbers, texts (menu entries, tooltips, messages), and edge cases. Keep the Python
   docstrings' explanations as comments where they help. The user guide `GUIDE.md` (repo root)
   describes what the user sees.
2. **Contracts.** The headers already written (`src/config.h`, `src/core/*.h`, `src/gfx/*.h`,
   `src/platform/platform.h`, `src/audio/devices.h`, `src/audio/sound_file.h`, `src/audio/dsp.h`,
   `src/audio/synth.h`, `src/plugins/plugin.h`, `src/model/*.h`, `src/ui/theme.h`,
   `src/ui/toolkit/*.h`) are contracts between modules. Implement them as declared. You may **add**
   members, functions or new headers when you need them; never remove or rename what is declared
   (others code against it). If a contract is genuinely wrong, add a better alternative next to it
   and say so in your final report.
3. **Stay in your files.** Only create/edit the files of your assignment. Shared files that others
   own: if you need something there, work around it in your own files and report it.
4. **Building while others work.** Several people build in this tree at the same time, so the full
   `make` may fail because of someone else's unfinished file. Compile your own objects with
   `make build/src/<dir>/<file>.o` (the pattern rule works per file), and link your tests by hand
   against just the objects they need, e.g.
   `g++ -std=c++17 -O1 -g -Isrc -pthread tests/test_gfx.cpp build/src/gfx/*.o build/src/core/*.o -o build/tests/test_gfx -lm`.
   Tests: `tests/test_<area>.cpp`, using `tests/check.h` (CHECK, CHECK_NEAR, TEST_MAIN_END).
   `make test` builds and runs every `tests/*.cpp` against all of `src/` (minus `main.cpp`), so a
   test must not define symbols that `src/` defines.
5. **Threads.** The GUI thread owns the project; the audio thread reads it through `List<T>`
   snapshots (`model/model.h`, THREADS). The audio thread never allocates large buffers, never
   locks a mutex the GUI holds for long, never loads/destroys plugins, and never throws out of the
   callback.
6. **The look.** Flat, as `daw/ui/theme.py`'s stylesheet: plain fills in the skin's colours
   (`theme::c("name")`, `theme::shades()`), hover lightens, lit/checked = accent, corners
   `theme::radius()`. Fonts: `theme::uiFont()` (9 pt), `theme::monoFont(pt)`.
7. **Linux first.** Paths: settings and caches in `plat::configDir()` (`~/.config/Exbit`, the
   Windows program's `%APPDATA%\Exbit`). Plugins: VST3 in `~/.vst3`, `/usr/lib/vst3`,
   `/usr/local/lib/vst3` (bundles `X.vst3/Contents/x86_64-linux/X.so`); VST2 `.so` files in the
   folders chosen in Options → VST2 plugin folders (usual: `~/.vst`, `/usr/lib/vst`,
   `/usr/local/lib/vst`, `/usr/lib/lxvst`). Audio driver types: ALSA, PulseAudio.
8. **No GUI pop-ups in tests.** Test drawing by painting into an `Image` (widgets can be painted
   without a window: `Widget::paintTree`). If you must open a window to check something, use
   `WindowOptions::offscreen` or close it at once. Never play loud sound in tests.

## Layout of src/

```
main.cpp                 start: exbit [project.json | audio and MIDI files...]; --probe <list> (plugin types)
config.h                 daw/config.py
core/                    json (json module), util (files, paths, strings), settings (QSettings)
gfx/                     geom (Color…), image (+PNG, zlib), painter (QPainter), font (TrueType), svg
platform/                platform.h, x11.cpp (windows, events, main loop, clipboard, XDND drops)
model/                   model.py, timing.py, edits.py, project_io.py
audio/                   devices (ALSA/PulseAudio), sound_file (+ codecs: mp3, vorbis, flac), file_io.py,
                         dsp.py, synth.py, engine.py, mixer.py, patch.py, plugin_audio (plugins.py's
                         audio-thread half), automation.py, live.py, render.py, metronome.py,
                         loudness.py, metering.py, cpu.py
plugins/                 plugin.h, vst3_api.h (our VST3 interfaces), vst3host.cpp, vst2.h, vst2host.cpp,
                         library (plugins.py's PluginLibrary, plugin_probe.py, juce_state.py)
ui/toolkit/              widget, layout, controls, menu, dialogs (the Qt stand-in)
ui/                      theme, skin, colors, formatting, and every window of daw/ui/
```
