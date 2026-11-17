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
  Windows VST3 and VST2 plugins through Wine (Tiny DAW's own bridge, no yabridge needed), each
  placed and wired by hand in a patch space with sidechains and a Mix (dry/wet) control;
  instrument patch panels; automation of any parameter, volume and pan; plugin delay compensation.
- **Audio**: ALSA, PulseAudio (PipeWire through either) and JACK; tracks worked out on every
  processor core at once, with each track's CPU use shown; recording with count-in and a
  measurable recording offset; freeze and bounce; render to WAV, FLAC, MP3 or OGG with peak or
  loudness normalizing; stems; level meters with peak hold and clip lights; LUFS; a master scope.
- **Projects**: templates, recent projects, backups of what each save replaces, autosave with crash
  recovery, Ableton Live set import.
- **Look**: skins (Dark, Light, Midnight, OLED, High contrast, or your own, editable), interface size
  75–200 %, a sample Explorer with search and previews, a project overview.

## The master scope

Under the Master row (the right-hand column), a small window shows what the master plays, as a
**waveform** (an oscilloscope: the default) or a **spectrum**. Options → Master scope shows or hides it;
right-click it for its options, which are remembered:

- **Combined channels** (left and right mixed) or **separate** (left and right side by side), in
  either view.
- Waveform: the **capture window** (5 ms … 2 s of the newest sound) and **pitch follow**: the note's
  period is found (YIN) and whole cycles of it are shown (1, 2, 4 or 8), each picture starting where
  the last one did, so a held note stands still; its name and frequency are shown ("A3 220 Hz").
  Where no clear pitch is heard (noise, chords, silence) the capture window runs freely.
- Spectrum: the **FFT size** (1024 … 16384), the **window function** (Hann, Hamming,
  Blackman-Harris, rectangular), how fast it **falls back**, the **slope** (0, 3 or 4.5 dB per octave
  about 1 kHz) and the **range** (60, 90 or 120 dB); frequencies on a logarithmic scale.

It shows the master after its volume (what is heard), outputs 1 and 2. While it is hidden nothing is
kept for it, and the audio thread does nothing more for it at any time: the meters' copy of what was
played is read on the interface thread.

## Several cores

The tracks of a song are worked out side by side on all of the processor's cores, so a song with
many tracks of plugins doesn't have to fit on one core (the audio thread and up to 15 worker
threads, real-time ones where the system allows it; `src/audio/workers.h`). It sounds exactly the
same, bit for bit, as when everything runs on one thread: each track is worked out into a buffer of
its own, and the tracks are then added together in the same order as always (`src/audio/mixer.h`).
A track whose sidechain listens to another waits for that one; return tracks follow the tracks that
send to them. Renders (export, stems, freeze, bounce) use the cores too. Windows plugins gain the
most: each waits for its Wine host, and now they wait at the same time.

- **Options → Audio properties → Processing cores**: Automatic (all of them), 1 (everything on the
  audio thread, as before: try it if a plugin misbehaves), or a number in between. Changing it
  reopens the audio device.
- **Options → CPU per track**: a thin bar along the bottom of each track's row shows how much of one
  core the track takes (the whole width is half a core; it turns yellow, then red, as it grows). The
  Audio CPU monitor's tooltip names the costliest tracks either way. With several cores the tracks'
  shares can add up to more than the Audio CPU figure, which is the share of the audio thread's time.

## Build

Needs a C++17 compiler (g++ or clang++), `make`, and the development files of X11 (with Xext,
Xrandr, Xcursor, Xi), ALSA and PulseAudio (`libpulse-simple`). Nothing is downloaded. JACK is used
when its library (`libjack.so.0`, e.g. PipeWire's) is installed; it isn't needed to build. When
Wine's `winegcc` is installed (e.g. the `wine-staging` or `wine` packages), `make` also builds
`tinydaw-wine-host.exe`, the host for Windows plugins (Wine's own headers aren't needed).

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

**Windows plugins** (through Wine, 64- and 32-bit): Options → Wine prefixes chooses the Wine prefixes
to look in (at first the default one, `$WINEPREFIX` or `~/.wine`). In each, VST3 plugins are found
in `C:\Program Files\Common Files\VST3` (and 32-bit ones in `C:\Program Files (x86)\Common Files\VST3`); VST2 plugins (`.dll`) in the folders chosen in Options →
VST2 plugin folders, which offers their usual folders in each prefix. They are listed as
"… (Windows)" and used like any other plugin: their windows open inside Tiny DAW's plugin windows,
their settings are saved with the project. Each runs in a process of its own,
`tinydaw-wine-host.exe` under Wine in its prefix (Tiny DAW's own VST hosts built as a Winelib
program; audio through shared memory, `src/plugins/bridge.h`), so a Windows plugin that crashes
falls silent instead of taking the program with it. 32-bit plugins (listed as "… (Windows, 32-bit)")
run in `tinydaw-wine-host-32.exe`, built by `make` beside the 64-bit host when winegcc can build
32-bit programs (the `gcc-multilib` and `g++-multilib` packages, and Wine's 32-bit part), in a
64-bit prefix through Wine's WoW64 or in a 32-bit-only one. They are looked for in
`C:\Program Files (x86)` too (VST3 bundles' `Contents\x86-win`, the usual VST2 folders there).

No Terminal is needed to set them up: Options → Wine prefixes → **New prefix…** makes a 64-bit
prefix (named by you, in `~/.local/share/wineprefixes`, as winetricks does; about half a minute) and
adds it, and Options → **Install Windows plugin…** runs a plugin's installer (`.exe`, or `.msi`
through `msiexec`) in a prefix (asked when there are several; with none, it offers to make one),
waits for it, then lists the new plugins it finds. An installer that put VST2 plugins in a usual
folder that isn't searched yet gets that folder offered for Options → VST2 plugin folders.

**Drag and drop** works with Windows plugins' windows both ways. Files a plugin drags out of its window
(the MIDI or audio a drum machine or chord tool makes, through Windows' DoDragDrop) come to Tiny DAW:
the drag goes on under the pointer and drops where the button is let go, e.g. on a track as a clip.
Files dragged onto a plugin's window (from the explorer, a track, or the file manager) are given to it
as Windows does: to the drop target of its window under the pointer, else as WM_DROPFILES; a plugin
that doesn't take them there says so in a tooltip. (Drags of "virtual" files, which a few plugins
make without writing a file, aren't taken over yet: they stay inside Wine.)

Each prefix's plugins run with that prefix's own Wine: a Bottles bottle's runner (from its
`bottle.yml`), the Wine version of the Lutris game whose prefix it is, or one chosen in Options →
Wine prefixes (select the prefix, Wine program…; Automatic goes back). The dialog shows, for the
selected prefix, whether it is 64- or 32-bit and which Wine runs it, with its version (a Wine older
than 11, the one Tiny DAW's host is tested with, is pointed out). A runner that is missing, or can't
run outside Bottles, gives way to the usual Wine, and the dialog says why. A 32-bit-only prefix
(made with `WINEARCH=win32`) runs only 32-bit plugins: loading a 64-bit one from it says so.

Lines like `libEGL warning: egl: failed to create dri2 screen` on the terminal, when a prefix's
Wine starts, come from Wine's own programs in it (`explorer.exe`, `services.exe`…), on computers
whose graphics card runs NVIDIA's own driver; Tiny DAW's host draws through GLX (below) and doesn't
print them. They are harmless.

A Windows plugin that crashed shows "Stopped" (the status bar says so) and can be **reloaded** without
reopening the project: the Reload button on its picture in the effects window (or instrument patch
panel), Reload in its right-click menu there, in the instrument menu (▾ next to 🎹) and the track's
right-click menu, or the Reload button in its own window if that was open. It starts again in a new
Wine host with the settings it last had, its window opens again if it was open, and its automation
goes on. Options → Reload crashed Windows plugins automatically does this by itself, at most 3 times
a minute for a plugin (then it stays stopped, and the status bar says why). The settings it last had
are kept as you go: whenever it gives them (saving, autosave), 2 s after you change something in it,
and when its window closes; a project saved while a plugin is crashed keeps them too.

Their windows are drawn by the graphics card through OpenGL's GLX: before the first host starts in
a prefix, Tiny DAW sets `UseEGL=N` in that prefix's settings for `tinydaw-wine-host.exe` only
(`HKCU\Software\Wine\AppDefaults\tinydaw-wine-host.exe[.so]\X11 Driver`), as Wine 11's default,
EGL, falls back to drawing on the processor with NVIDIA's drivers before 560 under XWayland
(`TINYDAW_WINE_EGL=1` leaves the setting alone). Where the graphics card has a Vulkan driver,
Wine's Direct3D is told to draw with Vulkan (`WINE_D3D_CONFIG=renderer=vulkan`: Direct2D plugins
redraw faster with it), unless `WINE_D3D_CONFIG` or the prefix already chooses a renderer. For plugins drawn with Direct2D (Serum 2, Quanta
2) the host fills in what Wine's Direct2D and DXGI lack (`src/winehost/d2dfix.cpp`;
`TINYDAW_WINE_D2D_AS_IS=1` leaves them as they are): gradients with see-through colours (drawn too
bright and solid), layers (clipping to shapes and masks: Quanta 2's red rectangles) and waiting for
the screen's refresh (JUCE 8 plugins repaint only after it: their windows stood still). Shapes are
filled with a bounded stack: Wine's Direct2D can recurse without end on some (Serum 2's wavetable
view, its WT POS knob turned), and such a shape is left unfilled instead of the plugin crashing
(`TINYDAW_WINE_PATHS_AS_IS=1` leaves that as it is). Those shapes are mostly waveforms with points
exactly in line with one of their sides (an empty multisample display is one flat line): every point
a shape is given is nudged off such lines by a few thousandths of a pixel first, which Wine then fills
(Serum 2 with a drum kit: 0 shapes given up instead of over 800 in ten seconds; they had shown as
nothing, or as slivers fanning out of the display; `TINYDAW_WINE_FILL_AS_IS=1` leaves the points as
they are). Outlines that turn back sharper than Direct2D's mitre limit (a waveform's peaks) end flat
there, as Windows cuts them, instead of shooting spikes. A plugin that crashes anyway ends its host at
once, and Tiny DAW shows it as stopped, ready to be reloaded (above). The edges of their shapes aren't smoothed as on Windows
(Wine's Direct2D doesn't do that yet).

## Test

    TINYDAW_NO_WINDOWS=1 make -j16 test

builds and runs every `tests/test_*.cpp` (each linked against all of `src/` except `main.cpp`)
and the two test plugins in `build/test-plugins/` (a VST3 effect and a VST2 synth, built on Tiny
DAW's own plugin headers). With MinGW installed (`x86_64-w64-mingw32-g++`, e.g. the
`g++-mingw-w64-x86-64` package), `make win-plugins` (part of `make test`) also builds their Windows
versions and a small Windows installer for them in `build/win-test-plugins/` (32-bit ones too with `i686-w64-mingw32-g++`): `test_wine` makes a
new Wine prefix, installs them there with that installer as Options → Install Windows plugin… does,
and tests them through the Wine host like any installed plugin, without needing any commercial
Windows plugin.

`test_parallel` mixes projects with and without worker threads and checks that they sound the same
bit for bit; with `TINYDAW_PARALLEL_FX=<plugin>` and `TINYDAW_PARALLEL_SYNTH=<plugin>` it plays those
plugins (Windows ones too) on the workers instead of the test plugins, and reports whether they do.

Testing aids of the running program (hidden; see `src/ui/test_script.h`):

| variable | what it does |
|---|---|
| `TINYDAW_SCREENSHOT=<file.png>` | saves a picture of the main window after 60 frames (`TINYDAW_SCREENSHOT_FRAMES`) and quits |
| `TINYDAW_SCRIPT=<file>` | drives the program from a script: clicks, keys, typing, menus, tracks, files, plugins, playing, undo, templates, autosave, screenshots |
| `TINYDAW_VST3_PATH=<dir>[:<dir>…]` | more VST3 folders besides the standard ones (e.g. `build/test-plugins`) |
| `TINYDAW_NO_UPDATE_CHECK=1` | doesn't ask GitHub whether there are newer commits |
| `TINYDAW_WINE=<wine>`, `TINYDAW_WINE_HOST=<…/tinydaw-wine-host.exe>` | another Wine, another Wine host |
| `TINYDAW_WINE_TRACE=1` | the Wine host prints which window gets the keys and clicks, slow audio buffers, its corrections, the font families a plugin asks for (found or not) |
| `TINYDAW_WINE_EGL=1` | doesn't switch the Wine host's OpenGL to GLX |
| `TINYDAW_WINE_D2D_AS_IS=1` | doesn't fill in Wine's Direct2D gradients, layers and vertical blank |
| `TINYDAW_WINE_PATHS_AS_IS=1` | fills Direct2D shapes on the plugin's own thread, as Wine does (no bounded stack) |
| `TINYDAW_WINE_FILL_AS_IS=1` | gives Wine's Direct2D shapes' points as the plugin made them (not nudged off exact lines) |
| `TINYDAW_WINE_DUMP_PATHS=<folder>` | writes each shape Wine's Direct2D recursed on there (for `tinydaw-wine-host.exe --path-test-file`) |
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
                     and Vorbis encoders), engine, mixer and its worker threads, patch cables, synth,
                     sampler, MIDI effects, stretch, automation, metronome, loudness, metering, rendering
    src/effects/     the built-in effects
    src/plugins/     VST3 (our own declaration of the interfaces) and VST2 hosting, the plugin
                     library and cache, the out-of-process type probe, Windows plugins through
                     Wine (wine.h, bridge.h, bridged.cpp)
    src/winehost/    the Wine host for Windows plugins (a Winelib program; include/: Tiny DAW's
                     own declaration of the Win32 functions it uses)
    src/importers/   Ableton Live sets (gzip + XML)
    src/ui/toolkit/  the GUI toolkit (widgets, layouts, controls, menus, dialogs, file chooser)
    src/ui/          every window and panel (main_window, timeline, lane_panel, midi_editor, …)
    tests/           unit tests (tests/check.h) and tests/plugins/ (the test plugins' sources)
    tools/           manual checks (toolkit demo, platform check, plugin editor check, the mouse
                     in Windows plugins' windows and how quickly they redraw)

## License

GPL-3.0, see [LICENSE](LICENSE). VST is a registered trademark of Steinberg Media Technologies
GmbH. Tiny DAW hosts VST2 and VST3 plugins through its own declarations of their interfaces
(`src/plugins/vst2.h`, `src/plugins/vst3_api.h`), written for interoperability; it contains no
Steinberg SDK code. Wine is a separate program, which Tiny DAW runs; it contains none of its code.
