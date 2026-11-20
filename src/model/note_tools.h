// The MIDI editor's note tools (Tools ▾, ui/note_tools_panel.h), as pure transforms of a clip's notes: strum,
// arpeggiate, chop, flam, legato, quantize, humanize and velocity shape, and the commands split, join and
// remove overlaps. No GUI code: the editor previews a Result (drawn ghosted, never put in the clip) and
// applies it as one edit (one undo step).
//
// Each works on `targets` (indexes into `notes`: the selected notes, or all of them) and leaves the others as
// they are. Times are note time in seconds (0: the clip's notes start). Nothing ever starts before 0, and no
// note is made shorter than MIN_LEN. Notes starting within CHORD_S of each other are a chord (strum, arpeggiate).
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "audio/synth.h"

namespace note_tools {

constexpr double MIN_LEN = 1e-3;     // s: the shortest note a tool makes
constexpr double CHORD_S = 0.03;     // s: notes starting this close together are one chord
constexpr int MAX_PIECES = 256;      // chop: pieces per note at most
constexpr int MAX_ARP_STEPS = 4096;  // arpeggiate: notes per chord at most

enum class Tool { Strum, Arpeggiate, Chop, Flam, Legato, Quantize, Humanize, VelocityShape };
constexpr int TOOL_COUNT = 8;
const char* toolName(Tool t);  // "Strum" … "Velocity shape"
enum class ArpStyle { Up, Down, UpDown, Random };

struct Params {
    Tool tool = Tool::Strum;
    double curve = 0;          // strum's spacing, velocity shape's bend: -1 … 1 (0: even, straight)
    uint32_t seed = 1;         // arpeggiate (random), humanize
    // strum: each note of a chord this much after the one below it (down: above it)
    double strumS = 0.03;
    bool down = false;
    // arpeggiate: a chord's notes one after another, a note every rateS, gate of it long
    double rateS = 0.125;
    ArpStyle style = ArpStyle::Up;
    double gate = 0.8;
    // chop: into `pieces` equal parts, or (chopStepS > 0) parts that long from the note's start
    int pieces = 2;
    double chopStepS = 0;
    // flam: a grace note flamS before each note, at flamVelocity times its velocity
    double flamS = 0.02;
    double flamVelocity = 0.6;
    // quantize (the editor's own, MidiEditor::quantize): onGrid(t) the grid line nearest t, stepAt(t) the grid
    // step there; part of the way (strength 0…1); the ends too (never shorter than a step)
    std::function<double(double)> onGrid, stepAt;
    double strength = 1;
    bool ends = false;
    // humanize: start moved by up to ±timingS (the length kept), velocity by up to ±velocityRange
    double timingS = 0.01;
    double velocityRange = 10;
    // velocity shape: from the first note's velocity to the last's, along their starts (curve bends it)
    double from = 40, to = 120;
};

struct Result {
    std::vector<Note> notes;  // every note: the others as they were, then what the tool made of the targets
    std::vector<int> from;    // for each, the index in the input of the note it was made from
};

Result apply(const std::vector<Note>& notes, const std::vector<int>& targets, const Params& p);

// the targets crossing `at` (starting before it, ending after it) cut in two there
Result split(const std::vector<Note>& notes, const std::vector<int>& targets, double at);
// targets of the same pitch that touch or overlap become one (the first's velocity and properties)
Result join(const std::vector<Note>& notes, const std::vector<int>& targets);
// targets of the same pitch never overlap: a note ends where the next one starts (one starting at the same
// time as a longer one is removed)
Result removeOverlaps(const std::vector<Note>& notes, const std::vector<int>& targets);
// every target (any pitch) ends where the next later one starts (a chord's notes together); the last keep theirs
Result legato(const std::vector<Note>& notes, const std::vector<int>& targets);
// the indexes of the notes of `pitch`
std::vector<int> row(const std::vector<Note>& notes, int pitch);

}  // namespace note_tools
