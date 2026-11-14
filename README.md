# Tiny DAW

*Goal is to create a fully functional and open-sourced DAW, which is easy to navigate through by anybody.*

Tiny DAW is a digital audio workstation for Linux, written in C++17 with **no third-party
libraries**: its GUI toolkit, 2D renderer and font rasterizer, SVG and PNG, the audio codecs (WAV,
AIFF, FLAC, MP3, Ogg Vorbis — reading and writing), MIDI files, JSON and XML, and VST2/VST3 plugin
hosting are all its own code. It needs only the system's X11, ALSA and PulseAudio (and runs on
Wayland desktops through XWayland). It began as a Python program, which this one recreates and
extends; that program is in the repository's history.

- Please report any bugs that do appear: what isn't working, how to make it happen, and a fix
  suggestion if you know one.
- Desired features are very welcome!

## What it does

- **Timeline**: audio and MIDI tracks, groups, return tracks with sends, pan, track delay; clips you
  move, trim, split, duplicate, copy and paste, colour, loop, reverse, join, crossfade, stretch and
  pitch, with gain and fades; takes with comping; markers, tempo and time-signature changes,
  insert/delete time, a fixed or zoom-following grid; undo and redo of everything.
- **MIDI**: a piano roll with ghost notes of other clips, the song's key and scale, chord stamp,
  quantize, box select; the built-in synth and sampler; MIDI effects (arpeggiator, chord, scale,
  note length, velocity, random, pitch); a MIDI keyboard (with sustain pedal) or the computer
  keyboard; MIDI recording, Capture of what you just played, MIDI file import and export.
- **Effects and plugins**: eleven built-in effects (EQ, compressor, multi-band compressor, gate,
  limiter, reverb, delay, chorus, saturation, bit-crusher, utility), Linux VST3 and VST2 plugins,
  each placed and wired by hand in a patch space with sidechains and a Mix (dry/wet) control;
  instrument patch panels; automation of any parameter, volume and pan; plugin delay compensation.
- **Audio**: ALSA, PulseAudio (PipeWire through either) and JACK; recording with count-in and a
  measurable recording offset; freeze and bounce; render to WAV, FLAC, MP3 or OGG with peak or
  loudness normalizing; stems; level meters with peak hold and clip lights; LUFS.
- **Projects**: templates, recent projects, backups of what each save replaces, autosave with crash
  recovery, Ableton Live set import.
- **Look**: skins (Dark, Light, Midnight, OLED, High contrast, or your own, editable), interface size
  75–200 %, a sample Explorer with search and previews, a project overview.

## Build

Needs a C++17 compiler (g++ or clang++), `make`, and the development files of X11 (with Xext,
Xrandr, Xcursor, Xi), ALSA and PulseAudio (`libpulse-simple`). Nothing is downloaded. JACK is used
when its library (`libjack.so.0`, e.g. PipeWire's) is installed; it isn't needed to build.

    make -j16            # ./tinydaw  (release, -O2)
    make DEBUG=1 -j16    # ./tinydaw-debug (-O0 -g, AddressSanitizer + UBSan)
    make clean

## Run

    ./tinydaw                         # an empty project (or your chosen template)
    ./tinydaw song.json               # open a project
    ./tinydaw drums.wav bass.mid      # add audio and MIDI files (onto tracks of their kind)

The program finds its data (`data/icons`, `data/skins`, `data/templates`) next to the executable
(or in `../share/tinydaw`). Settings are kept in `~/.config/TinyDAW`; your skins, templates, plugin
pictures, the plugin cache and crash recovery in `~/.local/share/TinyDAW` (`$XDG_DATA_HOME`).

**Plugins**: Linux VST3 bundles (`X.vst3/Contents/x86_64-linux/X.so`) from `~/.vst3`,
`/usr/lib/vst3`, `/usr/local/lib/vst3`, and Linux VST2 `.so` plugins from the folders chosen in
Options → VST2 plugin folders (the usual ones: `~/.vst`, `/usr/lib/vst`, `/usr/local/lib/vst`,
`/usr/lib/lxvst`). A plugin's type is found out in a separate process (`tinydaw --probe`), so a
plugin that crashes can't take the program with it.

## Test

    TINYDAW_NO_WINDOWS=1 make -j16 test

builds and runs every `tests/test_*.cpp` (each linked against all of `src/` except `main.cpp`)
and the two test plugins in `build/test-plugins/` (a VST3 effect and a VST2 synth, built on Tiny
DAW's own plugin headers).

Testing aids of the running program (hidden; see `src/ui/test_script.h`):

| variable | what it does |
|---|---|
| `TINYDAW_SCREENSHOT=<file.png>` | saves a picture of the main window after 60 frames (`TINYDAW_SCREENSHOT_FRAMES`) and quits |
| `TINYDAW_SCRIPT=<file>` | drives the program from a script: clicks, keys, typing, menus, tracks, files, plugins, playing, undo, templates, autosave, screenshots |
| `TINYDAW_VST3_PATH=<dir>[:<dir>…]` | more VST3 folders besides the standard ones (e.g. `build/test-plugins`) |
| `TINYDAW_NO_UPDATE_CHECK=1` | doesn't ask GitHub whether there are newer commits |
| `XDG_CONFIG_HOME=<dir>`, `XDG_DATA_HOME=<dir>` | settings and data elsewhere, to leave your own alone |

## Folder layout

    Makefile
    data/            icons/ (transport buttons), skins/, templates/ (Recording, Songwriting)
    docs/development/  the notes the program was ported from Python with
    src/main.cpp     start-up
    src/config.h     sizes, limits, defaults
    src/core/        JSON, XML, files and paths, strings, settings
    src/gfx/         geometry and colours, images + PNG, the painter, TrueType fonts, SVG
    src/platform/    windows, events, main loop, clipboard, drops (Xlib)
    src/model/       the project, timing and the tempo map, edits, undo history, song time, project files
    src/audio/       devices (ALSA, PulseAudio, JACK), MIDI input, sound files and codecs (incl. MP3
                     and Vorbis encoders), engine, mixer, patch cables, synth, sampler, MIDI effects,
                     stretch, automation, metronome, loudness, metering, rendering
    src/effects/     the built-in effects
    src/plugins/     VST3 (our own declaration of the interfaces) and VST2 hosting, the plugin
                     library and cache, the out-of-process type probe
    src/importers/   Ableton Live sets (gzip + XML)
    src/ui/toolkit/  the GUI toolkit (widgets, layouts, controls, menus, dialogs, file chooser)
    src/ui/          every window and panel (main_window, timeline, lane_panel, midi_editor, …)
    tests/           unit tests (tests/check.h) and tests/plugins/ (the test plugins' sources)
    tools/           manual checks (toolkit demo, platform check, plugin editor check)

## License

GPL-3.0, see [LICENSE](LICENSE). VST is a registered trademark of Steinberg Media Technologies
GmbH. Tiny DAW hosts VST2 and VST3 plugins through its own declarations of their interfaces
(`src/plugins/vst2.h`, `src/plugins/vst3_api.h`), written for interoperability; it contains no
Steinberg SDK code.
