// Peak meter with one horizontal bar per output channel in use (top to bottom: lowest output
// first), level_meter.py.
//
// Used on every track row (its output pair), group row (its tracks' outputs) and the master row
// (every output a track plays on). A bar is never thicker than a tenth of the meter's width;
// the bars sit in the middle of the meter's height.
//
// Each bar keeps its peak for HOLD_S (a thin line), then lets it fall. At the meter's right end, the
// loudest peak since Play as a number (dBFS); it turns red, and stays red, once the sound went over
// full scale (CLIP_DB: the master warns from -0.1 dB) until it is clicked or Play is pressed again
// (resetPeaks).
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "ui/toolkit/widget.h"

namespace level_meter {
constexpr double WARN_DB = -12, HOT_DB = -0.1;  // bar turns yellow, then red, above these
constexpr double MAX_BAR_FRACTION = 0.1;        // of the meter's width: the thickest a bar may be
constexpr int GAP = 1;                          // px between bars
constexpr double HOLD_S = 1.5;                  // a peak is held this long before it falls
constexpr int NUMBER_W = 30;                    // px at the right end for the peak since Play (when the meter is
constexpr int NUMBER_MIN_W = 90;                // at least NUMBER_MIN_W wide)
constexpr double CLIP_DB = 0.0;                 // a track's sound over this is clipping (the master's: MASTER_CLIP_DB)
constexpr double MASTER_CLIP_DB = -0.1;
extern double testNow;                          // tests: the clock (seconds) when >= 0; else the monotonic clock
double now();
double dbFraction(double db);                   // -60 dB (MIN_DB) -> 0, 0 dB -> 1
std::string outputLabel(const std::vector<int>& channels);  // {4, 5} -> "outputs 5 and 6", {0} -> "output 1"
// a group's tracks together: per channel the loudest of their peaks (empty: none of them played)
std::vector<float> loudest(const std::vector<const std::vector<float>*>& peaks);
}  // namespace level_meter

class LevelMeter : public ui::Widget {
public:
    explicit LevelMeter(int channels = 2, double clipDb = level_meter::CLIP_DB);
    void setChannels(int channels);                     // the open device has this many outputs
    void showChannels(const std::vector<int>& indexes); // bars for these outputs (0-based) only; none exist: output 1
    // new peaks (one per output of the device; null = silence): bars fall back slowly instead of jumping
    void push(const std::vector<float>* peaks);
    void push(const std::vector<float>& peaks) { push(&peaks); }
    std::vector<std::pair<int, int>> barRects() const;  // (y, thickness) of each bar
    // go on from `old` (the meter this one replaces, its row rebuilt): its loudest peak, clip light and bars
    void takeOver(const LevelMeter& old);
    void resetPeaks();                                  // Play was pressed (or the meter clicked): start again
    int barsW() const;                                  // px of the meter the bars take (the rest: the number)
    const std::vector<double>& held() const { return held_; }
    double loudest() const { return loudest_; }        // the loudest peak since Play (gain)
    bool clipped() const { return clipped_; }           // it went over its clip level since Play (or the last click)
    bool mousePress(ui::MouseEvent& e) override;
    const std::vector<int>& shown() const { return shown_; }
    const std::vector<double>& levels() const { return levels_; }
    int channels() const { return channels_; }
    void paint(Painter& p) override;
    Point sizeHint() const override { return Point(0, 4); }
    Point minimumSizeHint() const override { return Point(0, 4); }

private:
    int channels_ = 2;
    std::vector<int> shown_, wanted_{0, 1};  // wanted: outputs asked for, kept while a device lacks some
    std::vector<double> levels_;
    std::vector<double> held_, heldAt_;  // each bar's held peak, and since when (clock s)
    double clipGain_ = 1.0;
    double loudest_ = 0.0;
    bool clipped_ = false;
    void showTooltip();
};
