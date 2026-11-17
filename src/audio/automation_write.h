// Writing automation: by moving a control while the song plays, and by drawing shapes. No GUI code.
//
// WRITING BY HAND ("touch"). A lane armed for writing (its Write switch: Automation::armed) follows its
// control while the song plays: from the moment the control is grabbed until it is let go, the curve
// takes the movement (replacing what was there in that time), then it plays its curve again. The
// movements come from the track's fader and pan (the Volume and Pan lanes), a built-in effect's controls
// in its window, and a plugin's own window (TweakWatcher: the parameters it says were changed). A
// plugin's window says nothing about grabbing and letting go, so a run of its edits is one touch that
// ends TOUCH_IDLE_S after the last.
//
// The Recorder works on the GUI thread, with the song time being heard (Engine::heardPos). While a
// lane is touched (Automation::touched) the audio thread leaves its control alone (automation::apply
// skips it; the mixer plays a Volume / Pan lane at touchValue), so playback doesn't pull the control
// from under the hand; a modulated parameter's base is what is written (the user moves the base:
// modulation.h). The curve shows the movement as it is written; when the touch ends, the points it wrote
// are thinned (those on the line through their neighbours go: thin()). One touch is one undo step: the
// main window waits for busy() to be false before noting the change (history.h).
//
// If the song loops back during a touch, what was written so far stays, and the next pass writes on.
//
// SHAPES. drawShape() writes a ramp, a sine, a triangle, a saw or a square over a time range of a lane
// (the selection or the loop), a cycle per bar or per beat (timeline: the lane's right-click menu).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "model/model.h"

namespace automation {

constexpr double TOUCH_IDLE_S = 0.4;       // a plugin window's run of edits ends this long after the last
constexpr double THIN_TOLERANCE = 0.002;   // a written point this close (0…1) to its neighbours' line goes
constexpr double JUMP_S = 0.001;           // a jump in a curve: two points this far apart in time
constexpr int SINE_POINTS = 16;            // points per cycle of a drawn sine

// The lane of the track's own Volume or Pan (null: none, or not armed).
AutomationPtr armedControlLane(const Track& track, const std::string& param);
// The armed lane of `slot`'s parameter `param`, on whichever track has it (null: none).
AutomationPtr armedLane(const Project& project, const PluginSlot* slot, const std::string& param);
// The value a parameter is written with: a modulated one's base, else its value now (-1: unknown).
double writtenValue(const Project& project, const PluginSlot* slot, uint32_t id, const std::string& param);
// Arm (or disarm) all of a track's lanes.
void armAll(Track& track, bool on);
bool anyArmed(const Track& track);

// Douglas-Peucker on values: keeps the first and last points, and every point that is further than
// `tolerance` from the line through the points kept around it.
std::vector<AutoPoint> thin(const std::vector<AutoPoint>& points, double tolerance = THIN_TOLERANCE);
// The curve with the time `run` covers (its first to its last point) replaced by `run`. Outside, the
// curve is as it was (a point JUMP_S before and after the run keeps it there, jumping to the run and
// back); a curve without points just takes the run.
std::vector<AutoPoint> overwrite(const std::vector<AutoPoint>& points, const std::vector<AutoPoint>& run);

class Recorder {
public:
    // the song time being heard (seconds), false while stopped (set by the main window)
    std::function<bool(double&)> songTime;

    // --- with the song time now (songTime) and the clock: what the controls call ---
    void grab(const AutomationPtr& lane, double value);  // a control grabbed (held until release())
    void move(const AutomationPtr& lane, double value);  // its value now (not grabbed: a touch ending after TOUCH_IDLE_S)
    void release(const AutomationPtr& lane);             // let go
    void tick();                                         // every frame: touches idle long enough end; all when stopped

    // --- the same at given times (wall: a steady clock's seconds) ---
    void grabAt(const AutomationPtr& lane, double seconds, double value, double wall);
    void moveAt(const AutomationPtr& lane, double seconds, double value, double wall);
    void releaseAt(const AutomationPtr& lane);
    void tickAt(double wall, bool playing);

    void releaseAll();                     // every touch ends now (stopped, undo, another project)
    bool busy() const { return !touches_.empty(); }
    bool touching(const Automation& lane) const;
    int writes() const { return writes_; }  // bumped whenever a curve was written (the GUI repaints)

private:
    struct Touch {
        AutomationPtr lane;
        std::vector<AutoPoint> before;  // the curve as it was (a loop's earlier passes written in)
        std::vector<AutoPoint> run;     // what this pass wrote: (song time, value)
        bool held = false;              // grabbed: until released (else until TOUCH_IDLE_S without a move)
        double lastWall = 0;
    };
    std::vector<Touch> touches_;
    int writes_ = 0;
    Touch* find(const Automation* lane);
    void add(Touch& t, double seconds, double value);
    void end(size_t index);
};

Recorder& recorder();  // the program's one
double wallClock();    // a steady clock in seconds

// --- shapes ---
enum class Shape { RampUp, RampDown, Sine, Triangle, Saw, Square };
const char* shapeName(Shape shape);
// The curve with [start, end] (seconds) replaced by the shape, from 0 to 1: a ramp once over the range,
// the others a cycle every bar (perBar) or beat, counted from `start` in the song's tempo.
std::vector<AutoPoint> drawShape(const std::vector<AutoPoint>& points, double start, double end, Shape shape,
                                 const Timing& timing, bool perBar);

}  // namespace automation
