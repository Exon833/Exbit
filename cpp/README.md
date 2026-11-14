# Tiny DAW in C++ (Linux)

A port of Tiny DAW (the Python/PySide6 program in the repository root) to C++17, for Linux. It
does what the Python program does — timeline, MIDI editor, Explorer, patch-space effects, VST3
and VST2 plugins, automation, recording, freezing and bouncing, rendering, skins — with **no
third-party libraries**: the GUI toolkit, 2D renderer and font rasterizer, SVG/PNG, the audio
codecs, MIDI files, JSON and the VST2/VST3 hosting are all Tiny DAW's own code. The user guide is
[`../GUIDE.md`](../GUIDE.md); what is different on Linux is listed below.

## Build

Needs a C++17 compiler (g++ or clang++), `make`, and the development files of X11 (with Xext,
Xrandr, Xcursor, Xi), ALSA and PulseAudio (`libpulse-simple`). Nothing is downloaded.

    cd cpp
    make -j16            # ./tinydaw  (release, -O2)
    make DEBUG=1 -j16    # ./tinydaw-debug (-O0 -g, AddressSanitizer + UBSan), objects in build-debug/
    make clean

## Run

    ./tinydaw                         # an empty project
    ./tinydaw song.json               # open a project
    ./tinydaw drums.wav bass.mid      # add audio and MIDI files (onto tracks of their kind)

The program finds its data (`data/icons`, `data/skins`) next to the executable (or in
`../share/tinydaw`). Settings, your skins, the plugin cache and plugin pictures are kept in
`~/.config/TinyDAW` (`$XDG_CONFIG_HOME/TinyDAW`). `tinydaw --probe <list>` is the background
process that finds out plugin types; the program starts it itself.

## Test

    TINYDAW_NO_WINDOWS=1 make -j16 test

builds and runs every `tests/test_*.cpp` (each linked against all of `src/` except `main.cpp`)
and the two test plugins in `build/test-plugins/` (a VST3 effect and a VST2 synth, built on Tiny
DAW's own plugin headers).

Testing aids of the running program (hidden; see `src/ui/test_script.h`):

| variable | what it does |
|---|---|
| `TINYDAW_SCREENSHOT=<file.png>` | saves a picture of the main window after 60 frames (`TINYDAW_SCREENSHOT_FRAMES`) and quits |
| `TINYDAW_SCRIPT=<file>` | drives the program from a script: clicks, keys, typing, menus, adding tracks/files/plugins, playing, saving, opening, screenshots |
| `TINYDAW_VST3_PATH=<dir>[:<dir>…]` | more VST3 folders besides the standard ones (e.g. `build/test-plugins`) |
| `TINYDAW_NO_UPDATE_CHECK=1` | doesn't ask GitHub for a newer version |
| `XDG_CONFIG_HOME=<dir>` | settings elsewhere, to leave your own alone |

## Folder layout

    Makefile
    data/            icons/ (transport buttons), skins/ (Dark, Light, Midnight, OLED)
    src/main.cpp     start-up (main.py)
    src/config.h     sizes, limits, defaults (config.py)
    src/core/        JSON, files and paths, strings, settings (QSettings)
    src/gfx/         geometry and colours, images + PNG, the painter, TrueType fonts, SVG
    src/platform/    windows, events, main loop, clipboard, drops (Xlib)
    src/model/       the project (model.py), timing, edits, project files (project_io.py)
    src/audio/       devices (ALSA, PulseAudio), sound files (WAV/AIFF/FLAC/MP3/Ogg Vorbis), engine,
                     mixer, patch cables, synth, automation, metronome, loudness, metering, rendering
    src/plugins/     VST3 (our own declaration of the interfaces) and VST2 hosting, the plugin
                     library and cache, the out-of-process type probe
    src/ui/toolkit/  the GUI toolkit (widgets, layouts, controls, menus, dialogs, file chooser)
    src/ui/          every window and panel of daw/ui/ (main_window, timeline, lane_panel, …)
    tests/           unit tests (tests/check.h) and tests/plugins/ (the test plugins' sources)
    tools/           manual checks (toolkit demo, platform check, plugin editor check)
    *.md             notes for the people who ported it: PORTING, UI_PORTING, API_NOTES, INTEGRATION

## What differs from the Python program

- **Linux only** (X11; it runs on Wayland desktops through XWayland).
- **Audio drivers**: ALSA and PulseAudio (PipeWire through either) instead of ASIO, WASAPI,
  WDM-KS, DirectSound and MME. No exclusive mode (an ALSA `hw` device is always the program's
  alone; PulseAudio is always shared).
- **Plugins**: Linux VST3 bundles (`X.vst3/Contents/x86_64-linux/X.so`) from `~/.vst3`,
  `/usr/lib/vst3`, `/usr/local/lib/vst3`, and Linux VST2 `.so` plugins from the folders chosen in
  Options → VST2 plugin folders (the usual ones: `~/.vst`, `/usr/lib/vst`, `/usr/local/lib/vst`,
  `/usr/lib/lxvst`). Windows plugins (`.dll`, `.vst3` for Windows) can't be loaded.
- **Paths**: `~/.config/TinyDAW` where the Python program uses `%APPDATA%\TinyDAW`.
- **Look**: Tiny DAW's own toolkit draws everything in the skins' colours, as the Python
  program's stylesheet does; dialogs (including the file chooser) are the program's own, not the
  desktop's. Fonts are the system's TrueType fonts (Consolas falls back to a monospaced one).
- **The Explorer** is a panel on the left that View → Explorer (Ctrl+E) or its ✕ shows and hides
  and whose width can be dragged; unlike Qt's dock it can't be floated or moved to the right side.
- The update check asks GitHub through the system's `curl`.
