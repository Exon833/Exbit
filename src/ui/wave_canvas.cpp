// The Drawn synth's canvas (see wave_canvas.h).
#include "ui/wave_canvas.h"

#include <algorithm>
#include <cmath>

#include "audio/synth_patch.h"
#include "ui/panel_style.h"
#include "ui/theme.h"

namespace {
constexpr int MARGIN = 8;  // between the widget's edge and the wave's area
}

WaveCanvas::WaveCanvas() : wave((size_t)subsynth::WAVE_POINTS, 0.0f) {
    setStyleType("WaveCanvas");
    setFocusPolicy(ui::FocusPolicy::Strong);
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
}

Point WaveCanvas::sizeHint() const { return Point(640, 320); }

Rect WaveCanvas::area() const {
    return Rect(MARGIN, MARGIN, std::max(1, width() - 2 * MARGIN), std::max(1, height() - 2 * MARGIN));
}

int WaveCanvas::pointAt(int x) const {
    Rect a = area();
    int n = (int)wave.size();
    return std::clamp((int)std::floor((double)(x - a.x) * n / a.w), 0, std::max(0, n - 1));
}

double WaveCanvas::valueAt(int y) const {
    Rect a = area();
    return std::clamp(1.0 - 2.0 * (y - a.y) / std::max(1, a.h - 1), -1.0, 1.0);
}

void WaveCanvas::paint(Painter& p) {
    const css::Style& st = computedStyle();
    Rect a = area();
    p.fillRect(rect(), panel_style::bg(st));
    Color grid = ui::style::color(computedStyle("grid"), "color", theme::c("grid"));
    double midY = a.y + (a.h - 1) / 2.0;
    p.drawLine(PointF(a.x, midY), PointF(a.right(), midY), grid);
    for (int q = 1; q < 4; q++) {
        float x = a.x + a.w * q / 4.0f;
        p.drawLine(PointF(x, a.y), PointF(x, a.bottom()), grid);
    }
    p.strokeRect(RectF(a), grid);
    int n = (int)wave.size();
    if (n < 1) return;
    auto yOf = [&](double v) { return (float)(midY - v * (a.h - 1) / 2.0); };
    if (hasFocus()) {  // the keyboard's point
        Color c = ui::style::color(computedStyle("cursor"), "color", theme::c("highlight"));
        float x0 = a.x + (float)a.w * cursor / n, x1 = a.x + (float)a.w * (cursor + 1) / n;
        p.fillRect(RectF(x0, a.y, std::max(1.0f, x1 - x0), a.h), Color(c.r, c.g, c.b, 60));
        p.fillEllipse(RectF((x0 + x1) / 2 - 3, yOf(wave[(size_t)cursor]) - 3, 6, 6), c);
    }
    // each point at its centre, the last joined back to the first's value (one cycle, as it plays)
    std::vector<PointF> line;
    line.reserve((size_t)n + 2);
    line.emplace_back((float)a.x, yOf((wave[0] + wave[(size_t)n - 1]) / 2));
    for (int i = 0; i < n; i++) line.emplace_back(a.x + a.w * (i + 0.5f) / n, yOf(wave[(size_t)i]));
    line.emplace_back((float)a.right(), yOf((wave[0] + wave[(size_t)n - 1]) / 2));
    p.drawPolyline(line, ui::style::foreground(st), 2);
}

void WaveCanvas::drawTo(int point, double value) {
    int from = last_ < 0 ? point : last_;
    double fromValue = last_ < 0 ? value : lastValue_;
    int steps = std::abs(point - from);
    for (int k = 0; k <= steps; k++) {
        int i = from + (point > from ? k : -k);
        double v = steps ? fromValue + (value - fromValue) * k / steps : value;
        wave[(size_t)i] = (float)std::clamp(v, -1.0, 1.0);
    }
    last_ = point;
    lastValue_ = value;
    cursor = point;
    update();
}

bool WaveCanvas::mousePress(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton || wave.empty()) return true;
    setFocus();
    last_ = -1;
    drawTo(pointAt(e.pos.x), valueAt(e.pos.y));
    if (changed) changed(wave, false);
    return true;
}

bool WaveCanvas::mouseMove(ui::MouseEvent& e) {
    if (last_ < 0) return true;
    drawTo(pointAt(e.pos.x), valueAt(e.pos.y));
    if (changed) changed(wave, false);
    return true;
}

bool WaveCanvas::mouseRelease(ui::MouseEvent& e) {
    if (last_ < 0) return true;
    last_ = -1;
    if (changed) changed(wave, true);
    return true;
}

bool WaveCanvas::keyPress(ui::KeyEvent& e) {
    int n = (int)wave.size();
    if (n < 1) return false;
    switch (e.key) {
    case plat::Key_Left: cursor = std::max(0, cursor - (e.shift() ? 16 : 1)); break;
    case plat::Key_Right: cursor = std::min(n - 1, cursor + (e.shift() ? 16 : 1)); break;
    case plat::Key_Home: cursor = 0; break;
    case plat::Key_End: cursor = n - 1; break;
    case plat::Key_Up:
    case plat::Key_Down: {
        cursor = std::clamp(cursor, 0, n - 1);
        double step = (e.shift() ? 1.0 / 256 : 1.0 / 32) * (e.key == plat::Key_Up ? 1 : -1);
        wave[(size_t)cursor] = (float)std::clamp(wave[(size_t)cursor] + step, -1.0, 1.0);
        if (changed) changed(wave, false);
        break;
    }
    default: return false;
    }
    update();
    return true;
}

bool WaveCanvas::textCells(int cols, int rows, std::vector<ui::TextCell>& cells) const {
    cells.assign((size_t)cols * rows, ui::TextCell());
    int n = (int)wave.size();
    if (cols < 1 || rows < 1 || n < 1) return true;
    auto rowOf = [&](double v) { return std::clamp((int)std::lround((1 - v) / 2 * (rows - 1)), 0, rows - 1); };
    int mid = rowOf(0);
    for (int c = 0; c < cols; c++) cells[(size_t)(mid * cols + c)] = ui::TextCell{U'─', 8, 9, 0};  // ─
    int cursorCol = std::clamp((int)((long long)cursor * cols / n), 0, cols - 1);
    for (int c = 0; c < cols; c++) {
        int i = std::clamp((int)((long long)c * n / cols), 0, n - 1);
        int r = rowOf(wave[(size_t)i]);
        // (from the centre line to the value: a bar, so its shape reads at a glance)
        for (int y = std::min(r, mid); y <= std::max(r, mid); y++)
            cells[(size_t)(y * cols + c)] = ui::TextCell{y == r ? U'●' : U'│', 2, 9, 0};  // ● │
    }
    if (hasFocus())
        for (int y = 0; y < rows; y++) cells[(size_t)(y * cols + cursorCol)].attr |= 2;  // (reversed: tui::Reverse)
    return true;
}
