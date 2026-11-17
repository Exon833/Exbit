#include "ui/clip_painter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "config.h"
#include "ui/colors.h"
#include "ui/paint_helpers.h"
#include "ui/theme.h"
#include "ui/toolkit/style.h"
#include "ui/waveform_cache.h"

namespace clip_painter {

using waveform_cache::TILE_W;

int headerHeight() { return theme::metric("--clip-header-height", config::CLIP_HANDLE_H); }
int edgeWidth() { return theme::metric("--clip-edge-width", config::CLIP_EDGE_PX); }
int fadeHandleSize() { return theme::metric("--fade-handle-size", FADE_HANDLE_PX); }
int linkBadgeWidth() { return theme::metric("--link-badge-width", LINK_BADGE_W); }
int linkBadgeHeight() { return theme::metric("--link-badge-height", LINK_BADGE_H); }

// --- what the stylesheets give a clip ---


void Looks::refresh() {
    paint::Stamp stamp{generation_, states_};
    if (!stamp.stale(owner_)) return;
    generation_ = stamp.generation;
    states_ = stamp.states;
    looks_.clear();
}

const Look& Looks::get(const Clip& clip, bool selected, bool audible, bool frozen, int linkedWith) {
    bool linked = linkedWith >= 2, looping = clip.kind == Kind::Audio ? clip.loopingAudio() : clip.loop;
    int k = (selected ? 1 : 0) | (clip.disabled ? 2 : 0) | (frozen ? 4 : 0) | (!audible ? 8 : 0) | (linked ? 16 : 0) |
            (looping ? 32 : 0) | (clip.kind == Kind::Midi ? 64 : 0);
    auto it = looks_.find(k);
    if (it != looks_.end()) return it->second;
    std::vector<std::string> states;
    if (selected) states.push_back("selected");
    if (clip.disabled) states.push_back("disabled");
    if (frozen) states.push_back("frozen");
    if (!audible) states.push_back("muted");
    if (linked) states.push_back("linked");
    if (looping) states.push_back("looping");
    auto st = paint::nodeStyles(owner_, "Clip", {clip.kind == Kind::Midi ? "midi" : "audio"}, states,
                                {"", "body", "header", "overlay", "name", "fade", "loop-start", "link"});
    Look l;
    l.border = std::max(0.f, paint::length(st[0], "stroke-width", 1.5f));
    l.disabledInset = paint::length(st[0], "padding-top", 0);
    l.body = paint::withBackground(st[1], ui::style::background(st[1]));
    l.header = paint::withBackground(st[2], ui::style::background(st[2]));
    l.overlay = paint::withBackground(st[3], ui::style::background(st[3]));
    l.overlayShown = paint::shows(l.overlay);
    l.nameFont = ui::style::font(st[4], theme::uiFont());
    l.namePadTop = ui::style::px(st[4], "padding-top");
    l.namePadLeft = ui::style::px(st[4], "padding-left");
    l.namePadRight = ui::style::px(st[4], "padding-right");
    l.fadeShade = ui::style::background(st[5]);
    l.fadeStroke = std::max(0.f, paint::length(st[5], "stroke-width", 1.2f));
    l.loopStroke = std::max(0.f, paint::length(st[6], "stroke-width", 2));
    l.loopHeight = ui::style::px(st[6], "height", headerHeight() + 4);
    l.linkStroke = std::max(0.f, paint::length(st[7], "stroke-width", 1.6f));
    return looks_.emplace(k, std::move(l)).first->second;
}

Rect pixels(const RectF& r) {
    // a pixel is filled when its centre (x + 0.5) lies in [left, right)
    int x0 = (int)std::ceil(r.x - 0.5f), x1 = (int)std::ceil(r.right() - 0.5f);
    int y0 = (int)std::ceil(r.y - 0.5f), y1 = (int)std::ceil(r.bottom() - 0.5f);
    return Rect(x0, y0, x1 - x0, y1 - y0);
}

void paintClip(Painter& p, const Look& look, const Clip& clip, const RectF& rect, const Rect& visible, int viewLeft,
               double pps, int sr, bool selected, bool audible, bool frozen, bool handles, int linkedWith) {
    std::string color, tone;
    colors::clipColor(clip, color, tone);  // its own, its track's, or its kind's default
    colors::ClipColors cc = colors::clipColors(color, audible, tone);
    RectF badge;
    bool linked = linkedWith >= 2 && linkBadge(rect, badge);
    RectF named = linked ? rect.adjusted(0, 0, -(rect.right() - badge.x) + 2, 0) : rect;  // (the name stops short of it)
    if (clip.disabled) {
        // the whole line inside the clip's box
        float in = look.disabledInset;
        if (look.border > 0)
            p.strokeRect(rect.adjusted(in, in, -in, -in), selected ? cc.selectedBorder : cc.fill, look.border);
        paintName(p, look, clip, named, viewLeft, cc.fill);
        if (linked) paintLinkBadge(p, badge, cc.fill, look.linkStroke);
        return;
    }
    const int headerH = headerHeight();
    ui::style::box(p, pixels(rect), look.bodyIn(cc.fill));
    ui::style::box(p, pixels(RectF(rect.x, rect.y, rect.w, (float)headerH)), look.headerIn(cc.handle));
    RectF body = rect.adjusted(0, (float)headerH, 0, 0);
    if (body.bottom() >= visible.top() && body.y <= visible.bottom() - 1)  // not just the name strip
        paintContents(p, clip, body, visible, pps, sr, cc.content);
    if (clip.kind == Kind::Audio && !frozen && handles)
        paintFades(p, clip, body, pps, sr, cc.border, look.fadeShade, look.fadeStroke);
    if (clip.loopingAudio()) paintLoopStarts(p, clip, rect, visible, pps, sr, cc.border, look.loopStroke, look.loopHeight);
    if (look.overlayShown) ui::style::box(p, pixels(rect), look.overlay);  // (a frozen track's: its icy tint)
    if (look.border > 0) p.strokeRect(rect, selected ? cc.selectedBorder : cc.border, look.border);  // square corners
    paintName(p, look, clip, named, viewLeft, cc.text);
    if (linked) paintLinkBadge(p, badge, cc.text, look.linkStroke);
}

// --- linked MIDI clips: the chain mark ---

bool linkBadge(const RectF& rect, RectF& badge) {
    const int w = linkBadgeWidth(), h = linkBadgeHeight(), edge = edgeWidth();
    if (rect.w < w + 2 * edge + 12) return false;  // (too narrow: the name goes first)
    float x = rect.right() - edge - 2 - w;
    badge = RectF(x, rect.y + (headerHeight() - h) / 2.0f, (float)w, (float)h);
    return true;
}

void paintLinkBadge(Painter& p, const RectF& badge, Color color, float width) {
    // two links of a chain, side by side and overlapping, slightly tilted apart by their heights
    float h = badge.h - 2, w = badge.w * 0.62f;
    RectF left(badge.x + 0.5f, badge.y + 1, w, h);
    RectF right(badge.right() - w - 0.5f, badge.y + 1, w, h);
    p.strokeRoundedRect(left, h / 2, color, width);
    p.strokeRoundedRect(right, h / 2, color, width);
}

void paintLoopStarts(Painter& p, const Clip& clip, const RectF& rect, const Rect& visible, double pps, int sr, Color color,
                     float width, int height) {
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
            float reach = std::max(2.f, width);  // (its antialiased edges)
            if (visible.left() - reach <= x && x <= visible.right() - 1 + reach)
                p.drawLine(PointF(x, rect.y), PointF(x, rect.y + height), color, width);
        }
    }
}

bool fadeHandles(const Clip& clip, const RectF& body, double pps, int sr, RectF& in, RectF& out) {
    const int edge = edgeWidth(), size = fadeHandleSize();
    if (body.w < 4 * (edge + size)) return false;
    int64_t fadeIn, fadeOut;
    double a, b;
    clip.fadeParts(fadeIn, fadeOut, a, b);
    const double least = edge + 1, centre = body.x + body.w / 2.0;
    double xIn = std::min(std::max(body.x + (double)fadeIn / sr * pps, body.x + least), centre - size);
    double xOut = std::max(std::min(body.right() - (double)fadeOut / sr * pps, body.right() - least - size), centre);
    in = RectF((float)xIn, body.y + 1, (float)size, (float)size);
    out = RectF((float)xOut, body.y + 1, (float)size, (float)size);
    return true;
}

void paintFades(Painter& p, const Clip& clip, const RectF& body, double pps, int sr, Color color, Color shadeColor,
                float lineWidth) {
    // Each fade as its shape's curve (model.h FADE SHAPES), from the clip's edge to where it reaches full
    // level, with the part of the clip it makes quieter shaded above it. A piece of a longer fade starts
    // part-way along its curve (fadeParts: how far the fade is at the edge).
    int64_t fadeIn, fadeOut;
    double fromIn, toOut;
    clip.fadeParts(fadeIn, fadeOut, fromIn, toOut);
    struct Side { int64_t frames; float edge; int sign; double from; FadeShape shape; bool out; };
    for (const Side& s : {Side{fadeIn, body.x, 1, fromIn, clip.fadeInShape, false},
                          Side{fadeOut, body.right(), -1, toOut, clip.fadeOutShape, true}}) {
        if (!s.frames) continue;
        double width = (double)s.frames / sr * pps;
        // (a straight line needs its two ends; a curve a point every few pixels, within reason)
        int steps = s.shape == FadeShape::Linear ? 1 : std::max(4, std::min(64, (int)std::ceil(width / 3)));
        std::vector<PointF> curve;
        curve.reserve((size_t)steps + 1);
        for (int k = 0; k <= steps; k++) {
            double t = (double)k / steps;  // (across the fade, from the edge)
            double level = fadeCurve(s.shape, s.from + (1 - s.from) * t, s.out);
            curve.emplace_back((float)(s.edge + s.sign * width * t), (float)(body.bottom() - level * body.h));
        }
        std::vector<PointF> shade{PointF(s.edge, body.y)};
        shade.insert(shade.end(), curve.begin(), curve.end());
        p.fillPolygon(shade, shadeColor);
        if (lineWidth <= 0) continue;
        if (steps == 1) p.drawLine(curve[0], curve[1], color, lineWidth);
        else p.drawPolyline(curve, color, lineWidth, LineCap::Flat, LineJoin::Round);
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

void paintName(Painter& p, const Look& look, const Clip& clip, const RectF& rect, int viewLeft, Color color) {
    const Font& font = look.nameFont;
    std::string name = clipLabel(clip);
    float textW = font.width(name);
    float x = std::max(rect.x + look.namePadLeft,
                       std::min(std::max(rect.x, (float)viewLeft) + look.namePadLeft, rect.right() - textW - look.namePadRight));
    p.save();
    p.setClip(pixels(rect.adjusted(1, 1, -1, -1)));  // (inside its outline)
    p.drawText(PointF(x, rect.y + look.namePadTop + std::round(font.ascent())), name, font, color);
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
                const std::function<double(int64_t)>& xOf, const Rect& visible, const css::Style& fill, Color edge,
                int edgeWidth) {
    double x0 = xOf(range.start), x1 = xOf(range.end);
    int reach = 2 + std::max(0, edgeWidth / 2);  // (edge lines round to nearby pixels)
    if (x1 + reach < visible.left() || x0 - reach > visible.right() - 1) return;
    if (spans.empty()) return;
    int top = spans[0].first, bottom = spans[0].first + spans[0].second;
    for (auto& s : spans) {
        ui::style::box(p, pixels(RectF((float)x0, (float)s.first, (float)(x1 - x0), (float)s.second)), fill);
        top = std::min(top, s.first);
        bottom = std::max(bottom, s.first + s.second);
    }
    paint::Line l{edge, edgeWidth};
    paint::vline(p, (int)std::lround(x0), top, bottom + 1, l);
    paint::vline(p, (int)std::lround(x1), top, bottom + 1, l);
}

}  // namespace clip_painter
