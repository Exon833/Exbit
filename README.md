# Tiny DAW

*Goal is to create a fully functional and open-sourced DAW, which is easy to navigate through by anybody.*

Tiny DAW is a digital audio workstation for Linux, written in C++17 with **no third-party
libraries**: its GUI toolkit, 2D renderer and font rasterizer, SVG and PNG, the audio codecs (WAV,
AIFF, FLAC, MP3, Ogg Vorbis — reading and writing), MIDI files, JSON and XML, and VST2/VST3 plugin
hosting are all its own code. It uses the system's X11, ALSA and PulseAudio (and runs on
Wayland desktops through XWayland), each loaded only when there; in the terminal (`tinydaw --tui`), nothing but the C++ runtime (with no sound system it still starts, to edit projects). It began as a Python program, which this one recreates and
extends; that program is in the repository's history.

- Please report any bugs that do appear: what isn't working, how to make it happen, and a fix
  suggestion if you know one.
- Desired features are very welcome!

## What it does

- **Timeline**: audio and MIDI tracks (right-click the track panel, empty space or a row: Add track ▸ audio, MIDI,
  return; or Ctrl+T, Ctrl+Shift+T, Ctrl+Alt+T), groups that are buses (their own fader, pan, sends and effects;
  groups in groups; or Fold only), any track's output into a group or another track, return tracks with sends (before or after the
  fader; a return's row has its fader on its meter: drag the ▼), pan, track delay; tracks duplicated (with or without their clips), moved (drag a name, or
  Alt+↑/↓) and selected several at once (Shift/Ctrl-click: a fader, mute, solo or new effect changes them
  all); clips you
  move, trim, split, duplicate, copy and paste, colour, loop, reverse, join, crossfade, stretch and
  pitch, with gain and fades (linear, smooth, fast, slow or equal power; crossfades equal power) and a
  gain envelope inside an audio clip (right-click it → Gain envelope), before the track's effects; slip
  (Ctrl+Alt-drag a clip: its audio or notes move inside it), edges and fades dragged on every selected clip
  at once, Crop to the selection, Fades at every boundary…; automation that follows moved, copied and
  duplicated clips (Edit → Automation follows clips) and a lane's range copied and pasted;
  takes with comping; markers, tempo and time-signature changes (a tempo lane: steps or ramps, the song kept on its bars by
  the Timebase, its BPM range fixed (Min, Max) or Auto on its row beside it; a time-signature lane under it: a block
  per signature, added by clicking a bar, dragged along the bars, typed or removed; both shown or hidden from the
  View menu or their rows' ✕; any time signature N/D, 6/8, 7/8, 5/16, 3/2, 12/8…, with the ruler, grid and metronome counting its
  unit, MIDI files and plugins told the real signature), warping (audio follows the tempo: markers, Beats / Tones / Complex, its tempo found from the
  file's tags, name or sound),
  insert/delete time, a fixed or zoom-following grid; undo and redo of every kind of edit (the last
  100 steps), with a list of the steps to jump back to (Edit → History…); what the control under the mouse does, in the
  status bar; every command found by typing (Ctrl+Shift+P, Ctrl+P in the terminal) and every key
  rebindable (Options → Keyboard shortcuts…, kept in keymap.txt).
- **MIDI**: a piano roll with ghost notes of other clips, the song's key and scale (Key ▾ in the
  transport: detected from the notes; a clip may have its own; Snap to scale), swing for the song, a
  track or a Steps row (Swing ▾, applied as it plays, committed into the notes on request), chord stamp,
  quantize, box select; a Tools panel previewing strum, arpeggiate, chop, flam, legato, quantize,
  humanize and velocity shapes before applying them; a knife (K), join (Ctrl+J), a pitch's row selected
  (Shift-click its key), overlaps removed; a Steps view of the same clip; notes with their own chance, repeats (rolls),
  slide, pan and fine pitch; linked clips (edit one, every linked copy changes); the built-in synth (two
  oscillators and noise, unison, a filter, two envelopes, an LFO, glide, drift; drag a ⊕ onto a knob to
  modulate it; 36 presets by category), the Drawn synth (nothing to set: draw one cycle of its wave on a canvas,
  and every key plays it), the sampler (Classic, One-shot or Slice; a filter with its envelope, an
  LFO, glide, warp, reverse, normalise) and Pad Kit (a sample per pad, choke groups; any pad on its own track, or Pads to tracks; slice a
  loop onto one by its hits, a grid or by hand, hearing each slice); MIDI effects (arpeggiator, chord, scale, note length, velocity, random, pitch, groove), also on notes
  played live, and committed into the notes (this clip, every clip, or onto a new track); MIDI keyboards (several ports at once: Options → MIDI ports…; a
  track can listen to one port and channel), hardware synths (External synth: notes out, audio back, its delay
  measured), MIDI clock in and out, JACK transport (Sync in the transport); a MIDI keyboard (with sustain pedal) or the computer
  keyboard playing the selected track, or several instruments at once (light the keyboard focus toggle, a MIDI socket,
  on each MIDI track's row; right-click it to Lock keyboard focus so selecting other tracks leaves it); MIDI recording, Capture of what you just played, MIDI file import and export.
- **Modulators**: an LFO, Random, a Level Follower, a Macro knob and MIDI Control (the mod wheel or any
  controller of a MIDI keyboard), placed in the effects window like
  effects, that move any knob of the track's effects or instrument (see below).
- **Effects and plugins**: eighteen built-in effects (EQ with a live spectrum, compressor,
  multi-band compressor, gate, limiter, transient shaper, reverb, Space (a convolution reverb),
  delay, Time FX, chorus, Phaser / Flanger, Tremolo / Pan, Filter Motion, saturation, bit-crusher,
  utility, tuner), Linux VST3 and VST2 plugins,
  Windows VST3 and VST2 plugins through Wine (Tiny DAW's own bridge, no yabridge needed), each
  placed and wired by hand in a patch space with sidechains and a Mix (dry/wet) control;
  instrument patch panels; automation of any parameter, volume and pan; plugin delay compensation.
- **Audio**: ALSA, PulseAudio (PipeWire through either) and JACK; tracks worked out on every
  processor core at once, with each track's CPU use shown; several armed tracks recorded at once (each from its own input), input monitoring
  (In / Auto / Off, a low-latency mode), punch in and out, MIDI round the loop (Merge or Takes); recording with count-in and a
  measurable recording offset; freeze and bounce; render to WAV, FLAC, MP3 or OGG with peak or
  loudness normalizing; stems; level meters with peak and RMS, peak hold and clip lights; LUFS and true
  peak (dBTP) on the master; solo safe (right-click S) and exclusive solo (Alt-click S); automation kept in
  time behind plugins that add delay; a master scope.
- **Presets and MIDI learn**: save and recall an effect's or instrument's settings, or a whole track's
  chain; compare two settings of one effect (Now / Kept); link any knob to a MIDI controller's knob.
- **Projects**: templates, recent projects, backups of what each save replaces, autosave with crash
  recovery, Ableton Live set import; missing samples found by one folder search (File → Find missing samples…);
  Collect and save; a project and its samples in one archive (File → Save as archive…, opened by File →
  Open); samples decoded on every core.
- **Look**: skins written in CSS (Dark, Light, Midnight, OLED, High contrast; 90s, XP and 7, evoking classic
  desktops of those years; Neon District, a neon-noir near-future look; or your own): every colour,
  font, padding, border, corner and size of the interface, edited in your text editor and shown live; interface size
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

## Built-in effects

Every built-in effect is in the plugin chooser of a track's FX window (search "built-in", or a
category), has a Mix (dry / wet) control, can be automated, and sleeps while silence goes through
it. Besides the EQ, compressor, multi-band compressor, gate, limiter, reverb, delay, chorus,
saturation, bit-crusher and utility:

- **Filter Motion** (Filter): a low-pass, high-pass, band-pass or notch filter (12 or 24 dB per
  octave; resonance, drive) whose cutoff moves with a wave (sine, triangle, saws, square, smooth
  random, sample and hold; in Hz or in time with the song, 1/16 to 4 bars; left and right apart by up
  to 180°) and with the loudness of the sound, or of another track through its sidechain. Its window
  draws the filter's curve and where the cutoff is now.
- **Phaser / Flanger** (Modulation): 4, 8 or 12 phasing stages, or a flanger's short sweeping delay;
  rate, depth, feedback (±), centre, stereo spread.
- **Tremolo / Pan** (Modulation): the volume moved by a wave, the two sides together (tremolo) or
  against each other (auto-pan, at 180°), with smoothing for the square wave.
- **Transient Shaper** (Dynamics): more or less of a sound's attack and of what rings after it,
  whatever its level (attack and sustain ±100 %, their times, output, a soft clip), from its own
  sound or a sidechain; a meter shows the gain change.
- **Time FX** (Delay): in time with the song, when Trigger is on (automate it, or switch it in the
  window): **Tape stop** (slows to a halt over 1/8 to 4 bars, and spins back up when Trigger goes
  off), **Stutter** (repeats the last slice, 1/32 to 1 bar, each repeat lower or quieter if you
  like) or **Half speed** (an octave down, caught up every 1, 2 or 4 bars). Every jump is crossfaded.
  With Trigger off the sound passes untouched.
- **Space** (Reverb): a convolution reverb of six made rooms (Small room, Chamber, Hall, Plate,
  Spring, Cathedral) with length, pre-delay, low and high cut, width; no added delay (the first part
  of the room is played directly), and the rooms are built on a thread of their own and crossfaded
  in, so turning the knobs never stops the sound. (Rooms from your own impulse-response files: not
  yet.)
- **Tuner** (Utility): shows the note played, how many cents off and its frequency, with the pitch of
  A (415–466 Hz); the sound passes untouched.
- The **EQ** window shows a live spectrum of the sound behind its curve (Off, before or after the
  EQ), worked out only while the window is open.

## Presets and MIDI controllers

- **An effect's or instrument's preset**: the **Presets ▾** button in a built-in effect's window, or
  right-click a unit in the effects window → Presets (for plugins, whose own window the plugin fills):
  Save as…, the presets saved for that effect, rename or delete them. **Compare**: "Keep this" keeps the
  settings now as Kept; switch between Now and Kept to hear one against the other (a built-in effect's
  window marks the knobs that differ). Loading an effect's preset can't be undone (Compare is the way
  back).
- **A track's preset**: right-click a track → Save track preset… keeps its effects as they are wired,
  with their places and modulation, its instrument (and the built-in sampler's sample) and its MIDI
  effects; Load track preset ▸ puts them on a track (one undo step). Track presets are also in the
  Explorer's Presets place: drag one onto a track, or below the tracks for a new one. A plugin that
  isn't installed here stays as a silent unit keeping its settings, and a message says which.
  Presets are kept in `~/.local/share/TinyDAW/presets`.
- **MIDI learn**: right-click a knob (a built-in effect's, a track's fader or pan, a Macro's) → Link to a
  MIDI controller…, then turn a knob on the controller: from then on it moves that knob (Escape
  cancels). One controller knob can move several. Options → MIDI controller links… lists them, to
  remove some. Links are saved with the project. Moved this way, a knob writes its armed automation
  lane and moves its modulation base like a turn by hand.

## Fades, sends, automation, loop recording, history and hints

- **Fade shapes**: an audio clip's fade-in and fade-out each have a shape: Linear, Smooth (an S-curve),
  Fast, Slow, or Equal power. Right-click the clip → Fade-in shape / Fade-out shape, or right-click a
  fade's handle; the clip draws the curve. Clips overlapping make an equal-power crossfade (the sound
  keeps its loudness through it, where a linear one dips by about 3 dB). Reversing a clip turns its
  fade-in into the matching fade-out.
- **Pre-fader sends**: the **Pre** switch beside a send takes the track's sound for that return before
  its fader and pan (after its effects), so the return gets the same level whatever the fader does: a
  reverb that stays when the dry sound is faded out, a headphone mix. It stays in time with the track's
  delay compensation and delay.
- **Edit → History…**: every undo step by name ("Add track", "Move clips", "Fades"…), the current one
  marked; click one to undo or redo to it. It follows the edits while open.
- **Writing automation**: switch on **Write** on an automation lane's row (or right-click the track →
  "Arm all its automation lanes for writing"; armed lanes are tinted red). While the song plays, moving
  that control writes the lane, from when you take hold of it until you let go (touch): the track's
  fader or pan, a built-in effect's knob, or a plugin's knob in its own window (a run of edits counts as
  one touch, ending 0.4 s after the last). Only the touched stretch is replaced, the points are thinned,
  and each touch is one undo step. On a modulated knob it writes the knob's own place (its base).
- **Curved automation**: Ctrl+drag a segment between two points to bend it (Ctrl+double-click: straight
  again), or right-click it → Curve this segment. Right-click a lane → Draw a shape over the selection
  or the loop: ramps up or down, sine, triangle, saw or square, one cycle a bar or a beat. A bent
  segment keeps its shape when cut by new points, writing, or inserting and deleting time.
- **Loop recording**: with the loop on, recording audio from before the loop's end goes round the loop
  and makes a take of each pass: the last one plays, and every pass is in the track's takes (under it)
  to comp from (Use, or drag across a take lane). Right-click ● Rec → "A take each time round the loop"
  turns it off (recording then plays straight on past the loop). MIDI recording doesn't loop yet.
- **Hints**: rest the mouse on a control for a moment and the status bar says what it does (the first
  line of its tooltip); a message the program shows wins while it lasts. Options → Hints in the status
  bar turns it off.

## Pad Kit

A drum kit made of samples: choose **Pad Kit…** in a MIDI track's instrument menu (▾ next to 🎹). Its window
shows 16 pads (two banks: 32), each a sample on its own key from C2 (MIDI 36) up: its name, waveform and
key, and its choke group's colour. Click a pad to hear it and select it; its settings (tuning, gain,
pan, envelope, one-shot, start and end) are below. Drop sound files from the Explorer or the file
manager onto a pad (several fill the pads from there on), or right-click a pad: Load sample…, Clear,
Choke group ▸ (a pad of a group cuts off the others of the group: an open and a closed hi-hat). The
MIDI editor's keys and Steps rows show the pads' names. It plays live and in clips alike, and is saved
with the project (its samples in the project's audio folder).

**Slice to a Pad Kit**: right-click an audio clip → Slice to a Pad Kit…: the hits it finds are drawn as
lines on the clip, more or fewer with the Sensitivity; Slice makes a MIDI track under it with a Pad Kit
holding a slice per pad (up to 32) and a MIDI clip that plays the slices where they were, so the loop
sounds as before and can be rearranged note by note. The audio clip is switched off (kept, drawn as an
outline; right-click it → Enable clips, or 0, to hear it again); undo puts everything back.

## Note properties and Steps

Besides its velocity, every note has a **chance** (0–100 %: how often it plays), **repeats** (1–8: played
as a roll of so many equal hits across its length), **slide** (its pitch glides from the previous
note's, as an 808 bass does), **pan** and **fine** pitch (±100 cents). In the MIDI editor, the button
at the left of the lane under the notes chooses which one the lane shows and edits: Velocity, Chance,
Repeats, Pan or Fine (drag a bar; with several notes selected, drag one to move them all by as much).
Right-click the lane for the same choice, Slide, and putting a property back to its default for the
selected notes. **Slide** (the toolbar button, or G) slides the selected notes; a sliding note is
drawn with an arrow coming into it, a note's chance as how solid it looks, its repeats as ticks.

Chance is decided the same way every time the song plays or is rendered (from the note's place and
pitch, and the loop's pass), so a render sounds like playback; adding a note doesn't change whether the
others play. Chance and repeats come before the track's MIDI effects (the arpeggiator gets the notes
that play). The built-in synth and sampler glide a sliding note's voice (60 ms), and use pan and fine
pitch; a VST instrument gets a sliding note legato (the next note starts before the last one ends), and
doesn't get pan and fine pitch (they can't be sent per note). Unchanged notes are saved as before.

**Steps** (the toolbar button) shows the same clip as a grid: a row for each pitch it uses (+ Row adds
one), a column for each step of the editor's grid (1/16 when the grid follows the zoom). Click a cell to
add or remove a note one step long, drag to paint or clear, Ctrl+click to select, right-click to delete;
the lane underneath works as in the piano roll (velocity, chance, repeats of each step). Steps writes
ordinary notes into the clip, so MIDI effects, linked copies, freezing and export work as with any.

## Linked clips and Groove

**Linked clips**: Alt+Shift-drag a MIDI clip by its name (or right-click it → **Duplicate as linked**) to make a
copy that shares its notes: editing the notes of any of them (in the MIDI editor, quantize, switching
notes off), its loop length or its loop switch changes every linked copy at once. Each copy keeps its
own place, track, name, colour and trim. A chain mark on the clip's name strip shows it is linked
(its tooltip says with how many). Right-click → **Make unique** makes a copy independent again,
keeping its notes. A plain copy (Ctrl-drag, Ctrl+D) stays independent as before; pasted clips are
independent too, unless they were copied together with their partners (then they are linked among
themselves). Splitting a clip (S) or joining clips makes the pieces unique. Links are saved with the
project.

**Groove** (a MIDI effect: right-click a MIDI track → MIDI effects…): swings the notes on the
off-beats of a grid (1/8, 1/16 or 1/32) later, from straight (50 %) to a heavy shuffle (75 %), on the
song's tempo map; notes between grid lines move in proportion and keep their lengths. Accent makes the
swung notes louder or softer; Humanize moves each note by a little random time (± ms) and velocity,
the same every time the song plays or renders (from a Seed). It changes clips' notes, not notes
played live (as the arpeggiator).

## Modulators

A modulator moves knobs instead of changing the sound: it is placed in a track's effects window like
an effect (Add… → the "Modulators" category, or search "built-in") and drawn with a wave across its
face. Right-click it → **Modulate a knob…** and choose a knob of the track's effects, other
modulators or its instrument (type part of its name: big synths have hundreds). A dashed line goes
from the modulator to the unit it moves, labelled with the knob and how far it moves it. The same
right-click menu lists each knob it moves: **Depth…** (−100 … 100 %: at 100 % its full swing moves
the knob over its whole range), **The other way**, **Remove** (the knob goes back to where it was).

- **LFO**: a wave (sine, triangle, saw up or down, square) in time with the song (1/32 … 8 bars) or
  at a rate in Hz, with a phase; both ways (around the knob's place) or up only.
- **Random**: a new random value every step (in time, or in Hz), jumping or gliding (Smooth); from a
  Seed, so the same song moves the same way every time it plays or is rendered.
- **Level Follower**: how loud what is wired into its input is, or its sidechain (SC): wire another
  track into it (the jacks along the bottom) and, with a negative depth, that track ducks a knob of
  this one: a kick pumping a pad's volume or a filter.
- **Macro**: one knob (0 … 100 %). Wire it to several knobs to turn them together; automate it like
  any parameter.
- **MIDI Control**: the latest value of a controller of the MIDI keyboard (Options → MIDI ports…): the
  mod wheel (CC 1, the default), any other CC 0–127, or the pitch bend (both ways), with a Smooth time.
  Wire it to a filter's cutoff and the mod wheel opens it, on any plugin.

**Pitch bend and the mod wheel**: a MIDI keyboard's pitch bend bends the notes played live into the
selected track (or every track whose keyboard focus toggle is lit): the built-in synth's and sampler's voices (the range, ±2 semitones unless the track's
right-click → Pitch bend range… says otherwise, 1–24, saved with the project), and plugin instruments
(VST2 as MIDI pitch bend; VST3 through the parameter the plugin maps it to; Windows plugins through
their Wine host). The mod wheel (CC 1) goes on to plugin instruments too. Pitch bend isn't recorded
yet (notes are).

The knob keeps its own place: the modulation moves it around that. Turning the knob in its window,
or an automation lane on it, moves that place, and the modulation goes on around the new one. While
the song plays, LFOs and Random follow the song's position, not a clock of their own, so playing from
anywhere, looping and rendering move the knobs exactly alike, and the knobs move every 128 frames of
the song (2.7 ms at 48 kHz) whatever the buffer size. A Windows plugin's knobs move too (its
parameters are sent to its Wine host with the sound). An instrument's knobs move once per buffer
(it plays before the effects). Modulation is saved with the project. (Not yet: modulating a track's
own fader and pan, and modulators on the master.)

## Skins (CSS)

The whole interface is styled by stylesheets written in CSS. `data/skins/default.css` is the program's
own: it holds every colour as a variable of `:root` (`--lane`, `--playhead`, `--meter-hot`… each with a
comment saying what it colours), the shades worked out from them (`--hover: color-mix(…)`), the fonts, the
layout's sizes (`--ruler-height`…), and the rules that draw every control and surface from them. A skin is
a stylesheet put on top of it: it changes only what it names.

```css
/* a skin of your own: a few colours, and a rule */
:root {
    --skin-name: "Night owl";
    --window: #15171c;
    --lane: #1b1e25;
    --playhead: #ff6b81;
    font-family: "Noto Sans";
    font-size: 10pt;
}
PushButton { border-radius: 8px; padding: 5px 16px; }
.toggle-solo { --toggle-color: #2fbf71; }
```

- **Options → Skin** lists the skins: the program's own, then yours (`~/.local/share/TinyDAW/skins`).
- **Edit this skin…** opens the skin's stylesheet in your text editor (the one your desktop opens `.css`
  files with). One of the program's own skins is copied into your skins folder first, under a name you
  choose. **Tiny DAW shows what you save at once**: it watches the file (and `default.css`), and the
  status bar says "Skin … reloaded".
- **Skin problems** lists what Tiny DAW left out of the stylesheet, with the file, line and column
  (`Mine.css:12:5: unknown colour 'blu' (left out)`): like a browser, it skips what it can't read and keeps the
  default there, so a typo never breaks the interface.
- **Import skin…** copies a `.css` skin into your folder; **Export this skin…** saves a copy of it anywhere.
- Skins of earlier versions (JSON) found in your skins folder are turned into stylesheets once, the
  first time the skins are listed; the JSON files are moved into the folder's `old JSON skins`.

Rules match the interface by **type** (`Button`, `PushButton`, `ToolButton`, …), `#name`, **class** (the
program's own marks: `.lit`, `.accent`, `.toggle-mute`, …) and **state** (`:hover`, `:pressed`, `:checked`,
`:disabled`, `:focus`, and each control's own, like a button's `:lit`), with parts (`Button::arrow`). The
CSS understood:

- **Selectors:** type, `*`, `.class`, `#id`, states, `:root`, `::part`; the descendant, child, `+` and `~`
  combinators; `:not()`, `:is()`, `:where()`, `:first-child`, `:last-child`, `:only-child`, `:nth-child()`,
  `:nth-last-child()`; lists, specificity and `!important`.
- **At-rules:** `@import` (shared parts of skins; saving one reloads the skin), `@media` on the window's size
  and the interface size (re-worked out as the window is resized), `@font-face` (a font file shipped with a skin).
- **Colours:** `#hex`, `rgb()`, `hsl()`, names, `transparent`, `currentColor`, `color-mix(in srgb, …)`.
- **Lengths:** `px`, `pt`, `em`, `rem`, `ch`, `vw`, `vh`, `vmin`, `vmax`, `%`, `in`, `cm`, `mm` and `calc()`;
  `var()` with fallbacks.
- **Boxes:** per-side border widths, colours and styles (solid, dashed, dotted, double, inset, outset, groove,
  ridge), four corner radii, `linear-gradient()` and `radial-gradient()` (and repeating ones), `box-shadow`
  (blurred, spread, inset, several), `text-shadow`, `opacity`.
- **Shorthands:** `padding`, `margin`, `border`, `border-radius`, `border-style`, `background`.

**Window frames.** Every Tiny DAW window (the main window, dialogs, effect and plugin windows) has its own
title bar and border, drawn in the skin's look: drag the bar to move the window (the desktop still snaps
and tiles it), double-click it to maximize, right-click it for Minimize / Maximize / Close, drag the
edges to resize. Skins restyle them (`TitleBar`, its buttons, `Window:framed`, `:active` / `:inactive`,
`--title-bar-height`). **Options → Window frames** switches to the system's own frames at once.

**See-through windows.** With Options → Window frames → Transparency (on by default) and a desktop that
composites windows (KWin, Mutter, picom…), Tiny DAW's windows can be see-through where the skin makes them so:
rounded frame corners are smooth against the desktop, menus and tooltips can cast soft shadows, and a skin can use
a translucent `background-color`, `opacity` on a window and `backdrop-filter: blur(…)` (glass, blurred behind on
KWin). Without a compositing manager everything stays opaque, as before.

**Pictures in skins.** Skins can use PNG and SVG images next to them: `url()` layers in `background` (with
`background-repeat`, `-position`, `-size`, `-origin`, `-clip`, and `image-rendering: pixelated` for pixel-exact
art), `-tiny-glyph: url(…)` to replace any control's check mark, arrow or caption-button sign, and `font-smooth:
never` for crisp, unsmoothed text. Saving an image reloads the skin. The skins' own fonts are in
`data/skins/fonts` (Selawik, Wine's Tahoma and Rajdhani, free, with their licences: see [LICENSE.md](LICENSE.md)).

**Classic controls, 1:1.** Skins can draw spin buttons as separate bevelled boxes, a drop-down's arrow inside
its border, tabs where the chosen one is larger and joins its page, joined title-bar button groups (`:first` /
`:last`), a face-coloured ring inside a window's frame (`Window:framed::frame`), a message box's button strip,
dotted tree lines and focus rectangles (`outline`); `calc()` may mix percentages and lengths in backgrounds and
gradient stops, SVG images stretch (`preserveAspectRatio`) and have gradients, and unsmoothed text keeps its
thin strokes. **Options → Skin → Edit this skin…, Import… and Export…** take every file a skin uses (images,
fonts, imported stylesheets) into a folder of its own beside the copy, so a copy works wherever it goes.

**docs/skins.md** lists every variable, type, part, class, state and property.

## Several cores

The tracks of a song are worked out side by side on all of the processor's cores, so a song with
many tracks of plugins doesn't have to fit on one core (the audio thread and a worker for every other
core, real-time threads where the system allows it; `src/audio/workers.h`). It sounds exactly the
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
Xrandr, Xcursor, Xi), ALSA and PulseAudio (`libpulse-simple`). Nothing is downloaded. None of them is
linked: X11's, ALSA's and PulseAudio's libraries are loaded when the window, a sound device or the sound
server is used, so the same program starts without them (in the terminal; without any sound system it says
so, and projects can still be edited). JACK is used when its library
(`libjack.so.0`, e.g. PipeWire's) is installed; it isn't needed to build. When
Wine's `winegcc` is installed (e.g. the `wine-staging` or `wine` packages), `make` also builds
`tinydaw-wine-host.exe`, the host for Windows plugins (Wine's own headers aren't needed).

    make -j16            # ./tinydaw  (release, -O2)
    make DEBUG=1 -j16    # ./tinydaw-debug (-O0 -g, AddressSanitizer + UBSan)
    make appimage        # build/appimage/Tiny-DAW-<YEAR.MONTH.DAY.HOUR.MINUTE.SECOND>-x86_64.AppImage
    make publish         # the same, published as a GitHub release (tag = its version), before a push
    make clean

The AppImage is the whole program in one file that runs on any x86-64 Linux with glibc 2.35 or newer (most
distributions from 2022 on): its program is built in Ubuntu 22.04 (`tools/appimage.Containerfile`, with podman
or docker) with the C++ runtime linked in, and packed by AppImage's `appimagetool` (`tools/appimage.sh`; both
tools must be installed). `NATIVE=1 make appimage` builds it on this machine instead (then it needs this
machine's glibc or newer).

## Run

    ./tinydaw                         # an empty project (or your chosen template)
    ./tinydaw song.json               # open a project
    ./tinydaw drums.wav bass.mid      # add audio and MIDI files (onto tracks of their kind)
    TINYDAW_DEBUG=0 ./tinydaw         # without the diagnostics on the terminal
    ./tinydaw --tui [song.json]       # in the terminal, by the keyboard alone

**In the terminal** (`--tui`): the whole program in a terminal, worked by the keyboard alone, with
nothing but a sound driver (ALSA, or none at all, to edit) installed: no window system, no mouse, no fonts, and any terminal
down to a bare Linux text console (8 colours; ASCII lines when the locale isn't UTF-8). It is laid
out as the window is: the menu bar and transport at the top, the Explorer, the timeline (a row per
track) and the track panel side by side, the master at the bottom, the status bar last. Tab (or F6)
moves the keyboard between the transport, Explorer, timeline and track panel; F10 or Alt+letter
opens the menus (the window's own, plus Quit); F2 the menu of what is selected; F1 lists every key;
Ctrl+Q quits. In the timeline the arrows move the cursor by the grid's step and between tracks,
Enter selects the clip at the cursor, I … O selects a range, `,` `.` `[` `]` move the selected
clips, M adds a marker, E / N open the MIDI editor (N: a new clip), and the window's own keys work as
there (S split, Del, R record, L loop, Ctrl+C/X/V/D, Ctrl+Z…). Up from the first lane is the ruler
(Enter: a tempo change; F2: its menu; Shift+T / Shift+S show the tempo and time-signature lanes as rows under
it, Shift+R types the tempo lane's range, `/` the time signature at the cursor's bar, Shift+Del removes it); an automation lane takes Enter (a point at the cursor), `[`
`]` (lower, raise) and Del; a take lane Enter (use it, or the range selected of it). In the track
panel E opens a track's effects, I its instrument (Shift+I edits it), U its automation, K its takes,
D its sends, A / N a new audio / MIDI track below (F2: Add track, with return tracks too), Shift+K the keyboards
play the MIDI track too (♪ before its name; F2: Lock keyboard focus). The program's other windows appear as text over it, worked by the keys: dialogs (the
file chooser, Render, Audio properties…; Tab between their controls, Enter the default button, Esc
cancels), the effects' patch panel (arrows a unit, Enter its window, B bypass, C / X plug or unplug
a cable, F2 its menu: modulation), a built-in effect's knobs, a VST plugin's parameters as a list,
the Pad Kit's pads, the sampler, the MIDI editor (a keyboard cursor: arrows, Enter puts or selects a
note; the Steps view: Enter lights a pad). Ctrl+W closes the window on top, F12 hides the open ones
to work in the main window. What a text console can't send isn't needed: Ctrl+Shift shortcuts are
in the menus; notes played on the computer keyboard last as long as the key repeats (a console tells
no key being let go). Plugins' own windows and Windows plugins (Wine) need the window system.

Tiny DAW always prints diagnostics on its error output (the terminal, or a file it's sent to): which
font file each font family became, every frame that took longer than one picture of the screen (at
most a line a second, with how many more there were), the audio device opened (its rate, channels,
block size and latency) or why it couldn't be, dropouts while playing (at most a line a second), MIDI
inputs opened, how many plugins were found, and every status bar message and message box. Windows
plugins' Wine host adds its own (`TINYDAW_WINE_TRACE`, below). In the terminal UI they go to
`~/.local/share/TinyDAW/terminal.log` instead, written afresh each run and named when the program
ends (or wherever `2>` sends them). `TINYDAW_DEBUG=0` turns them all off.

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
64-bit prefix through Wine's WoW64 or in a 32-bit-only one. Where Wine is built only for WoW64 (no
32-bit Unix part: `winegcc -m32` can't link), it is built with MinGW's `i686-w64-mingw32-g++`
instead, as a plain Windows program whose few Linux calls go to a small Linux library of its own
(in `wine32/` beside it; `src/winehost/unixside.h`). They are looked for in
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
Windows plugin. Every test program runs even when one fails; the end says which failed (and `make`
exits with an error), or `== all tests passed`.

`test_stress` runs the worst cases, each aimed at one thing: 256 tracks (audio with automation, dense
MIDI) played in small blocks while every fader, mute and solo changes and tracks come and go; the main
window resized 400 times from 1 x 1 to 4000 x 2500 while the interface size changes; every slider and
toggle used in every frame while playing and dragging, then all undone and redone; and the terminal UI
hit with thousands of keys while its screen is resized. `make hooks` makes git test every push
(`tools/pre-push`; `git push --no-verify` skips it once) with `make test-affected`: only the tests the pushed
commits reach, picked by `tools/affected_tests.sh` from the compiler's include lists, and every test when a change
to the build, the tools or the shipped data could reach any of them. `make test-affected BASE=<commit>` does
the same by hand, and `make test TESTS="build/tests/test_x …"` runs the named ones (a push to the `unstable`
branch, the webpage or a tag runs none).

`test_plugin_restore` saves and restores the settings of a real plugin, [Surge XT](https://surge-synthesizer.github.io/)
1.3.4 (free, GPL-3, by the Surge Synth Team; not shipped with Tiny DAW, and skipped without it): its Linux
build (`surge-xt-linux-1.3.4-pluginsonly.tar.gz`, unpacked in `~/.local/share/tinydaw-test-plugins/linux`, or
`TINYDAW_SURGE=<folder>`), its Windows build under Wine (in the prefix `~/.local/share/tinydaw-test-wine`, or
`TINYDAW_SURGE_WINE=<prefix>`, in `C:\Program Files\Common Files\VST3`), and on Windows the one installed there.

`test_parallel` mixes projects with and without worker threads and checks that they sound the same
bit for bit; with `TINYDAW_PARALLEL_FX=<plugin>` and `TINYDAW_PARALLEL_SYNTH=<plugin>` it plays those
plugins (Windows ones too) on the workers instead of the test plugins, and reports whether they do.
`test_modulation` moves a knob of the test VST3 effect with a Macro; `TINYDAW_MOD_PLUGIN=<plugin>` and
`TINYDAW_MOD_PARAM=<parameter name>` try another plugin's knob instead (e.g. a Windows plugin's).

Testing aids of the running program (hidden; see `src/ui/test_script.h`):

| variable | what it does |
|---|---|
| `TINYDAW_SCREENSHOT=<file.png>` | saves a picture of the main window after 60 frames (`TINYDAW_SCREENSHOT_FRAMES`) and quits |
| `TINYDAW_SCRIPT=<file>` | drives the program from a script: clicks, keys, typing, menus, tracks, files, plugins, playing, undo, templates, autosave, screenshots |
| `TINYDAW_VST3_PATH=<dir>[:<dir>…]` | more VST3 folders besides the standard ones (e.g. `build/test-plugins`) |
| `TINYDAW_NO_UPDATE_CHECK=1` | doesn't ask GitHub whether there are newer commits |
| `TINYDAW_WINE=<wine>`, `TINYDAW_WINE_HOST=<…/tinydaw-wine-host.exe>` | another Wine, another Wine host |
| `TINYDAW_WINE_TRACE=1` | (on with the diagnostics above) the Wine host prints which window gets the keys and clicks, slow audio buffers, its corrections, the font families a plugin asks for (found or not) |
| `TINYDAW_WINE_EGL=1` | doesn't switch the Wine host's OpenGL to GLX |
| `TINYDAW_WINE_D2D_AS_IS=1` | doesn't fill in Wine's Direct2D gradients, layers and vertical blank |
| `TINYDAW_WINE_PATHS_AS_IS=1` | fills Direct2D shapes on the plugin's own thread, as Wine does (no bounded stack) |
| `TINYDAW_WINE_FILL_AS_IS=1` | gives Wine's Direct2D shapes' points as the plugin made them (not nudged off exact lines) |
| `TINYDAW_WINE_DUMP_PATHS=<folder>` | writes each shape Wine's Direct2D recursed on there (for `tinydaw-wine-host.exe --path-test-file`) |
| `XDG_CONFIG_HOME=<dir>`, `XDG_DATA_HOME=<dir>` | settings and data elsewhere, to leave your own alone |

## Folder layout

    Makefile
    data/            icons/ (transport buttons), skins/ (default.css and the program's own skins),
                     templates/ (Recording, Songwriting), fonts/ (the interface's symbols: tools/symbol_fonts.py)
    docs/skins.md    everything a skin can style: variables, types, parts, states, properties
    docs/development/  the notes the program was ported from Python with
    src/main.cpp     start-up
    src/config.h     sizes, limits, defaults
    src/core/        JSON, XML, CSS (the stylesheets' parser and cascade), files and paths, strings, settings
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
                     in Windows plugins' windows and how quickly they redraw); skins_reference.py (writes
                     docs/skins.md's variables from default.css)

## Credits

Tiny DAW is written by Exon, with [Claude Code](https://claude.com/claude-code) (Anthropic's Claude) writing
and testing much of the code under Exon's direction.

## License

GPL-3.0, see [LICENSE](LICENSE); the fonts shipped with the skins, under their own licences, are listed in [LICENSE.md](LICENSE.md). VST is a registered trademark of Steinberg Media Technologies
GmbH. Tiny DAW hosts VST2 and VST3 plugins through its own declarations of their interfaces
(`src/plugins/vst2.h`, `src/plugins/vst3_api.h`), written for interoperability; it contains no
Steinberg SDK code. Wine is a separate program, which Tiny DAW runs; it contains none of its code.
