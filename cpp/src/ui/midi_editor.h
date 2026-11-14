// The MIDI editor (double-click a MIDI clip): a piano roll with the notes' velocities under it
// (midi_editor.py).
//
//     +------+--------------------------------------------+--+
//     |      | ruler: bars and beats, both cursors        |  |
//     | keys | notes: time across, pitch up               |sb|
//     |      |                                            |  |
//     +------+--------------------------------------------+--+
//     | vel. | one bar per note: its velocity             |  |
//     |      | horizontal scroll bar                      |  |
//     +------+--------------------------------------------+--+
//
// Editing (a change goes into the clip when the mouse is let go, so it is rendered once):
//     click an empty spot      a new note there, one grid step long (drag to make it longer)
//     drag a note              move it in time and pitch (all selected notes together)
//     drag a note's right end  change its length
//     click / Ctrl+click       select / add to the selection; Delete deletes the selected notes
//     Ctrl+D                   duplicate the selected notes right after them (the copies are selected)
//     0 / Switch off           the selected notes stop playing but stay where they are (drawn as an outline;
//                              0 again switches them back on)
//     Ctrl+U / Quantize        the selected notes (none selected: all) start on the grid at this zoom,
//                              keeping their lengths; ▾ for a gentler strength (75 / 50 / 25 %: part
//                              of the way) and for quantizing their ends too
//     right-click a note       delete it
//     Stamp ▾                  a chord: a click places all of it, rooted on the key clicked (dragging lengthens
//                              it); "In the scale" stacks the song's scale in thirds (needs a scale)
//     Shift+drag               a box: the notes it touches are selected (Ctrl+Shift adds them to the selection)
//     Ctrl+drag a note         copies of the selected notes go where they are dropped (Ctrl+click: select)
//     arrows                   the selected notes a grid step earlier / later (Left, Right), a semitone up / down
//                              (Up, Down; folded, a row); Shift+Up / Down an octave, Shift+Left / Right shorter /
//                              longer by a grid step
//     Ctrl+C / Ctrl+X / Ctrl+V copy / cut the selected notes, paste them at the selection cursor (when it is over
//                              the clip, else after its last note); from one clip to another too
//     velocity lane            click or drag across the bars to set velocities
//     keys on the left         press to play a pitch; drag over them to play every key passed
//     wheel / Shift+wheel / Ctrl+wheel / Alt+wheel: scroll up and down / sideways / zoom / key height
//     ⤢ Expand                 the editor fills the screen (Keys − / + change the key height)
//     Ghosts ▾                 other clips' notes, faint behind this clip's (as they play: loops repeated,
//                              MIDI effects applied): every track's, this track's, or none (remembered).
//                              Right-click one for its clip's editor
//     Scale (key, scale)       the song's key and scale (saved with the project, under "timing"): the
//                              rows of notes outside it are shaded, its root rows tinted. Fold ▾ shows
//                              only the scale's rows (or only the pitches the clip uses: drums), so
//                              every row is a right one; Fit to scale moves the selected notes (none:
//                              all) to the nearest note of the scale
// The ruler shows the timeline's two cursors, as on the timeline: the selection cursor (blue,
// where Play starts) and the playback cursor (red, what is heard). Click or drag on the ruler to
// move the selection cursor (while playing, playback jumps there too).
// Notes sound through the track itself: its VST instrument (or the built-in synth or sampler), its MIDI
// effects, effects and volume (audio/live.h). A pressed key, a new note or a grabbed note sounds while
// the mouse is held; moving a note to another pitch plays the pitches it passes; a note moved with the
// arrow keys sounds briefly.
// In bars and beats, times snap to the project's grid at this zoom (hold Alt to place freely).
//
// Note times count from the start of the clip's notes, which is (clip.start - clip.offset) on the
// timeline. The part outside the clip's window (after splitting it) is shaded: those notes don't play.
// A looping clip shows a dashed line where each pass of its notes ends, and the later passes faintly;
// a note written past the loop lengthens it by whole bars.
//
// Rows: the roll shows rows() top to bottom, all 128 pitches or, folded, some of them; rowAt / pitchAt /
// yOf convert between rows, pitches and y. A note on a hidden pitch (folded away) isn't drawn.
//
// It is a normal (non-modal) window: the program keeps playing and editing while it is open, and the
// transport's application-wide shortcuts (Space, Home…) work in it.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "audio/engine.h"
#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

namespace midi_editor {
constexpr int KEYS_W = 56;          // px: the keyboard on the left (and the velocity lane's label)
constexpr int ROW_H = 12;           // px per pitch, to start with
constexpr int MIN_ROW_H = 6, MAX_ROW_H = 32;  // the key height can be changed between these (Keys − / +, Alt+wheel)
constexpr int VELOCITY_H = 90;      // px: the velocity lane
constexpr int RULER_H = 22;         // px: the editor's ruler
constexpr int CURSOR_MS = 33;       // how often the cursors are checked for having moved
constexpr int EDGE_PX = 5;          // px: grabbing this close to a note's right end changes its length (narrow: less)
constexpr double NOTE_MIN_PX = 3.0; // a note is drawn (and grabbed) at least this wide
constexpr int DEFAULT_VELOCITY = 100;
constexpr double GLIDE_S = 0.12;    // a key passed while dragging over the keys sounds this long
constexpr double NOTE_SOUND_S = 0.25;  // a note moved with the arrows sounds this long
constexpr int GHOST_ALPHA = 120;    // of 255: how strongly other clips' notes show behind this clip's
constexpr int ROOT_TINT_ALPHA = 22; // of 255: the scale's root rows, tinted in the selected-note colour
bool isBlackKey(int pitch);         // pitch % 12 in {1, 3, 6, 8, 10}
std::string noteName(int pitch);    // 60 = "C4"

// notes copied (Ctrl+C): times from the first one's start; shared by every editor (copy in one, paste in another)
struct CopiedNote {
    double start, end, pitch, velocity;
    bool off;  // switched off
};
extern std::vector<CopiedNote> noteClipboard;

// (mode, menu text): "all" every track's notes, "track" this track's other clips, "off" none
const std::vector<std::pair<std::string, std::string>>& ghostModes();
// the chord stamp's choices: "Off", the Chord effect's chords (not "Octave down"), "In the scale: triad" / "…: 7th"
const std::vector<std::string>& stamps();
// (fold, menu text): "off" every key, "scale" the scale's rows, "used" the pitches the notes use
const std::vector<std::pair<std::string, std::string>>& folds();

// --- the scales and chords of the MIDI effects (audio/midi_effects.h) the editor uses ---
extern const char* const* const KEYS;  // "C" … "B"
// the Chord effect's chords, in their saved order: name, semitones added above the root
const std::vector<std::pair<std::string, std::vector<int>>>& chords();
// SCALES: name, steps from the root
const std::vector<std::pair<std::string, std::vector<int>>>& scales();
using Steps = std::set<int>;  // pitch classes 0 = C … 11 = B
// the pitch classes of `scale` (a SCALES name) on `root` (0…11); empty for an unknown scale
Steps scaleSteps(int root, const std::string& scale);
// the note of the scale nearest to `pitch` (the lower one, when two are as near), within 0…127
int nearestInScale(int pitch, const Steps& steps);
// the chord of `notes` notes stacked in thirds of the scale on the scale note nearest to `root`, lowest first
std::vector<int> inScaleChord(int root, const Steps& steps, int notes = 3);
}  // namespace midi_editor

class MidiEditor;

// The keys (left) and the notes; all its editing goes through the MidiEditor.
class PianoRoll : public ui::Widget {
public:
    explicit PianoRoll(MidiEditor* editor);
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override { return mousePress(e); }
    bool wheel(ui::WheelEvent& e) override;
    void resized() override;
    // the roll takes the editing keys itself (Delete, arrows, 0, Ctrl+A/C/V/X/D/U…), before any shortcut
    bool overridesShortcut(const ui::KeyEvent& e) const override;
private:
    MidiEditor* ed_;
    void paintGrid(Painter& p, int h);
    void paintKeys(Painter& p, int h);
    void paintGhosts(Painter& p, int w);
    void paintRepeats(Painter& p, int w, int h);
};

// A bar per note at its start, as tall as its velocity; click or drag across to set them.
class VelocityLane : public ui::Widget {
public:
    explicit VelocityLane(MidiEditor* editor);
    int velocityAt(int y) const;
    bool setUnder(Point pos);  // set the velocity of the notes whose bar is under (or within 6 px of) the mouse
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override { return mousePress(e); }
    bool wheel(ui::WheelEvent& e) override;
private:
    MidiEditor* ed_;
};

// Bars and beats (or seconds) over the piano roll, with both cursors and their arrows. Click or
// drag to move the selection cursor.
class EditorRuler : public ui::Widget {
public:
    explicit EditorRuler(MidiEditor* editor);
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override { return mousePress(e); }
    bool wheel(ui::WheelEvent& e) override;
private:
    MidiEditor* ed_;
};

// Edits clip->notes (and clip->mutedNotes); onChange() is called after each change that went into the clip.
class MidiEditor : public ui::Window {
public:
    // The timeline whose selection cursor the ruler shows and moves (the Python `timeline`
    // argument). Leave `cursor` empty for none: then only the playback cursor shows.
    struct TimelineLink {
        std::function<int64_t()> cursor;           // timeline.cursor: the selection cursor (frames)
        std::function<void(int64_t)> setCursor;    // timeline.set_cursor(frame)
        std::function<void(int64_t)> seek;         // timeline.seek(frame): playback jumps there
        std::function<void(ClipPtr)> editNotes;    // timeline.edit_notes.emit(clip): open (or raise) its editor
    };

    // parent: the main window (kept above it, not modal). The editor deletes itself when closed
    // (deleteOnClose); `closed` (ui::Window's) is emitted first, so forget it then.
    MidiEditor(ui::Window* parent, ClipPtr clip, Project& project, audio::Engine& engine,
               std::function<void()> onChange, TimelineLink timeline = {});
    ~MidiEditor() override;

    ClipPtr clip;
    Project& project;
    audio::Engine& engine;
    std::function<void()> onChange;
    TimelineLink timeline;

    // the working copy of the notes (the clip's, then its switched-off ones), and indexes into it
    std::vector<Note> notes;
    std::set<int> selected;
    std::set<int> off;        // the switched-off ones (clip->mutedNotes)
    int lastVelocity = midi_editor::DEFAULT_VELOCITY;
    int rowH = midi_editor::ROW_H;
    double pps = 200.0;
    std::optional<int> held;  // the pitch sounding while the mouse is held
    std::vector<int> sounding;  // the pitches it sounds (through the track's MIDI effects)
    std::string ghostMode = "all";  // ghostModes(); remembered ("options/ghost_notes")
    std::string fold = "off";       // folds()
    std::string stamp = "Off";      // stamps(): the chord a click places ("Off": one note)
    std::set<int> used;             // the pitches of the notes, for folding to them (taken at commit, not mid-drag)

    // while the mouse is down
    struct Drag {
        enum Kind { None, Keys, Move, Resize, Velocity, Box, Copy } kind = None;
        Point startPos;
        int index = -1;
        std::map<int, Note> origin;  // the dragged notes as they were at the press
        bool changed = false;
        std::set<int> base;          // Box: the selection it adds to
        Point pos;                   // Box: the corner under the mouse
    } drag;

    PianoRoll* roll;
    VelocityLane* velocity;
    EditorRuler* ruler;
    ui::ScrollBar* hbar;
    ui::ScrollBar* vbar;
    ui::Label* hint;
    ui::Button* expandButton;
    ui::Button* ghostsButton;
    ui::Button* stampButton;
    ui::ComboBox* keyBox;
    ui::ComboBox* scaleBox;
    ui::Button* foldButton;
    ui::Button* fitButton;
    ui::Button* quantizeButton;
    ui::Button* switchButton;
    ui::Action* quantizeEnds;   // "Quantize the notes' ends too"
    std::map<std::string, ui::Action*> ghostActions, stampActions, foldActions;

    // --- time and pitch ---
    int sr() const { return engine.sr; }
    double originS() const;                  // timeline seconds of note time 0
    void windowS(double& from, double& to) const;  // the part of the notes the clip plays (note time)
    double lengthS() const;                  // note time covered by the view's scroll range
    double xOf(double t) const;
    double tAt(double x) const;
    std::optional<midi_editor::Steps> scale() const;      // the song's scale as pitch classes, or none
    const std::vector<int>& rows() const;    // the pitches shown, top to bottom
    int rowAt(double y) const;
    std::optional<double> yOf(double pitch) const;  // the top of pitch's row, or none when it is folded away
    int pitchAt(double y) const;
    std::optional<int> rowOf(int pitch) const;
    // {index: pitch} of the notes `origin` dragged `rowsDown` rows down (negative: up), as far as the rows let
    // the whole selection go; folded, a note goes so many rows from its own (one folded away: as many semitones as
    // the grabbed note)
    std::map<int, double> movedPitches(const std::map<int, Note>& origin, int grabbed, int rowsDown) const;
    int nearestRow(double pitch) const;      // the row of `pitch`, or of the shown pitch nearest to it
    void visibleRows(int height, int& first, int& last) const;  // rows [first, last) in view
    bool snapping(int mods) const;           // the project's grid snaps (bars and beats, snapping on; Alt: the other way)
    double stepS(double at = 0.0) const;     // the grid step at this zoom where note time `at` is, in seconds
    double stepBeats(double at = 0.0) const; // …in beats
    double shifted(double t, double beats) const;  // note time t moved by `beats` beats (through tempo changes)
    // {index: (start, end)} of the notes `origin` dragged by dt seconds (on the grid: by whole grid steps in
    // beats where the grabbed note is); never before the clip's notes start
    std::map<int, std::pair<double, double>> movedTimes(const std::map<int, Note>& origin, int grabbed, double dt,
                                                        int mods) const;
    double snap(double t, int mods, bool floor = false) const;  // note time t on the project's grid
    // the note under (x, y) in the roll: its index (or -1) and whether it is its right end; later notes on top
    int noteAt(double x, double y, bool* atEnd = nullptr) const;

    // --- view ---
    void fit();                              // the whole clip across, its notes (or middle C) up and down
    void updateRanges();
    void zoom(double factor, double x);
    void setRowHeight(int height);           // taller or shorter keys, keeping the pitch in the middle where it is
    void refold(const std::function<void()>& change);  // change which rows show, keeping the middle pitch
    void setExpanded(bool on);               // fill the screen with the editor (or back)
    bool isExpanded() const { return expanded_; }
    void wheelAt(const ui::WheelEvent& e, double x);
    void refresh();

    // --- the cursors ---
    std::optional<double> cursorX() const;   // x of the timeline's selection cursor in the views
    double playheadX() const;                // x of the playback cursor (what is heard)
    // redraw when a cursor moved, or the clips or the song's scale changed elsewhere (every CURSOR_MS)
    void followCursors();
    void paintCursorLines(Painter& p, int height);
    void moveCursor(double x, int mods);     // the ruler was clicked at x

    void updateTitle();

    // --- editing ---
    void commit();                           // put the edited notes into the clip and render it again
    void duplicate(int mods = 0);            // Ctrl+D
    void quantize(double strength = 1.0);    // Ctrl+U / Quantize (▾ 75 / 50 / 25 %)
    void reload();                           // take the clip's notes again (changed elsewhere: undo)
    double wholeBars(double t) const;        // note time t rounded up to whole bars from the notes' start
    void takeUsed();
    void takeMuted();                        // the clip's switched-off notes join the working copy, marked off
    void toggleOff();                        // 0 / Switch off
    void deleteNotes(const std::set<int>& indexes);
    void copyNotes(bool cut = false);        // Ctrl+C / Ctrl+X
    void pasteNotes();                       // Ctrl+V
    std::set<int> notesIn(const RectF& box) const;  // the (shown) notes touching a box of roll pixels
    void startCopyDrag(Point pos, int index);       // Ctrl+drag went past the threshold
    void nudge(int key, bool shift, int mods);      // the arrow keys

    // --- hearing notes, through the track (audio/live.h) ---
    bool builtin() const;                    // the track plays the built-in synth or sampler
    void hold(int pitch);                    // sound it until release() (or another pitch is held)
    void release();
    void glide(int pitch);                   // the held mouse moved to `pitch`: the pitches passed sound briefly

    Color color() const;                     // the clip's fill colour (notes are drawn in it)

    // --- ghost notes ---
    struct Ghost {
        ClipPtr clip;
        std::string color;             // "#rrggbb"
        std::vector<Note> notes;       // start, end in this clip's note time; pitch (velocity unused)
    };
    void setGhostMode(const std::string& mode);
    const std::vector<Ghost>& ghosts();      // other MIDI clips' notes that play (worked out when they changed)
    ClipPtr ghostAt(double x, double y);     // the clip of the ghost note under (x, y), or null
    void editOther(const ClipPtr& other);

    // --- the chord stamp ---
    void setStamp(const std::string& name);
    void syncStamp();
    std::vector<int> stamped(int root) const;  // the pitches a click on `root` places

    // --- the song's scale ---
    void syncScaleTools();                   // show the song's scale and this editor's fold in the toolbar
    void scaleChosen();
    void setFold(const std::string& fold);
    void fitToScale();

    // ui::Window
    bool keyPress(ui::KeyEvent& e) override;
    void resized() override;
    void close() override;
    // (keys go to the computer keyboard first when it plays: computer_keyboard.h)
    void onEvent(plat::Window* w, const plat::Event& e) override;

private:
    int cursorTimer_ = -1;
    std::pair<std::optional<int>, std::optional<int>> cursorXs_;  // (selection, playback) x as last drawn
    std::pair<int, std::string> seenScale_{-1, "\x01"};
    std::vector<int64_t> seenClips_;
    bool refit_ = false;
    bool expanded_ = false;
    Rect normalGeometry_;
    std::vector<int64_t> ghostKey_;
    bool ghostKeyValid_ = false;
    std::vector<Ghost> ghostList_;
    mutable std::vector<int64_t> rowsKey_;
    mutable bool rowsValid_ = false;
    mutable std::vector<int> shown_;
    mutable std::map<int, int> rowOf_;
    std::shared_ptr<int> life_ = std::make_shared<int>(0);  // (timers posted check it is still alive)
    std::shared_ptr<live::LiveNotes> live() const;
    void soundBriefly(int pitch, int velocity, double seconds);
    ui::Button* buildGhostsButton();
    ui::Button* buildStampButton();
    void buildScaleTools(ui::HBox* tools);
};
