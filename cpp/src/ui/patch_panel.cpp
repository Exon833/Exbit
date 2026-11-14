// The effects window's patch space (patch_panel.py).
#include "ui/patch_panel.h"

#include <algorithm>
#include <cmath>

#include "config.h"
#include "core/util.h"
#include "ui/theme.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"

using namespace patch_panel;

namespace {
const Color& color(const char* name) { return theme::c(name); }
Color mixed(const char* a, const char* b, float t) { return mixColor(theme::c(a), theme::c(b), t); }
constexpr int DRAG_START = 4;  // px the mouse moves before a unit is dragged (QApplication.startDragDistance on Windows)
RectF circle(PointF c, float r) { return RectF(c.x - r, c.y - r, 2 * r, 2 * r); }
double pyRound(double v) { return std::nearbyint(v); }  // (Python's round: halves to even)
bool contains(const std::vector<PanelCable>& cables, const PanelCable& c) {
    return std::find(cables.begin(), cables.end(), c) != cables.end();
}
// "−∞ dB", "+3.5 dB", "−6.0 dB", "0.0 dB"
std::string dbText(double db) {
    if (db <= config::MIN_DB) return "−∞ dB";
    if (db == 0) return "0.0 dB";
    return util::replaceAll(util::format("%+.1f dB", db), "-", "−");
}
}  // namespace

PanelNode into(const PanelNode& node) { return node.type == PanelNode::Side ? PanelNode::unit(node.index) : node; }

PatchPanel::PatchPanel(bool instruments_)
    : instruments(instruments_),
      cardH(CARD_H + VOLUME_H),  // (every unit has its fader: volume, or Mix)
      jackY(JACK_Y + VOLUME_H),
      row(CARD_H + VOLUME_H + 64) {
    setFocusPolicy(ui::FocusPolicy::Strong);
    setMouseTracking(true);
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
}

// --- content ---

void PatchPanel::setUnits(std::vector<PanelUnit> units, std::vector<PanelCable> cables, std::optional<int> current_,
                          std::vector<std::pair<std::string, bool>> sources) {
    units_ = std::move(units);
    cables_ = std::move(cables);
    sources_ = std::move(sources);
    pixmaps_.clear();
    for (auto& u : units_) pixmaps_.push_back(scaled(u.picture));
    std::vector<PanelUnit> placed;
    for (auto& unit : units_) {
        if (!unit.pos) {
            std::vector<PanelUnit> taken = placed;
            for (auto& u : units_)
                if (u.pos) taken.push_back(u);
            unit.pos = freeSpot(taken);
        }
        placed.push_back(unit);
    }
    if (current_) current = *current_;
    current = std::min(current, (int)units_.size() - 1);
    if (current < 0 && !units_.empty()) current = 0;
    if (wiring && wiring->lifted && !contains(cables_, *wiring->lifted)) wiring.reset();
    fit();
    update();
}

Image PatchPanel::scaled(const Image& image) const {
    if (image.empty()) return Image();
    double s = std::min((double)PIC_W / image.width(), (double)PIC_H / image.height());
    int w = std::max(1, (int)std::lround(image.width() * s)), h = std::max(1, (int)std::lround(image.height() * s));
    return w == image.width() && h == image.height() ? image : image.scaled(w, h);
}

void PatchPanel::setCurrent(int index) {
    if (index >= 0 && index < (int)units_.size() && index != current) {
        current = index;
        update();
        selected.emit(index);
    }
    if (current >= 0 && current < (int)units_.size()) ensureVisible(unitRect(current));
}

ui::ScrollArea* PatchPanel::scrollArea() const {
    Widget* viewport = parent();
    return viewport ? dynamic_cast<ui::ScrollArea*>(viewport->parent()) : nullptr;  // (viewport, then the scroll area)
}

void PatchPanel::ensureVisible(const Rect& r) {
    ui::ScrollArea* area = scrollArea();
    if (area && !r.empty()) {
        Point c = r.center();
        int mx = r.w / 2 + 8, my = r.h / 2 + 8;
        area->ensureVisible(Rect(c.x - mx, c.y - my, 2 * mx, 2 * my));
    }
}

// --- geometry ---

Point PatchPanel::contentSize() const {
    int right = END_W + GAP + CARD_W, bottom = top() + cardH;
    bool any = false;
    for (auto& u : units_)
        if (u.pos) {
            right = any ? std::max(right, u.pos->x + CARD_W) : u.pos->x + CARD_W;
            bottom = any ? std::max(bottom, u.pos->y + cardH) : u.pos->y + cardH;
            any = true;
        }
    int w = std::max({right + MARGIN + END_W, END_W * 2 + 2 * (CARD_W + GAP) + GAP,
                      SOURCES_X + (int)sources_.size() * SOURCE_W});
    int h = bottom + SAG + MARGIN + (sources_.empty() ? 0 : SOURCES_H) + RAIL;
    return Point(w, h);
}

int PatchPanel::sourcesTop() const { return height() - RAIL - SOURCES_H; }

void PatchPanel::fit() {
    Point s = contentSize();
    setMinimumSize(s.x, s.y);  // (a scroll area around it scrolls when it is bigger than the view)
    updateGeometry();
    if (ui::ScrollArea* area = scrollArea()) area->contentResized();
    else if (width() < s.x || height() < s.y) resize(std::max(width(), s.x), std::max(height(), s.y));
}

Rect PatchPanel::unitRect(int index) const {
    Point p = units_[index].pos.value_or(Point(0, 0));
    return Rect(p.x, p.y, CARD_W, cardH);
}

int PatchPanel::visibleWidth() const {
    ui::ScrollArea* area = scrollArea();
    return area ? area->viewport()->width() : width();
}

Point PatchPanel::freeSpot(const std::vector<PanelUnit>& taken) const {
    int columns = std::max(1, (int)std::floor((double)(visibleWidth() - 2 * END_W - GAP) / (CARD_W + GAP)));
    for (int n = 0; n < 1000; ++n) {
        Point spot(END_W + GAP + (n % columns) * (CARD_W + GAP), top() + (n / columns) * row);
        Rect area = Rect(spot.x, spot.y, CARD_W, cardH).adjusted(-GAP / 2, -GAP / 2, GAP / 2, GAP / 2);
        bool free = true;
        for (auto& u : taken)
            if (u.pos && area.intersects(Rect(u.pos->x, u.pos->y, CARD_W, cardH))) {
                free = false;
                break;
            }
        if (free) return spot;
    }
    return Point(END_W + GAP, top());
}

Point PatchPanel::spotAt(Point point) const {
    int x = std::max(END_W + SNAP, point.x - CARD_W / 2);
    int y = std::max(top(), point.y - cardH / 2);
    return Point((int)pyRound((double)x / SNAP) * SNAP, (int)pyRound((double)y / SNAP) * SNAP);
}

PointF PatchPanel::jackPoint(const Jack& jack) const {
    float y = (float)(top() + jackY);
    const PanelNode& node = jack.node;
    if (node.type == PanelNode::In) return PointF(END_W / 2.f, y);
    if (node.type == PanelNode::Out) return PointF(width() - END_W / 2.f, y);
    if (node.type == PanelNode::Track)
        return PointF((float)(SOURCES_X + node.index * SOURCE_W), sourcesTop() + SOURCES_H / 2.f);
    Rect r = unitRect(node.index);
    if (jack.kind == Jack::Source) return PointF((float)(r.x + CARD_W - 1 - 15), (float)(r.y + jackY));
    return PointF((float)(r.x + 16 + (jack.kind == Jack::SideIn ? SIDE_DX : 0)), (float)(r.y + jackY));
}

std::vector<Jack> PatchPanel::jacks() const {
    std::vector<Jack> out{{Jack::Source, PanelNode::in()}, {Jack::Target, PanelNode::out()}};
    for (int k = 0; k < (int)sources_.size(); ++k) out.push_back({Jack::Source, PanelNode::track(k)});
    for (int i = 0; i < (int)units_.size(); ++i) {
        out.push_back({Jack::Target, PanelNode::unit(i)});
        if (units_[i].sidechain) out.push_back({Jack::SideIn, PanelNode::unit(i)});
        out.push_back({Jack::Source, PanelNode::unit(i)});
    }
    return out;
}

std::optional<Jack> PatchPanel::jackAt(Point pos) const {
    for (const Jack& jack : jacks()) {
        PointF p = jackPoint(jack);
        float dx = p.x - pos.x, dy = p.y - pos.y;
        if (dx * dx + dy * dy <= (JACK_R + 4) * (JACK_R + 4)) return jack;
    }
    return std::nullopt;
}

Rect PatchPanel::ledRect(int index) const {
    Rect r = unitRect(index);
    int left = 34 + (units_[index].sidechain ? SIDE_DX + 20 : 0);
    return Rect(r.x + left, r.y + jackY - 10, CARD_W - left - 34, 20);
}

std::optional<Rect> PatchPanel::volumeRect(int index) const {
    if (!units_[index].fader()) return std::nullopt;
    Rect r = unitRect(index);
    return Rect(r.x + 8, r.y + NAME_Y + 20, CARD_W - 16, VOLUME_H - 4);
}

std::pair<double, double> PatchPanel::groove(int index) const {
    Rect v = volumeRect(index).value_or(Rect());
    return {v.x + 8.0, (v.x + v.w - 1.0) - valueWidth()};
}

int PatchPanel::valueWidth() const {
    int widest = (int)std::ceil(smallFont().width("Mix 100 %"));
    return std::max(VOLUME_TEXT_W, widest + KNOB_HALF_W + 8);
}

double PatchPanel::volumeAt(int index, double x) const {
    auto [x0, x1] = groove(index);
    double share = std::min(1.0, std::max(0.0, (x - x0) / (x1 - x0)));
    if (!units_[index].volumeDb) return pyRound(share * 100) / 100;
    double db = config::MIN_DB + share * (config::MAX_DB - config::MIN_DB);
    return std::fabs(db) < 0.75 ? 0.0 : pyRound(db * 2) / 2;
}

double PatchPanel::volumeX(int index, double value) const {
    auto [x0, x1] = groove(index);
    if (!units_[index].volumeDb) return x0 + value * (x1 - x0);  // (a Mix: 0…1 across)
    return x0 + (value - config::MIN_DB) / (config::MAX_DB - config::MIN_DB) * (x1 - x0);
}

std::optional<int> PatchPanel::volumeUnder(Point pos) const {
    auto index = unitAt(pos);
    if (!index) return std::nullopt;
    auto rect = volumeRect(*index);
    return rect && rect->contains(pos) ? index : std::nullopt;
}

void PatchPanel::setVolume(int index, double db) {
    PanelUnit& unit = units_[index];
    if (!unit.volumeDb && unit.mix) {  // a Mix: 0…1
        double value = std::min(1.0, std::max(0.0, db));
        if (value != *unit.mix) {
            unit.mix = value;
            update();
            volumeChanged.emit(index, value);
        }
        return;
    }
    db = std::min((double)config::MAX_DB, std::max((double)config::MIN_DB, db));
    if (!units_[index].volumeDb || db != *units_[index].volumeDb) {
        units_[index].volumeDb = db;
        update();
        volumeChanged.emit(index, db);
    }
}

std::optional<int> PatchPanel::unitAt(Point pos) const {
    auto order = paintOrder();
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (unitRect(*it).contains(pos)) return *it;
    return std::nullopt;
}

std::vector<int> PatchPanel::paintOrder() const {
    std::vector<int> order;
    for (int i = 0; i < (int)units_.size(); ++i)
        if (i != current) order.push_back(i);
    if (current >= 0 && current < (int)units_.size()) order.push_back(current);
    return order;
}

Path PatchPanel::cablePath(PointF a, PointF b) const {
    Path path;
    path.moveTo(a.x, a.y);
    if (a.y > b.y + 30) {  // (up from the other tracks' row: rises, then hangs into the jack)
        path.cubicTo(a.x, a.y - 70, b.x, b.y + 80, b.x, b.y);
        return path;
    }
    float sag = SAG + std::min(90.0f, std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)) * 0.12f);
    path.cubicTo(a.x + 14, a.y + sag, b.x - 14, b.y + sag, b.x, b.y);
    return path;
}

std::pair<PointF, PointF> PatchPanel::cableEnds(const PanelCable& cable) const {
    return {jackPoint({Jack::Source, cable.first}), jackPoint(targetJack(cable.second))};
}

Jack PatchPanel::targetJack(const PanelNode& node) {
    return node.type == PanelNode::Side ? Jack{Jack::SideIn, PanelNode::unit(node.index)} : Jack{Jack::Target, node};
}

PanelNode PatchPanel::targetNode(const Jack& jack) {
    return jack.kind == Jack::SideIn ? PanelNode::side(jack.node.index) : jack.node;
}

std::optional<PanelCable> PatchPanel::cableAt(Point pos) const {
    PointF q(pos.x + 0.5f, pos.y + 0.5f);
    for (auto it = cables_.rbegin(); it != cables_.rend(); ++it) {
        auto [a, b] = cableEnds(*it);
        for (auto& poly : cablePath(a, b).flatten(0.5f)) {
            for (size_t i = 1; i < poly.pts.size(); ++i) {
                PointF p0 = poly.pts[i - 1], p1 = poly.pts[i];
                float dx = p1.x - p0.x, dy = p1.y - p0.y, len2 = dx * dx + dy * dy;
                float t = len2 > 0 ? std::max(0.f, std::min(1.f, ((q.x - p0.x) * dx + (q.y - p0.y) * dy) / len2)) : 0;
                float ex = p0.x + t * dx - q.x, ey = p0.y + t * dy - q.y;
                if (ex * ex + ey * ey <= 25) return *it;  // (a stroke 10 px wide)
            }
        }
    }
    return std::nullopt;
}

std::string PatchPanel::nodeName(const PanelNode& node) const {
    switch (node.type) {
    case PanelNode::Track: return sources_[node.index].first;
    case PanelNode::Side: return units_[node.index].name + "'s sidechain";
    case PanelNode::In: return "IN";
    case PanelNode::Out: return "OUT";
    default: return units_[node.index].name;
    }
}

// --- wiring rules ---

bool PatchPanel::reaches(const PanelNode& start, const PanelNode& goal, const std::vector<PanelCable>& cables) {
    std::vector<PanelNode> todo{start}, seen;
    while (!todo.empty()) {
        PanelNode node = todo.back();
        todo.pop_back();
        if (node == goal) return true;
        for (auto& [a, b0] : cables) {
            PanelNode b = into(b0);
            if (a == node && std::find(seen.begin(), seen.end(), b) == seen.end()) {
                seen.push_back(b);
                todo.push_back(b);
            }
        }
    }
    return false;
}

bool PatchPanel::canPlug(const PanelCable& cable, const std::optional<PanelCable>& without) const {
    PanelNode a = cable.first, b = into(cable.second);
    std::vector<PanelCable> others;
    for (auto& c : cables_)
        if (!without || c != *without) others.push_back(c);
    if (instruments)  // notes from IN into an instrument, its sound to OUT: nothing else
        return !contains(others, cable) && ((a.type == PanelNode::In && b.type == PanelNode::Unit) ||
                                            (a.type == PanelNode::Unit && b.type == PanelNode::Out));
    if (a.type == PanelNode::Track)  // another track: only into a sidechain, never one that would make a loop
        return cable.second.type == PanelNode::Side && a.index >= 0 && a.index < (int)sources_.size() &&
               !sources_[a.index].second && !contains(others, cable);
    return !contains(others, cable) && a != b &&
           !(a.type != PanelNode::In && b.type != PanelNode::Out && reaches(b, a, others));
}

std::optional<PanelCable> PatchPanel::wiringTarget(const std::optional<Jack>& jack) const {
    if (!jack || !wiring) return std::nullopt;
    const Jack& held = wiring->from;
    if ((held.kind == Jack::Source) == (jack->kind == Jack::Source)) return std::nullopt;  // (outputs go to inputs)
    PanelCable cable = held.kind == Jack::Source ? PanelCable(held.node, targetNode(*jack))
                                                 : PanelCable(jack->node, targetNode(held));
    return canPlug(cable, wiring->lifted) ? std::optional<PanelCable>(cable) : std::nullopt;
}

// --- painting ---

Font PatchPanel::smallFont(bool bold, float size) const {
    Font base = theme::uiFont();
    return base.withPointSize(std::max(6.5f, base.pointSize() + size)).withBold(bold);
}

void PatchPanel::paint(Painter& p) {
    paintPanel(p);
    paintEnd(p, true);
    paintEnd(p, false);
    if (!sources_.empty()) paintSources(p);
    for (int index : paintOrder()) paintUnit(p, index);
    if (units_.empty())
        p.drawTextBlock(RectF((float)END_W, (float)(top() + 20), (float)(width() - 2 * END_W), (float)(cardH - 40)),
                        AlignCenter,
                        instruments
                            ? "Double-click here (or use Add…) to place an instrument.\n"
                              "Then wire it: drag a cable from IN (the notes) to its input, and from its output to OUT."
                            : "Double-click here (or use Add…) to place an effect.\n"
                              "Then wire it: drag a cable from IN to its input, and from its output to OUT.",
                        smallFont(false, 0), color("hint_text"));
    for (auto& cable : cables_)
        if (!(wiring && wiring->lifted && cable == *wiring->lifted)) {
            auto [a, b] = cableEnds(cable);
            paintCable(p, a, b, cableColor(cable));
        }
    if (wiring) paintWiring(p);
}

void PatchPanel::paintPanel(Painter& p) {
    int w = width(), h = height();
    p.fillRect(rect(), mixed("base", "window", 0.5f));
    for (int y : {0, h - RAIL}) {
        p.fillRect(Rect(0, y, w, RAIL), color("button"));
        for (int x = SCREW_EVERY / 2; x < w; x += SCREW_EVERY) paintScrew(p, PointF((float)x, y + RAIL / 2.f));
    }
    p.fillRect(Rect(0, RAIL, w, 1), color("frame_edge"));
    p.fillRect(Rect(0, h - RAIL - 1, w, 1), color("frame_edge"));
}

void PatchPanel::paintSources(Painter& p) {
    int top_ = sourcesTop();
    p.fillRect(Rect(0, top_, width(), SOURCES_H), mixed("base", "window", 0.25f));
    p.fillRect(Rect(0, top_, width(), 1), mixed("base", "frame_edge", 0.5f));
    p.drawTextBlock(RectF(8, (float)top_, SOURCES_X - 24.f, (float)SOURCES_H), AlignVCenter | AlignLeft,
                    "OTHER TRACKS\n(sidechains)", smallFont(true, -1.5f), color("hint_text"));
    Font f = smallFont();
    for (int k = 0; k < (int)sources_.size(); ++k) {
        p.save();
        p.setOpacity(sources_[k].second ? 0.35f : 1.f);
        Jack jack{Jack::Source, PanelNode::track(k)};
        paintJack(p, jack);
        PointF c = jackPoint(jack);
        std::string text = f.elided(sources_[k].first, (float)(SOURCE_W - JACK_R * 2 - 14));
        p.drawText(RectF(c.x + JACK_R + 6, c.y - 9, (float)(SOURCE_W - JACK_R * 2 - 12), 18), AlignLeft | AlignVCenter,
                   text, f, color("button_text"));
        p.restore();
    }
}

void PatchPanel::paintScrew(Painter& p, PointF c) {
    p.fillEllipse(circle(c, 4), mixed("button", "window_text", 0.28f));
    p.drawLine(PointF(c.x - 2.6f, c.y + 1.5f), PointF(c.x + 2.6f, c.y - 1.5f), mixed("button", "frame_edge", 0.6f), 1.4f);
}

void PatchPanel::paintJack(Painter& p, const Jack& jack) {
    PointF c = jackPoint(jack);
    p.fillEllipse(circle(c, JACK_R), mixed("button", "window_text", 0.35f));
    p.fillEllipse(circle(c, JACK_R - 3), color("frame_edge"));
    if (wiring && wiringTarget(jack)) {  // a place the cable can go
        bool hot = hover.kind == Hover::JackHit && hover.jack == jack;
        Color ring = color("accent").withAlpha(hot ? 255 : 150);
        p.strokeEllipse(circle(c, JACK_R + 4), ring, hot ? 3.f : 2.f);
    }
}

void PatchPanel::paintEnd(Painter& p, bool in) {
    Jack jack = in ? Jack{Jack::Source, PanelNode::in()} : Jack{Jack::Target, PanelNode::out()};
    PointF c = jackPoint(jack);
    p.drawText(RectF(c.x - END_W / 2.f, top() + 8.f, (float)END_W, 18), AlignCenter, in ? "IN" : "OUT",
               smallFont(true), color("hint_text"));
    p.drawText(RectF(c.x - END_W / 2.f, top() + 24.f, (float)END_W, 16), AlignCenter,
               instruments ? (in ? "notes" : "to effects") : (in ? "from track" : "to fader"), smallFont(),
               color("hint_text"));
    paintJack(p, jack);
}

void PatchPanel::paintUnit(Painter& p, int index) {
    const PanelUnit& unit = units_[index];
    Rect r = unitRect(index);
    float radius = (float)theme::radius();
    bool hovered = (hover.kind == Hover::Unit || hover.kind == Hover::Led || hover.kind == Hover::Volume) &&
                   hover.index == index && !wiring;
    Color fill = hovered ? mixed("button", "window_text", 0.06f) : color("button");
    if (radius > 0) p.fillRoundedRect(RectF(r), radius, fill);
    else p.fillRect(r, fill);
    // the picture of the plugin
    Rect pic(r.x + PIC_X, r.y + PIC_Y, PIC_W, PIC_H);
    p.fillRect(pic, color("base"));
    p.save();
    p.setOpacity(unit.bypass ? 0.35f : 1.f);
    const Image& pixmap = pixmaps_[index];
    if (!pixmap.empty()) p.drawImage(Point(pic.x + (pic.w - pixmap.width()) / 2, pic.y + (pic.h - pixmap.height()) / 2), pixmap);
    else paintFaceplate(p, pic);
    p.restore();
    // the name
    Font bold = smallFont(true, 0);
    p.drawText(RectF((float)(r.x + 8), (float)(r.y + NAME_Y), CARD_W - 16.f, 18), AlignCenter,
               bold.elided(unit.name, CARD_W - 16.f), bold, unit.bypass ? color("disabled_text") : color("button_text"));
    if (unit.fader()) paintVolume(p, index);
    // jacks, and the light with its state
    paintJack(p, {Jack::Target, PanelNode::unit(index)});
    if (unit.sidechain) {
        Jack side{Jack::SideIn, PanelNode::unit(index)};
        paintJack(p, side);
        PointF c = jackPoint(side);
        p.drawText(RectF(c.x + JACK_R + 2, c.y - 8, 20, 16), AlignLeft | AlignVCenter, "SC", smallFont(true, -2),
                   color("hint_text"));
    }
    paintJack(p, {Jack::Source, PanelNode::unit(index)});
    Rect led = ledRect(index);
    Font f = smallFont();
    float width_ = f.width(unit.status) + 2 * LED_R + 6;
    PointF centre(led.x + (led.w - 1) / 2.f - width_ / 2 + LED_R, led.y + (led.h - 1) / 2.f + 0.5f);
    Color lit = unit.problem ? color("record")
                : unit.bypass ? mixed("button", "frame_edge", 0.45f)
                : unit.idle   ? color("meter_warn")
                              : color("meter_ok");
    p.fillEllipse(circle(centre, LED_R), lit);
    p.strokeEllipse(circle(centre, LED_R), color("frame_edge"), 1);
    bool ledHot = hover.kind == Hover::Led && hover.index == index;
    p.drawText(RectF(centre.x + LED_R + 6, (float)led.y, width_, (float)led.h), AlignLeft | AlignVCenter, unit.status, f,
               ledHot ? color("button_text") : color("hint_text"));
    if (index == current) p.strokeRoundedRect(RectF(r).adjusted(1, 1, -1, -1), radius, color("accent"), 2);
}

void PatchPanel::paintVolume(Painter& p, int index) {
    // a unit's fader: a groove filled up to the knob, a mark at 0 dB (a volume), and the value
    const PanelUnit& unit = units_[index];
    Rect v = *volumeRect(index);
    auto [x0, x1] = groove(index);
    float y = v.y + (v.h - 1) / 2.f + 0.5f, knob = (float)volumeX(index, *unit.fader());
    p.fillRoundedRect(RectF((float)x0, y - 2, (float)(x1 - x0), 4), 2, color("frame_edge"));
    if (knob - x0 > 0)
        p.fillRoundedRect(RectF((float)x0, y - 2, (float)(knob - x0), 4), 2,
                          unit.bypass ? color("disabled_text") : mixed("accent", "button", 0.25f));
    if (unit.volumeDb) {
        float zero = (float)volumeX(index, 0.0);
        p.drawLine(PointF(zero + 0.5f, y - 7), PointF(zero + 0.5f, y - 4), color("hint_text"), 1);
        p.drawLine(PointF(zero + 0.5f, y + 4), PointF(zero + 0.5f, y + 7), color("hint_text"), 1);
    }
    bool hot = (hover.kind == Hover::Volume && hover.index == index) || (volumeDrag && *volumeDrag == index);
    RectF k(knob - KNOB_HALF_W, y - 8, 2 * KNOB_HALF_W, 16);
    p.fillRoundedRect(k, 3, hot ? color("accent") : mixed("button", "window_text", 0.55f));
    p.strokeRoundedRect(k, 3, color("frame_edge"), 1);
    std::string text = unit.volumeDb ? dbText(*unit.volumeDb)
                                     : "Mix " + std::to_string((int)pyRound(*unit.mix * 100)) + " %";
    float left = (float)(x1 + KNOB_HALF_W + 3);
    p.drawText(RectF(left, (float)v.y, (float)(v.x + v.w - 1) - left, (float)v.h), AlignRight | AlignVCenter, text,
               smallFont(), hot ? color("button_text") : color("hint_text"));
}

void PatchPanel::paintFaceplate(Painter& p, const Rect& pic) {
    // no picture yet: a blank faceplate with a few knobs
    Color pen = mixed("base", "hint_text", 0.35f);
    for (int i = 0; i < 4; ++i) {
        PointF c(pic.x + pic.w * (i + 0.5f) / 4, pic.y + pic.h * 0.42f);
        p.strokeEllipse(circle(c, 12), pen, 1.6f);
        p.drawLine(c, PointF(c.x - 6, c.y - 8), pen, 1.6f);
    }
    p.drawText(RectF((float)pic.x, pic.y + pic.h * 0.68f, (float)pic.w, 20), AlignCenter, "No picture yet", smallFont(),
               mixed("base", "hint_text", 0.7f));
}

Color PatchPanel::cableColor(const PanelCable& cable) const {
    auto it = std::find(cables_.begin(), cables_.end(), cable);
    int i = it == cables_.end() ? 0 : (int)(it - cables_.begin());
    return Color::fromHex(CABLES[i % CABLE_COLOURS]);
}

void PatchPanel::paintCable(Painter& p, PointF a, PointF b, Color cable) {
    Path path = cablePath(a, b);
    p.strokePath(path, cable.darker(150), 6, LineCap::Round, LineJoin::Round);
    p.strokePath(path, cable, 3.6f, LineCap::Round, LineJoin::Round);
    for (PointF end : {a, b}) {
        p.fillEllipse(circle(end, JACK_R - 1), cable.darker(130));
        p.fillEllipse(circle(end, JACK_R - 3.5f), cable);
    }
}

void PatchPanel::paintWiring(Painter& p) {
    // the cable in the hand: from the jack it is plugged into to the mouse (or the jack it would go in)
    Jack::Kind side = wiring->from.kind;
    PointF fixed = jackPoint(wiring->from);
    std::optional<Jack> target;
    if (hover.kind == Hover::JackHit) target = hover.jack;
    PointF loose = wiringTarget(target) ? jackPoint(*target) : wiring->to;
    PointF a = side == Jack::Source ? fixed : loose, b = side == Jack::Source ? loose : fixed;
    Color cable = wiring->lifted ? cableColor(*wiring->lifted)
                                 : Color::fromHex(CABLES[cables_.size() % CABLE_COLOURS]);
    paintCable(p, a, b, cable);
}

// --- mouse and keys ---

bool PatchPanel::mousePress(ui::MouseEvent& e) {
    setFocus();
    Point pos = e.pos;
    if (e.button == plat::RightButton) return true;  // (contextMenu() follows)
    if (e.button != plat::LeftButton) return false;
    if (auto jack = jackAt(pos)) {
        startWiring(*jack, PointF(pos));
        return true;
    }
    auto index = unitAt(pos);
    if (!index) return true;
    if (ledRect(*index).contains(pos)) {
        bypassToggled.emit(*index);
        return true;
    }
    setCurrent(*index);
    if (volumeUnder(pos) == index) {  // its fader: the knob goes where clicked, then follows the mouse
        volumeDrag = *index;
        setVolume(*index, volumeAt(*index, pos.x));
        return true;
    }
    Rect r = unitRect(*index);
    press_ = Press{*index, pos, Point(pos.x - r.x, pos.y - r.y)};
    return true;
}

bool PatchPanel::contextMenu(ui::MouseEvent& e) {
    rightClick(e.pos, e.globalPos);
    return true;
}

void PatchPanel::startWiring(const Jack& jack, PointF pos) {
    // pick up a cable: the one plugged into this input (to move or unplug it), else a new one from here
    std::optional<PanelCable> lifted;
    if (jack.kind != Jack::Source)
        for (auto it = cables_.rbegin(); it != cables_.rend(); ++it)
            if (it->second == targetNode(jack)) {
                lifted = *it;
                break;
            }
    Jack held = lifted ? Jack{Jack::Source, lifted->first} : jack;
    wiring = Wiring{held, lifted, pos};
    hover = Hover{Hover::JackHit, -1, jack};
    update();
}

void PatchPanel::rightClick(Point pos, Point where) {
    auto index = unitAt(pos);
    std::optional<PanelCable> cable = jackAt(pos) ? std::nullopt : cableAt(pos);  // (cables lie on top of the units)
    if (cable) {
        ui::Menu menu;
        PanelCable c = *cable;
        menu.addAction("Remove the cable " + nodeName(c.first) + " → " + nodeName(c.second),
                       [this, c] { patched.emit(c, std::nullopt); });
        menu.exec(where, this);
    } else if (index) {
        setCurrent(*index);
        menuRequested.emit(*index, where);
    } else {
        spaceMenuRequested.emit(pos, where);
    }
}

bool PatchPanel::mouseMove(ui::MouseEvent& e) {
    Point pos = e.pos;
    if (volumeDrag) {
        setVolume(*volumeDrag, volumeAt(*volumeDrag, pos.x));
        return true;
    }
    if (wiring) {
        wiring->to = PointF(pos);
        auto jack = jackAt(pos);
        hover = jack ? Hover{Hover::JackHit, -1, *jack} : Hover{};
        ensureVisible(Rect(pos.x, pos.y, 1, 1));
        update();
        return true;
    }
    if (press_) {
        Point d = pos - press_->start;
        if (moving_ || std::abs(d.x) + std::abs(d.y) >= DRAG_START) {
            moving_ = true;
            int index = press_->index;
            units_[index].pos = spotAt(pos - press_->offset + Point(CARD_W / 2, cardH / 2));
            fit();  // (the space grows if needed)
            ensureVisible(unitRect(index));
            update();
        }
        return true;
    }
    hoverAt(pos);
    return true;
}

void PatchPanel::hoverAt(Point pos) {
    auto jack = jackAt(pos);
    auto index = unitAt(pos);
    Hover hit;
    if (jack) hit = Hover{Hover::JackHit, -1, *jack};
    else if (index && ledRect(*index).contains(pos)) hit = Hover{Hover::Led, *index, {}};
    else if (index && volumeUnder(pos) == index) hit = Hover{Hover::Volume, *index, {}};
    else if (index) hit = Hover{Hover::Unit, *index, {}};
    if (hit != hover) {
        hover = hit;
        setCursor(hit.kind == Hover::JackHit || hit.kind == Hover::Led ? ui::Cursor::Hand
                  : hit.kind == Hover::Volume                          ? ui::Cursor::SizeHor
                                                                       : ui::Cursor::Arrow);
        update();
    }
}

bool PatchPanel::mouseRelease(ui::MouseEvent& e) {
    Point pos = e.pos;
    if (volumeDrag) {
        volumeDrag.reset();
        hoverAt(pos);
        update();
        return true;
    }
    if (wiring) {
        auto cable = wiringTarget(jackAt(pos));
        auto lifted = wiring->lifted;
        wiring.reset();
        if (cable) {
            if (cable != lifted) patched.emit(lifted, cable);  // plugged in (moved, if it was picked up off an input)
        } else if (lifted) {
            patched.emit(lifted, std::nullopt);  // dropped anywhere else: unplugged
        }
        hoverAt(pos);
        update();
        return true;
    }
    auto press = press_;
    bool moving = moving_;
    press_.reset();
    moving_ = false;
    if (press && moving && press->index < (int)units_.size()) unitMoved.emit(press->index, *units_[press->index].pos);
    return true;
}

bool PatchPanel::mouseDoubleClick(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton) return mousePress(e);
    Point pos = e.pos;
    if (jackAt(pos)) return true;
    auto index = unitAt(pos);
    if (!index) addRequested.emit(spotAt(pos));
    else if (volumeUnder(pos) == index)  // (back to 0 dB, or to 100 % of the effect)
        setVolume(*index, units_[*index].volumeDb ? 0.0 : 1.0);
    else if (ledRect(*index).contains(pos)) bypassToggled.emit(*index);  // (a quick second click on the light counts once more)
    else {
        setCurrent(*index);
        opened.emit(*index);
    }
    return true;
}

bool PatchPanel::wheel(ui::WheelEvent& e) {
    // over a unit's fader: 1 dB (a Mix: 5 %) a notch; elsewhere the space scrolls
    auto index = volumeUnder(e.pos);
    int steps = (int)std::floor(e.dy);
    if (steps == 0 && e.dy > 0) steps = 1;
    if (!index || !steps) return false;
    const PanelUnit& unit = units_[*index];
    if (unit.volumeDb) setVolume(*index, pyRound(*unit.volumeDb) + steps);
    else setVolume(*index, pyRound(*unit.mix * 20 + steps) / 20);
    return true;
}

void PatchPanel::leave() {
    if (!wiring) {
        hover = Hover{};
        update();
    }
}

std::string PatchPanel::toolTipAt(Point pos) const {
    auto jack = jackAt(pos);
    if (jack && instruments) {
        const PanelNode& node = jack->node;
        if (node.type == PanelNode::In)
            return "IN: the track's notes (its clips, and keys played in the MIDI editor). Drag a cable from "
                   "here to an instrument's input.";
        if (node.type == PanelNode::Out)
            return "OUT: on to the track's effects. Every instrument wired in here is heard, added up.";
        const std::string& name = units_[node.index].name;
        return jack->kind == Jack::Source ? name + "'s sound: drag a cable from here to OUT."
                                          : name + "'s input, for the notes: drag a cable from here to IN. With a cable "
                                                   "in: drag its plug away to move or unplug it.";
    }
    if (auto index = volumeUnder(pos)) {
        if (!units_[*index].volumeDb)
            return units_[*index].name + "'s Mix: how much of its sound you hear against its input passing by "
                                         "unchanged (dry / wet: 100 % is the effect only, 50 % half of each). Drag "
                                         "the knob, or turn the mouse wheel over it; double-click for 100 %.";
        return units_[*index].name + "'s volume: drag the knob, or turn the mouse wheel over it; double-click for 0 dB.";
    }
    if (jack) {
        const PanelNode& node = jack->node;
        if (node.type == PanelNode::In)
            return "IN: the track's sound, before its effects. Drag a cable from here to an effect's input.";
        if (node.type == PanelNode::Out)
            return "OUT: on to the track's fader. What is wired in here is heard (nothing: silence).";
        if (node.type == PanelNode::Track) {
            const auto& [name, blocked] = sources_[node.index];
            if (blocked) return name + " already listens to this one (through a sidechain), so it can't be wired in here.";
            return name + ": its sound after its effects and fader, for sidechains only (it isn't heard here). "
                          "Drag a cable from here to an effect's SC jack. Even muted, it still steers the sidechain.";
        }
        const std::string& name = units_[node.index].name;
        if (jack->kind == Jack::SideIn)
            return name + "'s sidechain input: what is wired in here steers " + name +
                   " (e.g. a compressor ducking to it) without being heard. Drag a cable from here to IN or an "
                   "effect's output.";
        return jack->kind == Jack::Source
                   ? name + "'s output: drag a cable from here to another effect's input, a sidechain or OUT."
                   : name + "'s input: drag a cable from here to IN or an effect's output. With a cable in: drag its "
                            "plug away to move or unplug it.";
    }
    if (auto index = unitAt(pos)) return units_[*index].tooltip;
    if (auto cable = cableAt(pos))
        return nodeName(cable->first) + " → " + nodeName(cable->second) +
               ": right-click to remove it, or drag its plug off the input to move or unplug it.";
    return "";
}

bool PatchPanel::keyPress(ui::KeyEvent& e) {
    int key = e.key;
    int n = (int)units_.size();
    if ((key == plat::Key_Left || key == plat::Key_Right) && n) {
        setCurrent(std::max(0, std::min(n - 1, current + (key == plat::Key_Left ? -1 : 1))));
    } else if ((key == plat::Key_Return || key == plat::Key_Enter) && current >= 0 && current < n) {
        opened.emit(current);
    } else if (key == plat::Key_Delete && current >= 0 && current < n) {
        removeRequested.emit(current);
    } else if (key == plat::Key_Escape && wiring) {
        wiring.reset();
        update();
    } else {
        return false;
    }
    return true;
}
