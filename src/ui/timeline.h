// The scrolling timeline: ruler, audio and MIDI lanes, clips, selection and the two cursors (timeline.py).
//
// Two cursors, each a line with an arrow on the ruler: the selection cursor (blue, `cursor`) is set
// by clicking or selecting a range; Play always starts from it, and splitting and adding files use
// it. The playback cursor (red, the engine's position) moves while playing and stays where playback
// stopped. Clicking the ruler while playing also makes playback jump there.
//
// Also owns the edits you make with the mouse and keyboard: selecting, moving clips, splitting and
// cutting, seeking, zooming, dropping files, adding and removing tracks. Drawing a clip itself lives
// in clip_painter.h; what is selected in selection.h; the split/cut operations in model/edits.h.
//
// A clip has two zones: its top strip (CLIP_HANDLE_H, with the name) moves it; the rest behaves like
// empty lane space, selecting a time range to split or cut. Its ends (CLIP_EDGE_PX at either side,
// top to bottom) trim it: drag one inwards to shorten the clip, or outwards to bring back what was
// cut off (by splitting, a cut, or a clip placed over it), as far as its audio goes; a MIDI clip can
// be made as long as you like. Snapped to the grid like everything else (Alt: freely); what it then
// covers is replaced, as when a clip is placed. A clip that is moved, dropped or added replaces the
// parts of other clips it covers on its track (edits::clearUnder), so clips on a track never overlap.
// A disabled clip stays where it is, drawn as an outline, but doesn't play (0, or right-click).
// An audio clip has a fade handle at each top corner of its body: drag it inwards to fade in or out
// (double-click it: no fade). Right-click → Clip gain… sets the selected audio clips' own volume, shown
// after the clip's name; the waveform shows both. Right-click → Stretch and pitch… makes them longer or
// shorter at the same pitch, or higher or lower at the same speed (stretch_dialog.h, audio/stretch.h),
// and Reverse plays them backwards. Join clips (Ctrl+J) makes one clip of the selected clips on each
// track (or of what lies in the selected range), as they play. Crossfade… overlaps two selected audio
// clips that meet, one fading out as the other fades in. Loop (right-click): a looping clip repeats
// when its end is dragged further (an audio clip's window becomes its loop; notches show the repeats).
// Ctrl+C / Ctrl+X / Ctrl+V copy, cut and paste clips (clipboard.h); Ctrl+drag a clip's name strip
// drags copies, leaving the clips where they are (a Ctrl+click without dragging still adds or removes
// it from the selection).
// Double-click a MIDI clip to edit its notes (editNotes → the MIDI editor); double-click empty space
// on a MIDI track (or right-click → New MIDI clip) to make a new, empty MIDI clip there.
// Clips are renamed with Ctrl+R, the right-click menu, or a double-click on their name strip.
// The clips of a frozen track (track->frozen) are locked: drawn with an icy tint, they can't be
// selected, moved, cut or dropped onto until the track is unfrozen.
//
// When editing in bars and beats (project.timing) the ruler and grid show bars and beats, and the
// playhead, range edges and moved clips (the start of the clip grabbed) snap to the grid unless Alt
// is held.
//
// A track's automation lanes (Auto on its row) lie under it: each a parameter's curve, from 0 at the
// bottom to 1 at the top (drag its bottom edge to make it taller; double-click the edge for the
// standard height). Click to add a point (dragged at once), drag points (in time they snap like the
// cursor, Alt: freely), double-click or right-click a point to delete it; right-click the lane for
// its menu. A frozen track's lanes are locked, like its clips.
//
// The loop range (project.timing) is a band on the ruler, bright while looping is on; drag its ends
// to change it (they snap like the cursor). The main window hands it to the engine (loopChanged).
//
// Snapping follows the grid (Timing::snaps): the fixed grid's step or the zoom's, and only while
// snapping is on (Ctrl+4); Alt does the opposite for one drag.
//
// Tempo and time-signature changes (timing.h) start at a bar: right-click the ruler → Tempo and time
// signature from bar N…; each is shown after its bar's number ("140 BPM 3/4").
//
// Take lanes (a track's recorded takes, shown with right-click its row → Show takes) lie under it:
// drag across a part of a take to put that part on the track (comping), replacing what is there;
// double-click a take to use all of it; right-click a take lane to use or delete it.
//
// Markers (project.markers: Intro, Chorus…) are flags along the lower half of the ruler. Click one to
// put the selection cursor there (and playback, while playing); drag it to move it (it snaps like the
// cursor); double-click it to rename it. Right-click the ruler to add a marker there or at the
// selection cursor, or to rename or delete the one under the mouse; it also inserts and deletes time
// in the whole song (song_time.h).
//
// A press on a clip, its edge or fade, a marker, the loop's ends or an automation point stays a click
// until the mouse moves DRAG_THRESHOLD_PX: nothing moves (a hand that isn't perfectly still mustn't
// snap an off-grid recording to the grid).
//
// HOW THE MAIN WINDOW USES IT (main_window.py):
//   auto scroll = ...add<ui::ScrollArea>(); scroll->setHorizontalPolicy(AlwaysOn);
//   timeline = scroll->makeContent<Timeline>(project, engine, scroll);   // (or setContent)
//   the master lane: scroll->viewport()->adopt(timeline->takeMasterView()), pinned to the
//     viewport's bottom (MASTER_H tall, the viewport's width), raised over the timeline;
//   call relayout() when the window resizes; setPlayhead(engine.heardPos()) every frame;
//   connect tracksChanged, editNotes, loopChanged, loopRangeChosen, currentTrackChanged, heightsChanged,
//   and the successor's: exportMidi(clips) → midi_export; clipboardChosen("cut"/"copy"/"paste") →
//   clipboard::use + status bar; timeEdited(message) → status bar (6 s; "" = nothing to say);
//   timingChanged → the transport's showTiming and MIDI clips with MIDI effects prepared again (set_timing).
//   Edit → Insert time… (Ctrl+Shift+I) / Delete time (Ctrl+Shift+Del): show insertTime() / deleteTime()'s
//   message. After the sample rate changed: changeSampleRate(old, new).
//   masterBelowView: true when the master lane is pinned under the view rather than over it (the
//   timeline is then as tall as the lanes above the master lane's place).
// The Timeline also re-fits itself when the viewport is resized (it chains the viewport's onEventFilter).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "audio/engine.h"
#include "model/model.h"
#include "ui/lanes.h"
#include "ui/selection.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

class MasterLaneView;

namespace timeline {
constexpr int GROUP_PAD = 6;           // px above and below the miniature clips in a group's header lane
constexpr int LOOP_GRAB_PX = 5;        // how close to an end of the loop band the mouse must be to drag it
constexpr int STEP_LINES_MAX = 16;     // a stepped parameter with up to this many steps gets a line at each value
constexpr int STEP_LABEL_MIN_PX = 22;  // …and each value's name on its line, when the lines are this far apart
constexpr double LOOP_SLACK_S = 2.0;   // a looping audio clip's audio built further than it plays by more than this is built again
constexpr int MARKER_MIN_PX = 4;       // a marker's flag is at least this wide, however close the next one is
constexpr double WIDEST_PX = 16000000; // the zoom keeps the whole song within this many pixels (Qt's widget limit)
}  // namespace timeline

// --- what a mouse drag is doing (timeline.py's MoveDrag, EdgeDrag…) ---
struct MoveDrag {               // dragging the selected clips; positions relative to where the drag began
    int64_t frame = 0;
    int row = 0;
    struct Origin { int64_t start; int row; };
    std::map<ClipPtr, Origin> origin;  // clip -> (start frame, lane index) when the drag began
    int64_t anchor = 0;         // start frame of the clip grabbed: it is the one put on the grid
    int rowsMoved = 0;          // last lane offset that suited every clip's kind
};
enum class ClipSide { None, Left, Right };
struct CopyDrag {               // Ctrl held on a clip's name strip: a click toggles it in the selection; dragging
    ClipPtr clip;               // drags copies of the selection (with it), leaving the clips where they are
    PointF pos;
};
struct EdgeDrag {               // dragging one end of a clip to trim it or lengthen it again
    ClipPtr clip;
    ClipSide side = ClipSide::Right;
};
struct FadeDrag {               // dragging an audio clip's fade handle to set how long it fades
    ClipPtr clip;
    bool in = true;             // its fade-in (else its fade-out)
};
struct TakeDrag {               // dragging across a take lane: the part of the take to use
    Lane lane;
    int64_t frame = 0;          // where the drag began
    int64_t to = 0;             // where the mouse is now
};
struct MarkerDrag {             // dragging a marker along the ruler
    Marker marker;
};
struct HeightDrag {             // dragging a lane's bottom edge to resize its track (or automation lane)
    TrackPtr track;             // one of these two
    AutomationPtr automation;
    double y = 0;               // mouse y when the drag began
    int height = 0;             // height when the drag began
};
struct PointDrag {              // dragging a point of an automation lane
    Lane lane;
    int index = 0;              // the point's place in its curve
};
struct LoopDrag {               // dragging one end of the loop range on the ruler
    bool end = false;           // true: its end, false: its start
};
struct RangeDrag {              // dragging over empty lane space or a clip's body to select a time range
    double x = 0;
    int64_t frame = 0;
    int row = 0;
    ClipPtr clip;               // clip whose body was pressed; a click without dragging selects it
    std::set<ClipPtr> keep;     // clips kept selected (Ctrl held)
    bool active = false;        // moved past DRAG_THRESHOLD_PX
    int endRow = 0;             // lane (among the tracks shown) where the range ends
};
using TimelineDrag = std::variant<std::monostate, MoveDrag, EdgeDrag, HeightDrag, PointDrag, LoopDrag, RangeDrag,
                                  CopyDrag, FadeDrag, TakeDrag, MarkerDrag>;

// an audio clip's fade handle (Timeline::fadeHandleAt)
struct FadeHit {
    ClipPtr clip;               // null: none
    bool in = true;
};

// repaint areas this close side to side are painted together (Timeline::strips)
constexpr int STRIP_GAP_PX = 16;

// the clip zone under the mouse (Timeline::hit)
enum class ClipZone { None, Handle, Body };

class Timeline : public ui::Widget {
public:
    Timeline(Project& project, audio::Engine& engine, ui::ScrollArea* scroll);
    ~Timeline() override;

    // --- signals ---
    ui::Signal<> tracksChanged;          // tracks added or removed; the lane panel rebuilds
    ui::Signal<ClipPtr> editNotes;       // a MIDI clip to open in the MIDI editor (main window)
    ui::Signal<> heightsChanged;         // a lane was resized; the lane panel resizes its rows
    ui::Signal<> loopChanged;            // the loop range was changed here
    ui::Signal<> automationEdited;       // an automation curve was edited here
    ui::Signal<> currentTrackChanged;    // another track was selected (recording records onto it)
    ui::Signal<> loopRangeChosen;        // "Loop this range": looping goes on
    ui::Signal<std::vector<ClipPtr>> exportMidi;   // MIDI clips to export as a MIDI file (→ midi_export)
    ui::Signal<std::string> timeEdited;            // what inserting or deleting time did, for the status bar ("": nothing)
    ui::Signal<std::string> clipboardChosen;       // "copy", "cut" or "paste" chosen in the right-click menu (→ clipboard::use)
    ui::Signal<> timingChanged;                    // the tempo or time signature changed (a tempo change on the ruler)

    // --- state (public as in Python) ---
    Project& project;
    audio::Engine& engine;
    ui::ScrollArea* scroll;
    double pps = config::DEFAULT_PPS;    // zoom, pixels per second
    Selection selection;
    TimelineDrag drag;                   // while a mouse button is down
    int playheadX = 0;                   // x of the playback cursor (the engine's position)
    int64_t cursor = 0;                  // the selection cursor, in frames
    int cursorX = 0;
    int viewLeft = 0;                    // horizontal scroll position, for onHscroll
    bool randomTrackColors = false;      // new tracks get a random colour (Options → Colours)
    TrackPtr currentTrack;               // the selected track: the lane (or panel row) clicked last
    std::optional<std::pair<TrackPtr, int64_t>> recording;  // (track, start frame) while recording: the take grows
    MasterLaneView* masterView = nullptr;  // the main window pins it to the bottom of the view
    bool masterBelowView = false;        // it is pinned under the view (not over it): its place isn't counted in the height
    // hands the master lane widget over to whoever shows it (the main window adopts it into the viewport)
    std::unique_ptr<MasterLaneView> takeMasterView();

    // repaint all of it (also the pinned master lane), or an area
    void update();
    void update(const Rect& area) { Widget::update(area); }
    void onHscroll(int value);

    // --- coordinates ---
    double xOf(int64_t frame) const;
    int64_t frameAt(double x) const;
    // frame at x for the playhead or a range edge: in bars and beats the nearest grid line (Alt: freely)
    int64_t snappedFrameAt(double x, int mods) const;
    std::vector<Lane> lanes() const;                    // below the ruler: groups, tracks, automation, master
    std::map<const Track*, int> trackTops() const;      // {track: y of its lane's top}, for the tracks shown
    std::vector<TrackPtr> visibleTracks() const;        // the tracks shown, top to bottom
    std::optional<Lane> laneAt(double y) const;         // the lane under y (master below all); none over the ruler
    TrackPtr trackAt(double y) const;                   // the track whose lane is under y, or null
    // the track or automation lane whose bottom edge is within LANE_EDGE_GRAB_PX of y (both null: none)
    std::pair<TrackPtr, AutomationPtr> laneEdgeAt(double y) const;
    Rect visibleRect() const;                           // the part shown in the scroll area
    // the clip's box (its time span, inset 2 px in its lane); none while its track is folded away
    std::optional<RectF> clipRect(const Clip& clip, const std::map<const Track*, int>& tops) const;
    std::pair<ClipPtr, ClipZone> hit(PointF pos) const;    // later clips are on top
    std::pair<ClipPtr, ClipSide> edgeAt(PointF pos) const; // an end of a clip (wide enough to have ends)
    FadeHit fadeHandleAt(PointF pos) const;                // the audio clip whose fade handle is under pos
    void dragEdge(PointF pos, int mods);
    void endEdgeDrag();
    // fade the grabbed side of the clip from its edge to the mouse (not snapped): no further than the other fade
    void dragFade(PointF pos);
    // resize to fit all clips (plus room to drag), all lanes and both cursors
    void relayout();
    double endSeconds() const;  // how far the timeline reaches: the last clip's end or either cursor
    double maxPps() const;      // the most the zoom may reach: MAX_PPS, or less for a long song (WIDEST_PX)
    // the selection cursor and range at a new sample rate (the project's clips are converted by the engine)
    void changeSampleRate(int oldSr, int newSr);
    // the rectangles of a repaint region joined into columns where they overlap side to side (or nearly)
    static std::vector<Rect> strips(std::vector<Rect> region);

    // --- tracks ---
    TrackPtr addTrack(Kind kind = Kind::Audio, GroupPtr group = nullptr);
    void tracksRegrouped();     // after groups changed or a track was frozen: forget hidden/locked selections
    GroupPtr groupTracks(const std::vector<TrackPtr>& tracks);
    void groupSelected();       // Ctrl+G
    void setGroup(const TrackPtr& track, const GroupPtr& group);  // null: out of its group
    void ungroup(const GroupPtr& group);
    void toggleFold(const GroupPtr& group);
    void setTrackHeight(Track& track, double height);             // within MIN/MAX_TRACK_H
    void setTrackHeight(Automation& lane, double height);         // within AUTOMATION_MIN/MAX_H
    static int defaultHeight(const Track&) { return config::TRACK_H; }
    static int defaultHeight(const Automation&) { return config::AUTOMATION_H; }
    void removeTrack(const TrackPtr& track);  // asks first when it has clips

    // --- clips ---
    ClipPtr loadClip(const std::string& path);  // a not-yet-placed clip, or null after showing why not
    void placeClip(const ClipPtr& clip, int64_t frame, const TrackPtr& track);
    void addFile(const std::string& path, int64_t frame = 0);  // onto the first empty track of its kind, or a new one
    void deleteSelected();      // Del: cut the range out, or remove the selected clips
    void duplicateSelected();   // Ctrl+D
    void splitSelected();       // S
    void chooseColor();         // C
    // a new, empty MIDI clip (frames 0: a bar in bars and beats, else 2 s); replaces what it covers
    ClipPtr newMidiClip(const TrackPtr& track, int64_t frame, int64_t frames = 0);
    void toggleDisabled();      // 0
    void renameSelected();      // Ctrl+R
    // --- the successor's clip edits ---
    void setGainSelected();     // Clip gain…: ask for the selected audio clips' gain (dB)
    void removeFadesSelected(); // Remove fades
    void stretchSelected();     // Stretch and pitch…: each replaced by a stretched one (replacing what it covers)
    void reverseSelected();     // Reverse (play backwards) / Play forwards again
    void joinSelected();        // Ctrl+J: per track, the selected clips (or what lies in the range) become one
    void crossfadeSelected();   // Crossfade…: two selected audio clips that meet overlap, one fading into the other
    // clips looping or not: a MIDI clip's notes repeat (each pass as long as the clip is now); an audio
    // clip's window as it is now becomes its loop, repeated when it is made longer (model's looped())
    void setLoopNotes(const std::vector<ClipPtr>& clips, bool on);
    // swap clips for new ones ({old: new}) all at once (the audio thread), keeping them selected
    void replaceClips(const std::map<ClipPtr, ClipPtr>& made);
    // MIDI clips that were moved or copied (their track before; their start before, none: a copy) play as
    // their track plays notes now: prepared again for a new track, or at another time when MIDI effects follow the beat
    void rerenderMoved(const std::map<ClipPtr, TrackPtr>& tracksBefore,
                       const std::map<ClipPtr, std::optional<int64_t>>& startsBefore);
    // prepare a clip the audio thread may be playing: made on a copy, its audio swapped in atomically
    void prepareLive(const ClipPtr& clip, int64_t reach = -1);
    // a MIDI file's clip (loadClip keeps its notes in beats) laid on the song's tempo map from `frame`:
    // its notes on the song's beats, the clip as long as the file rounded up to whole bars
    void fromBeats(const ClipPtr& clip, int64_t frame);

    // --- take lanes ---
    void comp(const TrackPtr& track, const ClipPtr& take, int64_t start, int64_t end);  // that part onto the track
    void useTake(const TrackPtr& track, const ClipPtr& take);
    void deleteTake(const TrackPtr& track, const ClipPtr& take);
    void fillTakeMenu(ui::Menu& menu, const Lane& lane);

    // --- markers ---
    // [(marker, its flag on the ruler)], left to right: from its time, as wide as its name, but never past
    // where the next marker's flag starts (its name is then shortened to fit)
    std::vector<std::pair<Marker, Rect>> markerFlags() const;
    std::optional<Marker> markerAt(PointF pos) const;  // the marker whose flag is under pos (the later one)
    void addMarker(int64_t frame);
    void renameMarker(const Marker& marker);
    void replaceMarker(const Marker& marker, const std::optional<Marker>& made);  // none: delete it

    // --- inserting and deleting time in the whole song (song_time.h); they return the status bar's message ---
    std::string insertTime();   // Edit → Insert time…: as long as the selected range, or bars / seconds asked for
    std::string deleteTime();   // Edit → Delete time: the selected range's stretch out of the whole song
    void editTempo(int bar);    // the tempo and time signature from bar `bar` (1: the song's own)
    // the right-click menu of the ruler (markers, tempo changes, inserting and deleting time)
    void fillRulerMenu(ui::Menu& menu, PointF pos);
    void rulerMenu(Point globalPos, PointF pos);
    // the clips' right-click menu, filled (its actions do what they say when triggered)
    void fillMenu(ui::Menu& menu, std::optional<Point> pos);
    void selectAll();           // Ctrl+A
    void clearSelection();      // Esc

    // --- transport and zoom ---
    void seek(int64_t frame);           // the playback cursor (playback continues from there if playing)
    void setPlayhead(int64_t frame);    // draw the playback cursor at frame
    using ui::Widget::setCursor;        // (the mouse pointer shape)
    void setCursor(int64_t frame);      // the selection cursor (where Play starts)
    void moveLine(int& line, int x);    // move a cursor line, repainting only the strips it leaves and enters
    void updateLine(int x);
    void zoom(double factor, double anchorX);  // keeping the time under anchorX in place

    // --- painting ---
    void paint(Painter& p) override;
    void paintArea(Painter& p, const Rect& r);
    // the master lane between y = top and bottom (master_lane draws it there); r in timeline x
    void paintMaster(Painter& p, const Rect& r, int top, int bottom);
    void paintRuler(Painter& p, const Rect& r, bool ruler = true, int line0 = -1, int line1 = -1);

    // --- automation lanes ---
    double automationY(const Lane& lane, double value) const;
    double automationValue(const Lane& lane, double y) const;  // snapped for a stepped parameter
    int pointAt(const Lane& lane, PointF pos) const;           // index of the point near pos, or -1
    std::string valueText(const Automation& a, double value) const;  // as the plugin shows it (else "50 %")
    std::pair<std::string, std::string> rangeLabels(const Automation& a) const;  // texts at 1 and at 0
    void deletePoint(const Lane& lane, int index);
    void removeAutomation(const TrackPtr& track, const AutomationPtr& a);
    void lanesChanged();        // automation lanes added, removed, shown or hidden

    // --- loop range ---
    bool loopXs(int& x0, int& x1) const;  // x of the loop's start and end; false: there is none
    int loopEndAt(PointF pos) const;      // end of the loop under the mouse on the ruler: 1 end, 0 start, -1 none
    void setLoop(int64_t start, int64_t end);  // frames start…end (at least LOOP_MIN_S)

    void setCurrentTrack(const TrackPtr& track);

    // --- menus, mouse, keys, drops ---
    // the right-click menu; pos: where it was asked for on the timeline (none: not over a lane)
    void showMenu(Point globalPos, std::optional<Point> pos = std::nullopt);
    void automationMenu(Point globalPos, const Lane& lane, PointF pos);
    void startMove(PointF pos, const ClipPtr& grabbed);
    void startCopyDrag();            // Ctrl+drag went past the threshold: copies of the selection are dragged
    bool stillAClick(PointF pos);    // the mouse, held down, hasn't moved DRAG_THRESHOLD_PX since the press
    void moveSelected(PointF pos, int mods = 0);
    void selectRange(PointF pos, int mods);
    void updateCursor(PointF pos);   // the pointer shape for what is under it
    // Ctrl+wheel zooms around timeline x; the wheel alone scrolls sideways (delta: Qt units, 120 a notch)
    void wheel(double delta, int mods, double x);
    // files dropped at frame on lane (and the lanes below); no lane or the master lane: new tracks
    void dropPaths(const std::vector<std::string>& paths, int64_t frame, const std::optional<Lane>& lane);

    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override;
    bool contextMenu(ui::MouseEvent& e) override;
    bool wheel(ui::WheelEvent& e) override;
    bool keyPress(ui::KeyEvent& e) override;
    void dragEnter(ui::DragEvent& e) override;
    void drop(ui::DragEvent& e) override;

private:
    std::unique_ptr<MasterLaneView> ownedMaster_;
    ui::ScrollBar* hbar_ = nullptr;
    int hscrollId_ = 0;
    std::shared_ptr<void> guard_;  // (the viewport's resize filter checks the timeline still exists)
    bool masterAlive() const;
    void paintLanes(Painter& p, const Rect& r, const std::vector<Lane>& lanes);
    void paintAutomation(Painter& p, const Rect& r, const Lane& lane, bool background = true);
    void paintGroupContents(Painter& p, const Rect& r, const Lane& lane);
    void paintBars(Painter& p, const Rect& r, bool ruler, int line0, int line1);
    void paintLoop(Painter& p);
    void paintMarkers(Painter& p, const Rect& r);
    void paintTempoChanges(Painter& p, const Rect& r);
    void paintTake(Painter& p, const Rect& r, const Lane& lane, bool background = true);
    std::string editTime(const std::function<bool(std::string& note, std::string& error)>& edit, const std::string& done);
    PointF pressPos_;
    bool pastThreshold_ = true;
    std::map<const Clip*, std::pair<std::vector<Note>, double>> inBeats_;  // loadClip's MIDI files: (notes in beats, beats)
    void paintRecording(Painter& p, const std::map<const Track*, int>& tops);
    void automationPress(ui::MouseEvent& e, const Lane& lane);
    void dragPoint(PointF pos, Point globalPos, int mods);
    void showValueTip(const Lane& lane, double value, Point globalPos);
    ui::Window* dialogParent() const;
    // rangeLabels() worked out once per plugin instance (Python keeps them on the Automation)
    mutable std::map<const Automation*, std::pair<const void*, std::pair<std::string, std::string>>> rangeText_;
};
