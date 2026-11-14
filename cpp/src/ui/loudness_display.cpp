// Small two-line LUFS readout (loudness_display.py).
#include "ui/loudness_display.h"

#include <cmath>
#include <algorithm>
#include <cstdio>

namespace loudness_display {

std::string fmtLufs(double value) {
    if (std::isinf(value) || std::isnan(value)) return "–";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", value);
    return buf;
}

namespace {
size_t codepoints(const std::string& s) {
    size_t n = 0;
    for (unsigned char ch : s)
        if ((ch & 0xC0) != 0x80) ++n;
    return n;
}
std::string rjust(const std::string& s, size_t width) {  // f"{s:>5}"
    size_t n = codepoints(s);
    return n >= width ? s : std::string(width - n, ' ') + s;
}
}  // namespace

std::string text(double shortTerm, double integrated) {
    return "S " + rjust(fmtLufs(shortTerm), 5) + "\nI " + rjust(fmtLufs(integrated), 5);
}

std::string tip(double momentary) {
    return "Loudness in LUFS (EBU R128)\n"
           "S  short-term, last 3 s\n"
           "I  integrated, since Play was pressed\n"
           "M  momentary, last 0.4 s: " + fmtLufs(momentary);
}

}  // namespace loudness_display

LoudnessDisplay::LoudnessDisplay() {
    objectName = "lufs";  // the skin's numbers font
    setFixedWidth(62);
    setAlignment(AlignRight | AlignVCenter);
    showValues(-INFINITY, -INFINITY, -INFINITY);
}

void LoudnessDisplay::showValues(double momentary, double shortTerm, double integrated) {
    setText(loudness_display::text(shortTerm, integrated));
    setToolTip(loudness_display::tip(momentary));
}

void LoudnessDisplay::paint(Painter& p) {
    Font f = font();
    Color c = color();
    const std::string& t = text();
    size_t nl = t.find('\n');
    std::string lines[2] = {t.substr(0, nl), nl == std::string::npos ? "" : t.substr(nl + 1)};
    int n = nl == std::string::npos ? 1 : 2;
    float spacing = f.lineSpacing();
    float pitch = std::min(spacing, (float)height() / n);
    float cap = f.ascent() * 0.72f;  // (digits' height)
    float top = (height() - pitch * n) / 2;
    for (int i = 0; i < n; ++i) {
        float baseline = top + i * pitch + (pitch + cap) / 2;
        float x = width() - f.width(lines[i]);
        p.drawText(PointF(std::round(x), std::round(baseline)), lines[i], f, c);
    }
}
