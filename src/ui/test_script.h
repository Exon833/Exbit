// A hidden testing aid: drives the running program from a script, through the toolkit's own event
// paths (the same ones the X events take), and photographs its windows. Not part of the Python
// program; nothing of it runs unless one of these is set:
//
//   EXBIT_SCREENSHOT=<file.png>   after EXBIT_SCREENSHOT_FRAMES frames (default 60) saves a
//                                   picture of the main window and quits (without asking)
//   EXBIT_SCRIPT=<file>           runs the script's commands, one per line ('#' comments):
//
//     wait <ms>                       frames <n>
//     target main | top | tooltip | <part of a window title>   (the window that clicks and shots go to;
//                                     top: the newest popup, else modal, else newest shown window (never
//                                     the tooltip); tooltip: the tooltip shown now, e.g. after
//                                     `move <x> <y>` onto a widget that has one and `wait 1000`; keys
//                                     then go to the active window)
//     (while the script runs, the real pointer's events are dropped: its moves, presses, wheel,
//     entering and leaving don't disturb what the script hovers, so a tooltip stays shown)
//     move <x> <y>                    click <x> <y> [left|right|double]   drag <x0> <y0> <x1> <y1>
//     wheel <x> <y> <notches> [ctrl|shift|alt]
//     key <keys>   e.g. key Ctrl+S, key Space, key Escape (to the window with the keyboard)
//     type <text>
//     shot <file.png>                 the target window (framebuffer; with plugin windows inside: native capture)
//     call <what> [args]              addTrack audio|midi, addFile <path>, play, stop, home, save <path>,
//                                     open <path>, new, midiEditor (of the first MIDI clip), fx <track #>|master,
//                                     addEffect <track #|master> <plugin path> (loads it, places it, wires it in),
//                                     instrument <track #> builtin|<plugin>, editInstrument <track #>, explorer on|off, volume <dB>, seek <s>,
//                                     menu <index> (opens the menu bar's menu under its title, as a click),
//                                     resize <w> <h>, skin <name>,
//                                     action <Menu/Entry[/Entry]> (a menu entry by its path, e.g. Edit/Undo),
//                                     undo, redo, autosave (now, and wait), recover open|discard|later (the
//                                     start's question, answered), template <name>, saveTemplate <name>,
//                                     crashPlugins (every Windows plugin's Wine host killed, as a crash),
//                                     frames own|system (Options → Window frames), maximize, restore,
//                                     minimize, raise (the target window, as its title bar's buttons; to the front)
//   (positions are in the window's logical pixels, whatever the interface size, from the top-left of its
//   inside: its own frame's title bar is at negative y. Dragging a title bar or a window's edge moves or
//   resizes it by the program, not the window manager: the script's pointer isn't the real one)
//     print                           what is open (transparent: see-through), the status bar, tracks and clips (stdout)
//     quit                            closes the program without asking; close: as the title bar's ✕;
//     closewin                        closes the target window (as its ✕)
#pragma once
#include <string>

class MainWindow;

namespace test_script {
// starts the script given by the environment, if any (called by main() after the window is shown)
void startFromEnvironment(MainWindow* window);
}  // namespace test_script
