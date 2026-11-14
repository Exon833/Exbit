// Internal (gfx only): the anti-aliased coverage rasterizer shared by the Painter and the glyph
// renderer. Exact area coverage per pixel (signed-area accumulation, as font-rs / stb_truetype):
// every line segment adds its signed area and cover to a cell buffer, and a running sum along each
// row gives the winding-weighted coverage of every pixel. Coverage = min(1, |sum|) (non-zero rule
// for shapes whose pieces all wind the same way) or the even-odd fold.
#pragma once
#include <cstdint>
#include <vector>

namespace raster {

class Accumulator {
public:
    // a w x h region; coordinates given to line() are relative to its top-left corner
    void begin(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    // adds the edge (x0,y0) -> (x1,y1); parts outside the region are clipped correctly (parts left
    // of it still cover the pixels to their right)
    void line(float x0, float y0, float x1, float y1);
    // reads out row y (0 <= y < h) as coverage 0..255 by calling span(x, len, coverage-array or
    // nullptr with constant `solid`) for the covered parts, and clears it for the next use.
    // `fn(int x, int len, const uint8_t* cov, int solid)`: cov == nullptr means every pixel of the
    // span has coverage `solid`.
    template <class F>
    void readRow(int y, bool evenOdd, F&& fn);
    // whole region into an 8-bit mask (stride w), clearing the buffer
    void readMask(uint8_t* mask, bool evenOdd = false);

private:
    void rawLine(float x0, float y0, float x1, float y1);
    int w_ = 0, h_ = 0, stride_ = 0;
    std::vector<float> cells_;        // stride_ * h_, kept all-zero between uses
    std::vector<int> rowMin_, rowMax_;
    std::vector<uint8_t> covRow_;
};

inline uint8_t coverageOf(float s, bool evenOdd) {
    float a = s < 0 ? -s : s;
    if (evenOdd) {
        a = a - 2.f * (float)(int)(a * 0.5f);
        if (a > 1.f) a = 2.f - a;
    } else if (a > 1.f) {
        a = 1.f;
    }
    return (uint8_t)(a * 255.f + 0.5f);
}

template <class F>
void Accumulator::readRow(int y, bool evenOdd, F&& fn) {
    int x0 = rowMin_[y], x1 = rowMax_[y];
    if (x0 > x1) return;
    float* c = &cells_[(size_t)y * stride_];
    int end = x1 < w_ - 1 ? x1 : w_ - 1;
    float s = 0;
    int runStart = -1;
    uint8_t* cov = covRow_.data();
    for (int x = x0; x <= end; x++) {
        s += c[x];
        c[x] = 0;
        uint8_t v = coverageOf(s, evenOdd);
        cov[x] = v;
        if (v) {
            if (runStart < 0) runStart = x;
        } else if (runStart >= 0) {
            fn(runStart, x - runStart, cov + runStart, 0);
            runStart = -1;
        }
    }
    if (runStart >= 0) fn(runStart, end + 1 - runStart, cov + runStart, 0);
    for (int x = end + 1; x <= x1 && x < stride_; x++) c[x] = 0;
    // after the last touched cell the coverage stays constant to the region's right edge
    if (end + 1 < w_) {
        uint8_t v = coverageOf(s, evenOdd);
        if (v) fn(end + 1, w_ - end - 1, nullptr, v);
    }
    rowMin_[y] = 1 << 30;
    rowMax_[y] = -1;
}

}  // namespace raster
