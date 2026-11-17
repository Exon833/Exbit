// Small dialogs of the timeline and the lane panel: QInputDialog.getDouble / getInt (the toolkit has
// getText and getItem), and the ones built on them: a track's delay (lane panel → Track delay…),
// clip gain, crossfade length and the time to insert (timeline).
//
// Tests: set `answer` and the dialogs ask it instead of opening a window: answer(title, value) gets
// the value shown (as text) and returns whether OK was pressed, having changed the value.
#pragma once
#include <functional>
#include <string>

#include "model/model.h"
#include "ui/toolkit/widget.h"

namespace track_dialogs {

extern std::function<bool(const std::string& title, std::string& value)> answer;

// a number between lo and hi with `decimals` decimals; true (and `value`) when OK was pressed
bool getDouble(ui::Window* parent, const std::string& title, const std::string& label, double& value, double lo,
               double hi, int decimals);
bool getInt(ui::Window* parent, const std::string& title, const std::string& label, int& value, int lo, int hi);
bool getText(ui::Window* parent, const std::string& title, const std::string& label, std::string& text);

// Right-click a track's row → Track delay…: how much later (negative: earlier) the track plays, in ms
// (the mixer's delay compensation lines it up). True if it was changed.
bool askTrackDelay(ui::Window* parent, Track& track);

// Right-click a MIDI track's row → Pitch bend range…: how many semitones the bend wheel bends its built-in
// synth or sampler, all the way up or down (1…24; a VST instrument bends by its own setting). True if changed.
bool askBendRange(ui::Window* parent, Track& track);

}  // namespace track_dialogs
