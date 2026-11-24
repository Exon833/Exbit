# Licences

## Tiny DAW

Tiny DAW is free software under the **GNU General Public License, version 3** (the full text is in
[LICENSE](LICENSE)). You may use, study, change and share it, and forks are welcome: anything you hand on that is
built from it stays under the same licence, with its source.

Its code is its own: the GUI toolkit, the 2D renderer and font rasterizer, SVG and PNG, the audio codecs, the
effects, the plugin hosts. VST2 and VST3 plugins are hosted through Tiny DAW's own declarations of their
interfaces (`src/plugins/vst2.h`, `src/plugins/vst3_api.h`), written for interoperability; it contains no
Steinberg SDK code. VST is a registered trademark of Steinberg Media Technologies GmbH.

## Third-party content shipped with it

Only fonts, each with its own licence beside it. The skins' fonts are shipped unmodified; details (versions, where
each came from) are in [data/skins/fonts/README.md](data/skins/fonts/README.md). The interface's symbol fonts in
`data/fonts` are subsets (only the glyphs the interface draws: ▾ ✕ ♩ ⟲ ⏮ …, plus ASCII and Latin-1), made by
`tools/symbol_fonts.py`, so the symbols show on a machine without DejaVu or Noto fonts (Windows).

| Files | What | Copyright | Licence |
|---|---|---|---|
| `data/skins/fonts/selawk*.ttf` | Selawik, the 7 skin's font | © 2015 Microsoft Corporation | SIL Open Font License 1.1: [Selawik-LICENSE.txt](data/skins/fonts/Selawik-LICENSE.txt) |
| `data/skins/fonts/tahoma*.ttf` | Wine's Tahoma, the XP and 90s skins' font | © 1993-2022 the Wine project's authors | GNU LGPL 2.1 or later: [Tahoma-LGPL-2.1.txt](data/skins/fonts/Tahoma-LGPL-2.1.txt) |
| `data/skins/fonts/rajdhani*.ttf` | Rajdhani, the Neon District skin's font | © 2014 Indian Type Foundry | SIL Open Font License 1.1: [Rajdhani-LICENSE.txt](data/skins/fonts/Rajdhani-LICENSE.txt) |
| `data/fonts/DejaVuSans-Symbols.ttf` | A subset of DejaVu Sans 2.37, the interface's symbols (first after the interface font) | © 2003 Bitstream, Inc.; © 2006 Tavmjong Bah; DejaVu changes public domain | Bitstream Vera and Arev fonts licence: [DejaVu-LICENSE.txt](data/fonts/DejaVu-LICENSE.txt) |
| `data/fonts/NotoSansSymbols-Symbols.ttf` | A subset of Noto Sans Symbols 2.003, Noto Sans Symbols 2 2.008 and Noto Sans Math 3.000 merged: the symbols DejaVu Sans lacks (⏮ ⏸ ⤓ ⤢ 🔊) | © 2022 The Noto Project Authors; © 2022 Google LLC | SIL Open Font License 1.1: [Noto-OFL.txt](data/fonts/Noto-OFL.txt) |

The fonts are separate works shipped beside the program (an aggregate); their licences, not the GPL, apply to them.
Anything else added to the project from elsewhere is listed here when it is added.

## Used when installed, never shipped

Tiny DAW loads these from the system when they are there, and works without each (see the README):
X11 and its extensions (windows), ALSA (sound devices, MIDI inputs), PulseAudio and JACK (sound servers),
Wine (Windows plugins), `curl` (the update check), and the system's fonts.
