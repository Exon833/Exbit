// The signed-area coverage accumulator (see ttf_raster.h).
#include "gfx/ttf_raster.h"

#include <algorithm>
#include <cmath>

namespace raster {

void Accumulator::begin(int w, int h) {
    w_ = std::max(0, w);
    h_ = std::max(0, h);
    stride_ = w_ + 2;
    size_t need = (size_t)stride_ * h_;
    if (cells_.size() < need) cells_.resize(need, 0.f);  // new cells are zero, used ones were cleared
    if ((int)rowMin_.size() < h_) {
        rowMin_.resize(h_, 1 << 30);
        rowMax_.resize(h_, -1);
    }
    if ((int)covRow_.size() < stride_) covRow_.resize(stride_);
}

void Accumulator::line(float x0, float y0, float x1, float y1) {
    if (y0 == y1 || !(std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1))) return;
    // clip vertically to [0, h]
    float H = (float)h_;
    if ((y0 <= 0 && y1 <= 0) || (y0 >= H && y1 >= H)) return;
    float dxdy = (x1 - x0) / (y1 - y0);
    if (y0 < 0) { x0 += (0 - y0) * dxdy; y0 = 0; }
    if (y1 < 0) { x1 += (0 - y1) * dxdy; y1 = 0; }
    if (y0 > H) { x0 += (H - y0) * dxdy; y0 = H; }
    if (y1 > H) { x1 += (H - y1) * dxdy; y1 = H; }
    if (y0 == y1) return;
    // horizontally: left of the region -> along its left edge; right of it -> nothing
    float W = (float)w_;
    if (x0 >= W && x1 >= W) return;
    if (x0 <= 0 && x1 <= 0) { rawLine(0, y0, 0, y1); return; }
    if ((x0 < 0) != (x1 < 0)) {
        float ym = y0 + (0 - x0) * (y1 - y0) / (x1 - x0);
        if (x0 < 0) {
            rawLine(0, y0, 0, ym);
            x0 = 0;
            y0 = ym;
        } else {
            line(x0, y0, 0, ym);
            rawLine(0, ym, 0, y1);
            return;
        }
    }
    if ((x0 > W) != (x1 > W)) {
        float ym = y0 + (W - x0) * (y1 - y0) / (x1 - x0);
        if (x0 > W) { x0 = W; y0 = ym; }
        else { x1 = W; y1 = ym; }
    }
    rawLine(x0, y0, x1, y1);
}

void Accumulator::rawLine(float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;
    float W = (float)w_;
    x0 = std::clamp(x0, 0.f, W);
    x1 = std::clamp(x1, 0.f, W);
    float dir = 1;
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        dir = -1;
    }
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    int ys = std::max(0, (int)y0), ye = std::min(h_, (int)std::ceil(y1));
    for (int y = ys; y < ye; y++) {
        float* row = &cells_[(size_t)y * stride_];
        float dy = std::min((float)(y + 1), y1) - std::max((float)y, y0);
        float xnext = x + dxdy * dy;
        if (y + 1 >= ye) xnext = x1;  // no drift at the end
        float d = dy * dir;
        float a0x = x < xnext ? x : xnext, a1x = x < xnext ? xnext : x;
        a0x = std::clamp(a0x, 0.f, W);
        a1x = std::clamp(a1x, 0.f, W);
        float x0floor = std::floor(a0x);
        int x0i = (int)x0floor;
        float x1ceil = std::ceil(a1x);
        int x1i = (int)x1ceil;
        int lo, hi;
        if (x1i <= x0i + 1) {
            float xmf = 0.5f * (a0x + a1x) - x0floor;
            row[x0i] += d - d * xmf;
            row[x0i + 1] += d * xmf;
            lo = x0i;
            hi = x0i + 1;
        } else {
            float s = 1.f / (a1x - a0x);
            float x0f = a0x - x0floor;
            float a0 = 0.5f * s * (1.f - x0f) * (1.f - x0f);
            float x1f = a1x - x1ceil + 1.f;
            float am = 0.5f * s * x1f * x1f;
            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1.f - a0 - am);
            } else {
                float a1 = s * (1.5f - x0f);
                row[x0i + 1] += d * (a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; xi++) row[xi] += d * s;
                float a2 = a1 + (float)(x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1.f - a2 - am);
            }
            row[x1i] += d * am;
            lo = x0i;
            hi = x1i;
        }
        if (lo < rowMin_[y]) rowMin_[y] = lo;
        if (hi > rowMax_[y]) rowMax_[y] = hi;
        x = xnext;
    }
}

void Accumulator::readMask(uint8_t* mask, bool evenOdd) {
    for (int y = 0; y < h_; y++) {
        uint8_t* m = mask + (size_t)y * w_;
        std::fill_n(m, w_, 0);
        readRow(y, evenOdd, [&](int x, int len, const uint8_t* cov, int solid) {
            if (cov) std::copy(cov, cov + len, m + x);
            else std::fill_n(m + x, len, (uint8_t)solid);
        });
    }
}

}  // namespace raster
