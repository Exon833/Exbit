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
// after the clip's name; the waveform shows both. Right-click → Warp… makes their audio follow the song's
// tempo (warp_dialog.h, audio/warp.h), or (not warped) longer or shorter at the same pitch, and higher or
// lower at the same speed (audio/stretch.h); Warp from here puts a warp marker there on the nearest bar.
// A warped clip in a lane taller than a strip shows its warp markers (timeline_warp.cpp): drag one along
// (snapping to the grid), double-click a transient (drawn faintly) to make it a marker, right-click a
// marker to remove it. Reverse plays them backwards. Join clips (Ctrl+J) makes one clip of the selected clips on each
// track (or of what lies in the selected range), as they play. Crossfade… overlaps two selected audio
// clips that meet, one fading out as the other fades in. Loop (right-click): a looping clip repeats
// when its end is dragged further (an audio clip's window becomes its loop; notches show the repeats).
// Ctrl+C / Ctrl+X / Ctrl+V copy, cut and paste clips (clipboard.h); Ctrl+drag a clip's name strip
// drags copies, leaving the clips where they are (a Ctrl+click without dragging still adds or removes
// it from the selection).
// Linked MIDI clips (model.h, "linked MIDI clips"): Alt+Shift+drag a MIDI clip's name strip drags LINKED
// copies (audio clips in the selection are copied plainly); the Alt pressed with it doesn't free the drag
// from the grid until it is let go and pressed again. Right-click → Duplicate as linked does the same
// as Ctrl+D with linked copies, and Make unique takes the selected clips out of their links (keeping
// their notes). Editing the notes of one linked clip, its loop switch or its length changes them all
// (links::notesChanged). A linked clip shows a chain mark at the right of its name strip; the tooltip
// there says how many clips share its notes. Splitting a linked clip (S) makes its pieces unique, and so
// does joining (Ctrl+J: a new clip); Ctrl-drag, Ctrl+D and paste make independent copies.
// Right-click an audio clip → Slice to a Pad Kit… cuts it at its hits, on a grid or by hand (the slice
// window, slice_dialog.h; lines on the clip show where the slices start while it is open, following every
// change: slicePreview) and, on Slice, makes a new MIDI track under the clip's with a Pad Kit holding a slice
// per pad and a MIDI clip playing them where the clip had them (makeSlicedKit; audio/padkit.h SLICING). The
// audio clip stays as it was (mute or delete it to hear only the kit).
// Editing every selected clip at once (timeline_slip.cpp, model/clip_edits.h): Ctrl+Alt+drag a clip (anywhere on
// it) SLIPS the selected clips' contents: the audio (or notes) moves inside them while they stay put, by whole
// grid steps (Shift as well: freely); the pointer is a ⇔ and the status bar says "Slip" and how far. Dragging an
// end or a fade handle of a selected clip moves every selected clip's end (or fade) on that side by as much.
// Right-click → Slip contents… asks how far (←/→ by the grid), Crop to the selection cuts the selected clips (and
// every clip on the range's tracks) to the selected range, and Fades at every boundary… gives every selected
// audio clip one fade-in and fade-out (length and shape).
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
// Curved segments (AutoPoint::bend): Ctrl+drag a segment between two points up or down to bow it that
// way (Alt is the grid's: it adds a point off the grid), Ctrl+double-click it to straighten it; the
// lane's right-click menu curves and straightens the segment there too, and draws shapes (ramps, sine,
// triangle, saw, square: a cycle per bar or beat) over the selected range or the loop
// (automation_write.h). A lane armed for writing (Write on its row) is tinted red.
// Shift+drag in a lane selects a time range of its curve (currentAutomation): Ctrl+C / Ctrl+X copy or cut
// it, Ctrl+V pastes at the cursor, Del clears it (clipboard.h). While automation follows clips (Edit menu,
// model/auto_follow.h) moving, copying, pasting and duplicating clips takes their tracks' automation along.
//
// The loop range (project.timing) is a band on the ruler, bright while looping is on; drag its ends
// to change it (they snap like the cursor). The main window hands it to the engine (loopChanged).
//
// Snapping follows the grid (Timing::snaps): the fixed grid's step or the zoom's, and only while
// snapping is on (Ctrl+4); Alt does the opposite for one drag.
//
// Tempo and time-signature changes (timing.h): right-click the ruler → Tempo and time signature from bar N… (a
// step at that bar) or Tempo changes ▸ (the change there, the Timebase); each is shown after its bar's number
// ("140 BPM 3/4"), or, with View → Tempo lane, as a point on the tempo lane under the ruler (timeline_tempo.cpp:
// steps and ramps, at any beat, dragged and typed; see "the tempo lane" below).
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
//
// STYLES: the Timeline is the stylesheets' `Timeline`; its ruler, lanes and clips are nodes under it (Ruler, Lane,
// Clip): timeline_look.h and clip_painter.h list their parts, states and properties. The layout's sizes are :root
// variables: --ruler-height, --group-height, --take-height (lanes.h), --automation-padding, --clip-header-height,
// --clip-edge-width, --fade-handle-size, --link-badge-width / -height, --cursor-arrow-width / -height.
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
#include "model/clip_edits.h"
#include "model/model.h"
#include "model/timebase.h"
#include "ui/clip_painter.h"
#include "ui/lanes.h"
#include "ui/paint_helpers.h"
#include "ui/selection.h"
#include "ui/timeline_look.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

class MasterLaneView;
namespace pad_kit_dialog { class SliceDialog; }

namespace timeline {
constexpr int GROUP_PAD = 6;           // (default: Lane.group's padding) above and below the miniature clips in a group's header lane
// px above and below the curve in an automation lane: --automation-padding (AUTOMATION_PAD)
int automationPad();
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
    bool altLinks = false;      // Alt held since an Alt+Shift press (linked copies): it snaps until Alt is let go
    bool copies = false;        // copies dragged (Ctrl): automation following them is copied, not moved
};
enum class ClipSide { None, Left, Right };
struct CopyDrag {               // Ctrl held on a clip's name strip: a click toggles it in the selection; dragging
    ClipPtr clip;               // drags copies of the selection (with it), leaving the clips where they are
    PointF pos;
    bool linked = false;        // Alt+Shift: the MIDI clips' copies are linked to them (model.h)
};
struct EdgeDrag {               // dragging one end of a clip to trim it or lengthen it again
    ClipPtr clip;
    ClipSide side = ClipSide::Right;
    std::map<ClipPtr, int64_t> origin;  // every selected clip's end on that side when the drag began (all move by as much)
};
struct FadeDrag {               // dragging an audio clip's fade handle to set how long it fades
    ClipPtr clip;
    bool in = true;             // its fade-in (else its fade-out)
    std::map<ClipPtr, int64_t> origin;  // every selected audio clip's fade on that side (frames in its window) then
};
struct SlipDrag {               // Ctrl+Alt+dragging a clip: the selected clips' contents slip (model/clip_edits.h)
    ClipPtr clip;               // the one grabbed (its place gives the grid's step)
    double x = 0;               // mouse x when the drag began
    std::vector<clip_edits::SlipStart> from;  // the clips as they were
    int64_t frames = 0;         // how far they are slipped now (later: positive)
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
struct BendDrag {               // Ctrl+dragging a segment of an automation lane up or down: curving it
    Lane lane;
    int index = 0;              // the segment's first point
    double y = 0;               // mouse y when the drag began
    double bend = 0;            // its bend then
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
struct EnvelopeDrag {           // dragging a point of a clip's gain envelope (model/clip_envelope.h)
    ClipPtr clip;
    int index = 0;              // the point's place in its envelope
    int64_t frame = 0;          // the frame of the clip's data where it was grabbed…
    double t = 0;               // …and its time then (frames of the clip's audio)
};
struct EnvelopeBendDrag {       // Ctrl+dragging a segment of a clip's gain envelope up or down: curving it
    ClipPtr clip;
    int index = 0;              // the segment's first point
    double y = 0;               // mouse y when the drag began
    double bend = 0;            // its bend then
};
struct WarpDrag {               // dragging a warped clip's warp marker along (model/warping.h)
    ClipPtr clip;
    int index = 0;              // the marker's place among its markers
    warp::Markers markers;      // its markers as dragged so far
    double grabBeat = 0;        // the clip beat under the mouse when it was grabbed…
    double beat = 0;            // …and the marker's beat then
};
// a tempo point (timeline_tempo.cpp): the change at (bar, beat into it); bar 1, beat 0: the song's own tempo
struct TempoRef {
    int bar = 1;
    double beat = 0;
    bool song() const { return bar == 1 && beat == 0; }
    bool operator==(const TempoRef& o) const { return bar == o.bar && beat == o.beat; }
};
struct TempoDrag {              // dragging a point of the tempo lane: its place (by the grid) and its tempo
    TempoRef at;                // where it is now
    double lo = 0, hi = 0;      // the lane's BPM range when the drag began (kept while dragging)
    bool bars = false;          // it changes the time signature: it moves by whole bars
    std::shared_ptr<timebase::Before> before;  // the song as the drag began (it follows from there: timebase.h)
    bool moved = false;
};
using TimelineDrag = std::variant<std::monostate, MoveDrag, EdgeDrag, HeightDrag, PointDrag, LoopDrag, RangeDrag,
                                  CopyDrag, FadeDrag, TakeDrag, MarkerDrag, BendDrag, EnvelopeDrag, EnvelopeBendDrag,
                                  SlipDrag, WarpDrag, TempoDrag>;

// an audio clip's fade handle (Timeline::fadeHandleAt)
struct FadeHit {
    ClipPtr clip;               // null: none
    bool in = true;
};

// repaint areas this close side to side are painted together (Timeline::strips)
constexpr int STRIP_GAP_PX = 16;

// the clip zone under the mouse (Timeline::hit)
enum class ClipZone { None, Handle, Body };

// The main window's scroll area of the timeline: a ScrollArea with the id #timeline, its box around the ruler, the
// lanes, the pinned master lane and the scroll bars, all inside its border and padding (a sunken well:
// `ScrollArea#timeline { border: 2px inset … }`). The track panel's rows keep lining up with the lanes: the main
// window moves them down by its top inset (border-top-width + padding-top) and keeps its bottom inset free under the
// panel's master row (LanePanel::fitHeight too).
class TimelineView : public ui::ScrollArea {
public:
    TimelineView() { objectName = "timeline"; }
    // what its border and padding take off at the top and bottom (0 in the default look)
    int insetTop() const;
    int insetBottom() const;
};

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
    AutomationPtr currentAutomation;     // the automation lane clicked last (null: another lane): its range's Ctrl+C / V, Del
    std::vector<std::pair<TrackPtr, int64_t>> recording;  // (track, start frame) of each take while recording: they grow
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
    // the clip's box (its time span, inset 2 px in its lane); none while its track is folded away
    std::optional<RectF> clipRect(const Clip& clip, const std::map<const Track*, int>& tops) const;
    std::pair<ClipPtr, ClipZone> hit(PointF pos) const;    // later clips are on top
    std::pair<ClipPtr, ClipSide> edgeAt(PointF pos) const; // an end of a clip (wide enough to have ends)
    FadeHit fadeHandleAt(PointF pos) const;                // the audio clip whose fade handle is under pos
    void dragEdge(PointF pos, int mods);  // (every selected clip's end on that side by as much: EdgeDrag::origin)
    void trimTo(const ClipPtr& clip, ClipSide side, int64_t frame);  // one clip's end moved to frame (within its limits)
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
    TrackPtr addTrack(Kind kind = Kind::Audio, GroupPtr group = nullptr, TrackPtr after = nullptr);
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
    bool warpNewFiles = false;  // Options → Warp new audio files: loadClip finds their tempo and warps them
    void placeClip(const ClipPtr& clip, int64_t frame, const TrackPtr& track);
    void addFile(const std::string& path, int64_t frame = 0);  // onto the first empty track of its kind, or a new one
    void deleteSelected();      // Del: cut the range out, or remove the selected clips
    void duplicateSelected();   // Ctrl+D (no clip nor range selected: duplicateTracks)
    std::function<void()> duplicateTracks;  // (the lane panel's: the selected tracks duplicated)
    void splitSelected();       // S
    void chooseColor();         // C
    // a new, empty MIDI clip (frames 0: a bar in bars and beats, else 2 s); replaces what it covers
    ClipPtr newMidiClip(const TrackPtr& track, int64_t frame, int64_t frames = 0);
    void toggleDisabled();      // 0
    void renameSelected();      // Ctrl+R
    // --- the successor's clip edits ---
    void setGainSelected();     // Clip gain…: ask for the selected audio clips' gain (dB)
    void removeFadesSelected(); // Remove fades
    // Fade-in / Fade-out shape: the shape of the fade-ins (in) or fade-outs of these audio clips (FADE SHAPES)
    void setFadeShape(const std::vector<ClipPtr>& clips, bool in, FadeShape shape);
    // the shapes as ticked entries, for these audio clips' fade-ins (in) or fade-outs (a fade handle's menu)
    void addFadeShapes(ui::Menu& menu, const std::vector<ClipPtr>& clips, bool in);
    // right-clicking a fade handle: the shapes of that fade alone (false: no handle at pos)
    bool fillFadeMenu(ui::Menu& menu, PointF pos);
    // Fade-in shape ▸ and Fade-out shape ▸ (the clip menu)
    void addFadeShapeMenus(ui::Menu& menu, const std::vector<ClipPtr>& clips);
    // --- warping (timeline_warp.cpp, model/warping.h) ---
    // Warp…: the dialog for the selected audio clips, each replaced by one warped (or stretched) as it says
    // (replacing what it covers); `at`: the frame clicked, for Warp from here (-1: the cursor)
    void warpSelected(int64_t at = -1);
    void warpFromHere(const ClipPtr& clip, int64_t frame);  // a marker at `frame` on the nearest bar
    ClipPtr warpClipAt(PointF pos) const;   // the warped clip whose markers are shown under pos (its body), or null
    int warpMarkerAt(const ClipPtr& clip, PointF pos) const;     // its marker's handle under pos (-1: none)
    int64_t transientAt(const ClipPtr& clip, PointF pos) const;  // its transient's tick under pos (a source frame; -1)
    bool warpPress(ui::MouseEvent& e);      // a marker's handle grabbed (false: none under the mouse)
    void dragWarpMarker(PointF pos, int mods);
    void releaseWarpMarker();
    bool warpDoubleClick(PointF pos);       // a transient made a marker (false: none there)
    bool warpRightClick(PointF pos);        // a marker removed (false: none there)
    void reverseSelected();     // Reverse (play backwards) / Play forwards again
    void joinSelected();        // Ctrl+J: per track, the selected clips (or what lies in the range) become one
    // --- linked MIDI clips (timeline_links.cpp) ---
    void duplicateLinkedSelected();  // Duplicate as linked: as Ctrl+D, the selected MIDI clips' copies linked to them
    void makeUniqueSelected();       // Make unique: the selected clips leave their links, keeping their notes
    // after a clip's notes, loop switch or length changed here: its linked clips get them too
    void linkedNotesChanged(const ClipPtr& clip);
    // the linked clip whose chain mark (at the right of its name strip) is under pos, or null
    ClipPtr linkBadgeAt(PointF pos) const;
    std::string toolTipAt(Point pos) const override;  // a chain mark: how many clips share its notes
    // the project's clips that aren't in `before` (pieces an explicit split made) leave their links
    void splitPiecesUnique(const std::vector<ClipPtr>& before);
    void crossfadeSelected();   // Crossfade…: two selected audio clips that meet overlap, one fading into the other
    // --- editing every selected clip at once: slip, crop, fades (timeline_slip.cpp, model/clip_edits.h) ---
    std::function<void(const std::string& text, int ms)> status;  // a message for the status bar (main window)
    std::vector<ClipPtr> editableSelected() const;  // the selected clips not on frozen tracks, in the project's order
    void slipSelected(int64_t frames);   // their contents `frames` later (negative: earlier), each kept to its audio
    void askSlip();                      // Slip contents…: how far, in beats (or seconds), ←/→ by the grid
    void cropSelected();                 // Crop to the selection: the selected clips (and the range's) cut to the range
    void askFades();                     // Fades at every boundary…: one fade-in and fade-out for every selected audio clip
    // Slip contents…, Crop to the selection, Fades at every boundary… (the clip menu)
    void addClipEditActions(ui::Menu& menu);
    void startSlip(PointF pos, const ClipPtr& clip);  // Ctrl+Alt pressed on a clip
    void dragSlip(PointF pos, int mods);              // …dragged: snapped to the grid's steps (Shift: freely)
    // the grid's step at `frame`, in frames (as the slip moves); `beats`: in beats
    double gridStepAt(int64_t frame, bool beats = false) const;
    std::string slipText(int64_t frames, int64_t at) const;  // "+1.5 beats" / "-0.250 s"
    // --- gain envelopes (timeline_envelope.cpp, model/clip_envelope.h): a volume curve inside an audio clip ---
    void showEnvelopes(const std::vector<ClipPtr>& clips, bool on);  // Gain envelope: its line shown and edited, or not
    void removeEnvelopes(const std::vector<ClipPtr>& clips);         // Remove gain envelope
    void editEnvelopePoints(const ClipPtr& clip);                    // Gain envelope points…: the dialog
    void setEnvelope(const ClipPtr& clip, std::vector<AutoPoint> points);  // replaced whole (none: removed)
    // the clip's body (under its name strip), where its envelope is drawn; none while its track is folded away
    std::optional<RectF> envelopeBody(const Clip& clip) const;
    // the point of the envelope shown on the clip under pos (-1: none); `frame`: the frame of its data it is drawn at
    int envelopePointAt(const ClipPtr& clip, PointF pos, int64_t* frame = nullptr) const;
    int envelopeSegmentAt(const ClipPtr& clip, double x) const;      // the first point of the segment at x, or -1
    // the clip whose envelope is shown and edited under pos (its body), or null
    ClipPtr envelopeClipAt(PointF pos) const;
    bool envelopePress(ui::MouseEvent& e, const ClipPtr& clip);      // false: not taken (a frozen track's clip)
    void dragEnvelope(PointF pos, Point globalPos, int mods);
    void dragEnvelopeBend(PointF pos, Point globalPos);
    // --- slicing an audio clip into a Pad Kit (timeline_slices.cpp) ---
    struct SlicePreview {
        ClipPtr clip;
        std::vector<int64_t> frames;  // where the slices would start: frames into the clip's window
    };
    std::optional<SlicePreview> slicePreview;  // drawn on the clip while the slice window is open
    pad_kit_dialog::SliceDialog* sliceWindow = nullptr;  // the slice window, while it is open
    // Slice to a Pad Kit…: the slice window for an audio clip (it returns at once; the track is made on Slice)
    pad_kit_dialog::SliceDialog* sliceToPadKit(const ClipPtr& clip);
    // a new MIDI track under the clip's: a Pad Kit with the clip's sound cut at `starts` (frames into its window)
    // and a MIDI clip replaying the slices at the clip's place; null if the clip has no sound
    TrackPtr makeSlicedKit(const ClipPtr& clip, const std::vector<int64_t>& starts);
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

    // --- the tempo lane (timeline_tempo.cpp): under the ruler while View → Tempo lane is on ---
    // A point per tempo change (bar 1's at the start), a line between them (flat for a step, sloped for a ramp)
    // and each time signature written at its bar. Click the lane to add a point (dragged at once: its time by
    // the grid, its tempo by the height); drag a point; double-click one to type its tempo, ramp and time
    // signature; right-click one to remove it or switch its ramp; right-click the lane for the Timebase.
    // Every edit is one undo step, with what the timebase moved (timebase.h).
    bool inTempoLane(double y) const;
    void tempoRange(double& lo, double& hi) const;     // the BPM from the lane's bottom to its top
    double tempoY(double bpm) const;
    double tempoBpmAt(double y) const;                // (within MIN_BPM…MAX_BPM)
    // the tempo point near pos on the lane (within AUTOMATION_GRAB_PX), or near `seconds` (within `px` at the
    // zoom: the ruler, the terminal's cursor)
    std::optional<TempoRef> tempoPointAt(PointF pos) const;
    std::optional<TempoRef> tempoPointNear(double seconds, double px) const;
    double tempoSeconds(const TempoRef& at) const;    // where a point is
    void paintTempoLane(Painter& p, const Rect& r, bool contents);  // (false: its box, under the grid)
    bool tempoPress(ui::MouseEvent& e);
    void dragTempo(PointF pos, int mods);
    void endTempoDrag();
    // the menu of a point (none: the lane at `seconds`): Edit…, Ramp to the next change, Remove, Add a tempo
    // change here…, Timebase ▸ Beats / Time
    void fillTempoMenu(ui::Menu& menu, double seconds, std::optional<TempoRef> point);
    void editTempoPoint(const TempoRef& at);         // the dialog (tempo, ramp, time signature)
    void addTempoPoint(double seconds);              // a step at the grid line there, at the tempo there…, then the dialog
    void removeTempoPoint(const TempoRef& at);
    void setTempoRamp(const TempoRef& at, bool on);
    void setTimebase(bool beats);
    // an edit of the tempo map: the song follows it as its timebase says (timebase.h), then timingChanged
    void tempoEdit(const std::function<void()>& edit);
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
    // the master lane's grid and cursors between y = top and bottom (master_lane draws it there, over its own box;
    // `divider`: its top border, drawn again over the grid); r in timeline x
    void paintMaster(Painter& p, const Rect& r, int top, int bottom, const paint::Line& divider = paint::Line{Color(), 0});
    // what the stylesheets give its drawing (read again after they change)
    const TimelineLook& look() const;
    void refreshLook() const;  // (also when its own states changed: once per paint)
    void paintRuler(Painter& p, const Rect& r, bool ruler = true, int line0 = -1, int line1 = -1);

    // --- automation lanes ---
    double automationY(const Lane& lane, double value) const;
    double automationValue(const Lane& lane, double y) const;  // snapped for a stepped parameter
    int pointAt(const Lane& lane, PointF pos) const;           // index of the point near pos, or -1
    int segmentAt(const Lane& lane, double x) const;           // the first point of the segment at x, or -1
    void setBend(const Lane& lane, int index, double bend);    // the segment from point `index`: curved (0: straight)
    // the lane's curve over [start, end] seconds replaced by a shape (automation_write.h)
    void drawShape(const Lane& lane, double start, double end, int shape, bool perBar);
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
    // its entries (a segment's curve, shapes, clearing, removing): what automationMenu shows
    void fillAutomationMenu(ui::Menu& menu, const Lane& lane, PointF pos);
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
    // a track preset (model/presets.h) dropped onto a track's lane (track), or elsewhere (null: a new track);
    // unset: such files are dropped as any other file
    std::function<void(const std::string& path, const TrackPtr& track)> dropPreset;

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
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // (false once gone: the slice window's handlers)
    bool masterAlive() const;
    void paintLanes(Painter& p, const Rect& r, const std::vector<Lane>& lanes);
    void paintAutomation(Painter& p, const Rect& r, const Lane& lane);
    bool automationLocked(const Lane& lane) const;  // its curve can't be edited (frozen; no plugin or parameter)
    void paintGroupContents(Painter& p, const Rect& r, const Lane& lane);
    void paintBars(Painter& p, const Rect& r, bool ruler, int line0, int line1);
    void paintLoop(Painter& p);
    void paintMarkers(Painter& p, const Rect& r);
    void paintTempoChanges(Painter& p, const Rect& r);
    void paintTake(Painter& p, const Rect& r, const Lane& lane);
    std::string editTime(const std::function<bool(std::string& note, std::string& error)>& edit, const std::string& done);
    PointF pressPos_;
    bool pastThreshold_ = true;
    std::map<const Clip*, std::pair<std::vector<Note>, double>> inBeats_;  // loadClip's MIDI files: (notes in beats, beats)
    void paintRecording(Painter& p, const std::map<const Track*, int>& tops);
    void automationPress(ui::MouseEvent& e, const Lane& lane);
    void dragPoint(PointF pos, Point globalPos, int mods);
    void dragBend(PointF pos, Point globalPos);
    void showValueTip(const Lane& lane, double value, Point globalPos);
    ui::Window* dialogParent() const;
    // rangeLabels() worked out once per plugin instance (Python keeps them on the Automation)
    mutable std::map<const Automation*, std::pair<const void*, std::pair<std::string, std::string>>> rangeText_;
    // what the stylesheets give its drawing, kept until they (or its own states) change: look()
    mutable TimelineLook look_;
    mutable LaneLooks laneLooks_{this};
    mutable clip_painter::Looks clipLooks_{this};
    mutable paint::Stamp lookStamp_;
};
