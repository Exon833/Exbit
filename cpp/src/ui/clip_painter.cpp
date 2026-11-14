#include "ui/clip_painter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "config.h"
#include "ui/colors.h"
#include "ui/theme.h"
#include "ui/waveform_cache.h"

namespace clip_painter {

using waveform_cache::TILE_W;

Rect pixels(const RectF& r) {
    // a pixel is filled when its centre (x + 0.5) lies in [left, right)
    int x0 = (int)std::ceil(r.x - 0.5f), x1 = (int)std::ceil(r.right() - 0.5f);
    int y0 = (int)std::ceil(r.y - 0.5f), y1 = (int)std::ceil(r.bottom() - 0.5f);
    return Rect(x0, y0, x1 - x0, y1 - y0);
}

void paintClip(Painter& p, const Clip& clip, const RectF& rect, const Rect& visible, int viewLeft, double pps, int sr,
               bool selected, bool audible, bool frozen, bool handles) {
    std::string color, tone;
    colors::clipColor(clip, color, tone);  // its own, its track's, or its kind's default
    colors::ClipColors cc = colors::clipColors(color, audible, tone);
    if (clip.disabled) {
        // the whole line inside the clip's box
        if (selected) p.strokeRect(rect.adjusted(0.75f, 0.75f, -0.75f, -0.75f), cc.selectedBorder, 2);
        else p.strokeRect(rect.adjusted(0.75f, 0.75f, -0.75f, -0.75f), cc.fill, 1.5f);
        paintName(p, clip, rect, viewLeft, cc.fill);
        return;
    }
    p.fillRect(pixels(rect), cc.fill);
    p.fillRect(pixels(RectF(rect.x, rect.y, rect.w, (float)config::CLIP_HANDLE_H)), cc.handle);
    RectF body = rect.adjusted(0, (float)config::CLIP_HANDLE_H, 0, 0);
    if (body.bottom() >= visible.top() && body.y <= visible.bottom() - 1)  // not just the name strip
        paintContents(p, clip, body, visible, pps, sr, cc.content);
    if (clip.kind == Kind::Audio && !frozen && handles) paintFades(p, clip, body, pps, sr, cc.border);
    if (clip.loopingAudio()) paintLoopStarts(p, clip, rect, visible, pps, sr, cc.border);
    if (frozen) p.fillRect(pixels(rect), theme::c("frozen_tint"));
    if (selected) p.strokeRect(rect, cc.selectedBorder, 2);  // square corners
    else p.strokeRect(rect, cc.border, 1.5f);
    paintName(p, clip, rect, viewLeft, cc.text);
}

void paintLoopStarts(Painter& p, const Clip& clip, const RectF& rect, const Rect& visible, double pps, int sr, Color color) {
    // (in its audio's time: repeats begin at its loop's end, then every pass)
    double endS = clip.loopStart + clip.loopLength;
    double firstS = (double)clip.offset / sr, lastS = (double)(clip.offset + clip.frames()) / sr;
    double seenS = firstS + std::max(0.0, (visible.left() - rect.x) / pps);  // (from the first repeat in view)
    int64_t k = std::max<int64_t>(0, (int64_t)std::floor((seenS - endS) / clip.loopLength));
    for (;; ++k) {
        double atS = endS + k * clip.loopLength;
        if (atS >= lastS) break;
        if (atS > firstS) {
            float x = (float)(rect.x + (atS - firstS) * pps);
            if (visible.left() - 2 <= x && x <= visible.right() - 1 + 2)
                p.drawLine(PointF(x, rect.y), PointF(x, rect.y + config::CLIP_HANDLE_H + 4), color, 2);
        }
    }
}

bool fadeHandles(const Clip& clip, const RectF& body, double pps, int sr, RectF& in, RectF& out) {
    if (body.w < 4 * (config::CLIP_EDGE_PX + FADE_HANDLE_PX)) return false;
    int64_t fadeIn, fadeOut;
    double a, b;
    clip.fadeParts(fadeIn, fadeOut, a, b);
    const double least = config::CLIP_EDGE_PX + 1, centre = body.x + body.w / 2.0;
    double xIn = std::min(std::max(body.x + (double)fadeIn / sr * pps, body.x + least), centre - FADE_HANDLE_PX);
    double xOut = std::max(std::min(body.right() - (double)fadeOut / sr * pps, body.right() - least - FADE_HANDLE_PX), centre);
    in = RectF((float)xIn, body.y + 1, FADE_HANDLE_PX, FADE_HANDLE_PX);
    out = RectF((float)xOut, body.y + 1, FADE_HANDLE_PX, FADE_HANDLE_PX);
    return true;
}

void paintFades(Painter& p, const Clip& clip, const RectF& body, double pps, int sr, Color color) {
    // (a piece of a longer fade: the line starts part-way up, at the level there)
    int64_t fadeIn, fadeOut;
    double fromIn, toOut;
    clip.fadeParts(fadeIn, fadeOut, fromIn, toOut);
    struct Side { int64_t frames; float edge; int sign; double level; };
    for (const Side& s : {Side{fadeIn, body.x, 1, fromIn}, Side{fadeOut, body.right(), -1, toOut}}) {
        if (!s.frames) continue;
        float x = (float)(s.edge + s.sign * (double)s.frames / sr * pps);
        float y = (float)(body.bottom() - s.level * body.h);
        p.fillPolygon({PointF(s.edge, body.y), PointF(x, body.y), PointF(s.edge, y)}, fadeShade());
        p.drawLine(PointF(s.edge, y), PointF(x, body.y), color, 1.2f);
    }
    RectF in, out;
    if (fadeHandles(clip, body, pps, sr, in, out))
        for (const RectF& square : {in, out}) p.fillRect(square, color);
}

std::string clipLabel(const Clip& clip) {
    if (!clip.gainDb) return clip.name;
    char buf[32];
    std::snprintf(buf, sizeof buf, "   %+.1f dB", clip.gainDb);
    return clip.name + buf;
}

void paintName(Painter& p, const Clip& clip, const RectF& rect, int viewLeft, Color color) {
    Font font = theme::uiFont();
    std::string name = clipLabel(clip);
    float textW = font.width(name);
    float x = std::max(rect.x + 6, std::min(std::max(rect.x, (float)viewLeft) + 6, rect.right() - textW - 6));
    p.save();
    p.setClip(pixels(rect.adjusted(1, 1, -1, -1)));
    p.drawText(PointF(x, rect.y + 3 + std::round(font.ascent())), name, font, color);
    p.restore();
}

void paintContents(Painter& p, const Clip& clip, const RectF& body, const Rect& visible, double pps, int sr,
                   Color color) {
    int base = (int)std::lround(body.x), top = (int)body.y;
    int total = (int)body.right() - base;  // contents width in pixels
    int height = (int)body.h;
    int first = std::max(0, visible.left() - base);
    int last = std::min(total, visible.left() + visible.w - base);  // exclusive
    if (last <= first || height <= 0) return;
    auto& cache = waveform_cache::tiles();
    const double dpr = p.scale();
    PointF t = p.translation();
    if (dpr == 1.0) {  // 1:1: as logical pixels
        for (int index = first / TILE_W; index <= (last - 1) / TILE_W; ++index) {
            int width = std::min(TILE_W, total - index * TILE_W);
            p.drawImage(Point(base + index * TILE_W, top), cache.tile(clip, index, width, height, pps, sr, color));
        }
        return;
    }
    // in device pixels: the clip's left/top device pixel, its size there, and the visible columns
    const double dx = t.x * dpr, dy = t.y * dpr;
    int left = (int)std::nearbyint(base * dpr + dx), topPx = (int)std::nearbyint(top * dpr + dy);
    int totalPx = (int)std::nearbyint(total * dpr), heightPx = (int)std::nearbyint(height * dpr);  // (tiles stay reusable)
    if (totalPx <= 0 || heightPx <= 0) return;
    int firstPx = std::max(0, (int)std::floor(first * dpr + base * dpr + dx) - left - 1);  // +-1: the strip's edges round
    int lastPx = std::min(totalPx, (int)std::ceil(last * dpr + base * dpr + dx) - left + 1);
    double step = TILE_W * dpr;
    for (int index = std::max(0, (int)(firstPx / step) - 1);; ++index) {
        auto cols = waveform_cache::tileColumns(index, dpr);
        int c0 = cols.first, c1 = std::min(cols.second, totalPx);
        if (c0 >= std::min(lastPx, totalPx)) break;
        if (c1 <= firstPx) continue;
        const Image& image = cache.tile(clip, index, c1 - c0, heightPx, pps, sr, color, dpr);
        RectF target((float)((left + c0 - dx) / dpr), (float)((topPx - dy) / dpr), (float)((c1 - c0) / dpr),
                     (float)(heightPx / dpr));
        p.drawImage(target, image, Rect(0, 0, c1 - c0, heightPx));
    }
}

void paintRange(Painter& p, const TimeRange& range, const std::vector<std::pair<int, int>>& spans,
                const std::function<double(int64_t)>& xOf, const Rect& visible) {
    double x0 = xOf(range.start), x1 = xOf(range.end);
    if (x1 + 2 < visible.left() || x0 - 2 > visible.right() - 1) return;  // +2: edge lines round to nearby pixels
    if (spans.empty()) return;
    Color fill = theme::c("selection_fill");
    int top = spans[0].first, bottom = spans[0].first + spans[0].second;
    for (auto& s : spans) {
        p.fillRect(pixels(RectF((float)x0, (float)s.first, (float)(x1 - x0), (float)s.second)), fill);
        top = std::min(top, s.first);
        bottom = std::max(bottom, s.first + s.second);
    }
    Color edge = theme::c("selection_edge");
    p.drawVLine((int)std::lround(x0), top, bottom + 1, edge);
    p.drawVLine((int)std::lround(x1), top, bottom + 1, edge);
}

}  // namespace clip_painter
