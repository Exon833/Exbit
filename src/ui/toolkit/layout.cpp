// Layouts: rows / columns (QBoxLayout), forms (QFormLayout), grids (QGridLayout).
//
// Space is shared the way Qt's qGeomCalc does it: everyone gets their size hint; extra space goes
// to the items with a stretch factor (in proportion to it), else to the Expanding ones, else to
// everyone who may grow; the shares are of the whole space (so two expanding items end up equally
// big when they can), never below an item's hint nor above its maximum. Too little space takes
// from the items that can shrink (down to their minimum). Across the row an item fills the space
// up to its maximum and is centred in what is left (unless aligned).
#include "ui/toolkit/layout.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "ui/theme.h"
#include "ui/toolkit/controls.h"

namespace ui {

// (the spacing of a layout made without one: default.css's :root)
Layout::Layout() : spacing(std::max(0, theme::metric("layout-spacing", 4))) {}
GridLayout::GridLayout() { hSpacing = vSpacing = std::max(0, theme::metric("grid-spacing", 6)); }

namespace {
const int kMax = 1 << 24;

struct Sz {
    int min = 0, hint = 0, max = kMax;
    int stretch = 0;
    bool expand = false;
};

// Measuring is recursive: a row's size hint asks each child for its hint, minimum and maximum, and
// whether it expands, along both axes; a child with a layout asks its own children the same, and so
// on, so without remembering, every level multiplies the work (a panel of track rows: ~40 ms to
// measure). While one measurement runs nothing changes, so each widget's and layout's sizes are
// remembered until the outermost measurement is done (a Measuring held by it), then forgotten.
struct Measured {
    int depth = 0;
    std::unordered_map<const void*, Sz> sizes[2];  // [horizontal]: widgets and layouts
};
Measured measured;

struct Measuring {
    Measuring() { ++measured.depth; }
    ~Measuring() {
        if (--measured.depth == 0) {
            measured.sizes[0].clear();
            measured.sizes[1].clear();
        }
    }
};

Sz measureWidget(const Widget* w, bool horizontal);
Sz measureLayout(const Layout* l, bool horizontal);

template <class T, class F>
Sz remembered(const T* key, bool horizontal, F measure) {
    Measuring m;
    auto& sizes = measured.sizes[horizontal ? 1 : 0];
    auto found = sizes.find(key);
    if (found != sizes.end()) return found->second;
    Sz s = measure(key, horizontal);
    sizes[key] = s;
    return s;
}

// a widget's sizes along one axis (horizontal: x)
Sz widgetSz(const Widget* w, bool horizontal) { return remembered(w, horizontal, measureWidget); }
Sz layoutSz(const Layout* l, bool horizontal) { return remembered(l, horizontal, measureLayout); }

Sz measureWidget(const Widget* w, bool horizontal) {
    Point h = w->sizeHint(), mh = w->minimumSizeHint(), mn = w->minimumSize(), mx = w->maximumSize();
    SizePolicy pol = horizontal ? w->hPolicy : w->vPolicy;
    Sz s;
    int hint = horizontal ? h.x : h.y;
    int minHint = horizontal ? mh.x : mh.y;
    int emin = horizontal ? mn.x : mn.y;
    int emax = horizontal ? mx.x : mx.y;
    s.max = emax;
    if (pol == SizePolicy::Fixed) s.max = std::min(emax, std::max(hint, emin));
    s.min = emin > 0 ? emin : (pol == SizePolicy::Fixed ? hint : minHint);
    s.min = std::min(std::max(0, s.min), s.max);
    s.hint = std::max(s.min, std::min(hint, s.max));
    s.expand = pol == SizePolicy::Expanding;
    if (w->layout() && pol != SizePolicy::Fixed && (w->layout()->expandingDirections() & (horizontal ? 1 : 2))) s.expand = true;
    return s;
}

Sz measureLayout(const Layout* l, bool horizontal) {
    Point h = l->sizeHint(), mn = l->minimumSize(), mx = l->maximumSize();
    Sz s;
    s.min = horizontal ? mn.x : mn.y;
    s.max = std::max(s.min, horizontal ? mx.x : mx.y);
    s.hint = std::max(s.min, std::min(horizontal ? h.x : h.y, s.max));
    s.expand = (l->expandingDirections() & (horizontal ? 1 : 2)) != 0;
    return s;
}

int satAdd(int a, int b) { return (long long)a + b >= kMax ? kMax : a + b; }

// sizes for items sharing `avail` px (spacing already taken off)
std::vector<int> distribute(const std::vector<Sz>& items, int avail) {
    size_t n = items.size();
    std::vector<int> out(n, 0);
    if (!n) return out;
    long long sumHint = 0, sumMin = 0;
    for (auto& it : items) {
        sumHint += it.hint;
        sumMin += it.min;
    }
    if (avail >= sumHint) {
        for (size_t i = 0; i < n; ++i) out[i] = items[i].hint;
        bool anyStretch = false, anyExpand = false;
        for (auto& it : items) {
            if (it.stretch > 0 && it.max > it.hint) anyStretch = true;
            if (it.expand && it.max > it.hint) anyExpand = true;
        }
        std::vector<int> weight(n, 0);
        for (size_t i = 0; i < n; ++i) {
            const Sz& it = items[i];
            if (it.max <= it.hint) continue;
            if (anyStretch) weight[i] = it.stretch;
            else if (anyExpand) weight[i] = it.expand ? 1 : 0;
            else weight[i] = 1;
        }
        std::vector<bool> fixedDone(n, false);
        for (size_t i = 0; i < n; ++i)
            if (!weight[i]) fixedDone[i] = true;
        long long total = avail;
        for (size_t i = 0; i < n; ++i)
            if (fixedDone[i]) total -= out[i];
        for (int round = 0; round < (int)n + 2; ++round) {
            long long W = 0;
            for (size_t i = 0; i < n; ++i)
                if (!fixedDone[i]) W += weight[i];
            if (W <= 0) break;
            bool changed = false;
            for (size_t i = 0; i < n && !changed; ++i) {
                if (fixedDone[i]) continue;
                double target = (double)total * weight[i] / W;
                if (target < items[i].hint) {
                    out[i] = items[i].hint;
                    fixedDone[i] = true;
                    total -= out[i];
                    changed = true;
                } else if (target > items[i].max) {
                    out[i] = items[i].max;
                    fixedDone[i] = true;
                    total -= out[i];
                    changed = true;
                }
            }
            if (changed) continue;
            // final shares, rounded so they add up
            double acc = 0;
            long long given = 0;
            for (size_t i = 0; i < n; ++i) {
                if (fixedDone[i]) continue;
                acc += (double)total * weight[i] / W;
                long long upto = (long long)(acc + 0.5);
                out[i] = (int)(upto - given);
                given = upto;
            }
            break;
        }
    } else if (avail >= sumMin) {
        long long over = sumHint - avail;
        long long room = 0;
        for (auto& it : items) room += it.hint - it.min;
        double acc = 0;
        long long taken = 0;
        for (size_t i = 0; i < n; ++i) {
            int r = items[i].hint - items[i].min;
            acc += room > 0 ? (double)over * r / room : 0;
            long long upto = (long long)(acc + 0.5);
            out[i] = items[i].hint - (int)(upto - taken);
            taken = upto;
        }
    } else {
        for (size_t i = 0; i < n; ++i) out[i] = items[i].min;
    }
    return out;
}

// place a widget in its cell as QWidgetItem::setGeometry does
void placeWidget(Widget* w, const Rect& cell, int align) {
    Point mx = w->maximumSize();
    Sz hs = widgetSz(w, true), vs = widgetSz(w, false);
    int sw = std::min(cell.w, std::min(mx.x, hs.max));
    int sh = std::min(cell.h, std::min(mx.y, vs.max));
    if (align & (AlignLeft | AlignRight | AlignHCenter)) sw = std::min(sw, std::max(hs.hint, hs.min));
    if (align & (AlignTop | AlignBottom | AlignVCenter)) sh = std::min(sh, std::max(vs.hint, vs.min));
    sw = std::max(0, sw);
    sh = std::max(0, sh);
    int x = cell.x, y = cell.y;
    if (align & AlignRight) x += cell.w - sw;
    else if (!(align & AlignLeft)) x += (cell.w - sw) / 2;
    if (align & AlignBottom) y += cell.h - sh;
    else if (!(align & AlignTop)) y += (cell.h - sh) / 2;
    w->setGeometry(x, y, sw, sh);
}

bool itemShown(const BoxLayout::Item& it) {
    if (it.widget) return it.widget->isVisible() && !it.widget->deleted_;
    return true;
}
}  // namespace

// a widget made a child of the layout's owner
static void adoptInto(Widget* owner, Widget* w) {
    if (!w || !owner || w->parent() == owner) return;
    if (w->parent()) owner->adopt(w->parent()->release(w));
    else owner->adopt(std::unique_ptr<Widget>(w));
}

// ============================================================================================
// BoxLayout
// ============================================================================================

void BoxLayout::adoptItems() {
    for (auto& it : items) {
        if (it.widget) adoptInto(owner, it.widget);
        if (it.layout) it.layout->setOwner(owner);
    }
}

void BoxLayout::addWidget(Widget* w, int stretch, int align) { insertWidget((int)items.size(), w, stretch, align); }

void BoxLayout::insertWidget(int index, Widget* w, int stretch, int align) {
    if (!w) return;
    adoptInto(owner, w);
    Item it;
    it.widget = w;
    it.stretch = stretch;
    it.align = align;
    index = std::max(0, std::min(index, (int)items.size()));
    items.insert(items.begin() + index, std::move(it));
    invalidate();
}

void BoxLayout::addSpacing(int px) {
    Item it;
    it.spacing = px;
    items.push_back(std::move(it));
    invalidate();
}

void BoxLayout::addStretch(int stretch) {
    Item it;
    it.isStretch = true;
    it.stretch = stretch;
    items.push_back(std::move(it));
    invalidate();
}

void BoxLayout::addLayout(std::unique_ptr<Layout> l, int stretch) {
    if (!l) return;
    l->setOwner(owner);
    Item it;
    it.layout = std::move(l);
    it.stretch = stretch;
    items.push_back(std::move(it));
    invalidate();
}

void BoxLayout::removeWidget(Widget* w) {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].widget == w) {
            items.erase(items.begin() + i);
            invalidate();
            return;
        }
    for (auto& it : items)
        if (it.layout) {
            if (auto* b = dynamic_cast<BoxLayout*>(it.layout.get())) b->removeWidget(w);
            else it.layout->forgetWidget(w);
        }
}

void BoxLayout::forgetWidget(Widget* w) {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].widget == w) {
            items.erase(items.begin() + i);
            invalidate();
            return;
        }
    for (auto& it : items)
        if (it.layout) it.layout->forgetWidget(w);
}

void BoxLayout::clear() {
    items.clear();
    invalidate();
}

namespace {
struct BoxItemSz { Sz main, cross; bool skip; bool isSpacing; };
}

static std::vector<BoxItemSz> boxSizes(const BoxLayout& b) {
    Measuring m;
    bool horz = b.dir == Direction::Horizontal;
    std::vector<BoxItemSz> v;
    for (auto& it : b.items) {
        BoxItemSz s{};
        s.skip = !itemShown(it);
        if (it.widget) {
            s.main = widgetSz(it.widget, horz);
            s.cross = widgetSz(it.widget, !horz);
            s.main.stretch = it.stretch;
        } else if (it.layout) {
            s.main = layoutSz(it.layout.get(), horz);
            s.cross = layoutSz(it.layout.get(), !horz);
            s.main.stretch = it.stretch;
        } else if (it.isStretch) {
            s.main.min = s.main.hint = 0;
            s.main.max = kMax;
            s.main.stretch = it.stretch;
            s.main.expand = true;
            s.cross.min = s.cross.hint = 0;
            s.cross.max = kMax;
            s.isSpacing = true;
        } else {
            s.main.min = s.main.hint = s.main.max = it.spacing;
            s.cross.min = s.cross.hint = 0;
            s.cross.max = kMax;
            s.isSpacing = true;
        }
        v.push_back(s);
    }
    return v;
}

// spacing goes between two shown items that aren't spacers next to each other... (as Qt: between
// every pair of shown items, but not next to an explicit addSpacing)
static int gapsFor(const BoxLayout& b, const std::vector<BoxItemSz>& v, std::vector<int>* before = nullptr) {
    int total = 0;
    bool havePrev = false;
    bool prevSpacing = false;
    if (before) before->assign(v.size(), 0);
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].skip) continue;
        bool isSpacer = !b.items[i].widget && !b.items[i].layout;
        if (havePrev && !isSpacer && !prevSpacing) {
            total += b.spacing;
            if (before) (*before)[i] = b.spacing;
        }
        havePrev = true;
        prevSpacing = isSpacer;
    }
    return total;
}

Point BoxLayout::sizeHint() const {
    bool horz = dir == Direction::Horizontal;
    auto v = boxSizes(*this);
    int main = gapsFor(*this, v), cross = 0;
    for (auto& s : v) {
        if (s.skip) continue;
        main = satAdd(main, s.main.hint);
        cross = std::max(cross, s.cross.hint);
    }
    int mw = margins[0] + margins[2], mh = margins[1] + margins[3];
    return horz ? Point(main + mw, cross + mh) : Point(cross + mw, main + mh);
}

Point BoxLayout::minimumSize() const {
    bool horz = dir == Direction::Horizontal;
    auto v = boxSizes(*this);
    int main = gapsFor(*this, v), cross = 0;
    for (auto& s : v) {
        if (s.skip) continue;
        main = satAdd(main, s.main.min);
        cross = std::max(cross, s.cross.min);
    }
    int mw = margins[0] + margins[2], mh = margins[1] + margins[3];
    return horz ? Point(main + mw, cross + mh) : Point(cross + mw, main + mh);
}

Point BoxLayout::maximumSize() const {
    bool horz = dir == Direction::Horizontal;
    auto v = boxSizes(*this);
    int main = gapsFor(*this, v), cross = 0, crossMin = 0;
    bool any = false;
    for (auto& s : v) {
        if (s.skip) continue;
        main = satAdd(main, s.main.max);
        if (!s.isSpacing) {
            cross = std::max(cross, s.cross.max);
            any = true;
        }
        crossMin = std::max(crossMin, s.cross.min);
    }
    if (!any) cross = kMax;
    cross = std::max(cross, crossMin);
    int mw = margins[0] + margins[2], mh = margins[1] + margins[3];
    main = satAdd(main, horz ? mw : mh);
    cross = satAdd(cross, horz ? mh : mw);
    return horz ? Point(main, cross) : Point(cross, main);
}

int BoxLayout::expandingDirections() const {
    int e = 0;
    for (auto& it : items) {
        if (!itemShown(it)) continue;
        if (it.widget) {
            if (widgetSz(it.widget, true).expand) e |= 1;
            if (widgetSz(it.widget, false).expand) e |= 2;
        } else if (it.layout) {
            e |= it.layout->expandingDirections();
        } else if (it.isStretch) {
            e |= dir == Direction::Horizontal ? 1 : 2;
        }
    }
    return e;
}

void BoxLayout::apply(Widget* o, const Rect& area0) {
    if (o) owner = o;
    bool horz = dir == Direction::Horizontal;
    Rect area = area0.adjusted(margins[0], margins[1], -margins[2], -margins[3]);
    auto v = boxSizes(*this);
    std::vector<int> before;
    int gaps = gapsFor(*this, v, &before);
    std::vector<Sz> mains;
    std::vector<size_t> idx;
    for (size_t i = 0; i < v.size(); ++i)
        if (!v[i].skip) {
            mains.push_back(v[i].main);
            idx.push_back(i);
        }
    int avail = (horz ? area.w : area.h) - gaps;
    std::vector<int> sizes = distribute(mains, std::max(0, avail));
    int pos = horz ? area.x : area.y;
    for (size_t k = 0; k < idx.size(); ++k) {
        size_t i = idx[k];
        pos += before[i];
        int sz = sizes[k];
        Rect cell = horz ? Rect(pos, area.y, sz, area.h) : Rect(area.x, pos, area.w, sz);
        Item& it = items[i];
        if (it.widget) placeWidget(it.widget, cell, it.align);
        else if (it.layout) it.layout->apply(owner, cell);
        pos += sz;
    }
}

// ============================================================================================
// FormLayout
// ============================================================================================

void FormLayout::adoptItems() {
    for (auto& r : rows) {
        adoptInto(owner, r.label);
        adoptInto(owner, r.field);
        if (r.fieldLayout) r.fieldLayout->setOwner(owner);
    }
}

void FormLayout::addRow(Widget* label, Widget* field) {
    adoptInto(owner, label);
    adoptInto(owner, field);
    Row r;
    r.label = label;
    r.field = field;
    rows.push_back(std::move(r));
    invalidate();
}

void FormLayout::addRow(const std::string& label, Widget* field) {
    Label* l = nullptr;
    if (owner) l = owner->add<Label>(label);
    addRow(l, field);
}

void FormLayout::addRow(Widget* spanning) { addRow(nullptr, spanning); }

void FormLayout::addRow(const std::string& label, std::unique_ptr<Layout> field) {
    Label* l = nullptr;
    if (owner) l = owner->add<Label>(label);
    Row r;
    r.label = l;
    field->setOwner(owner);
    r.fieldLayout = std::move(field);
    rows.push_back(std::move(r));
    invalidate();
}

void FormLayout::forgetWidget(Widget* w) {
    for (size_t i = 0; i < rows.size(); ++i) {
        Row& r = rows[i];
        if (r.label == w) r.label = nullptr;
        if (r.field == w) r.field = nullptr;
        if (r.fieldLayout) r.fieldLayout->forgetWidget(w);
        if (!r.label && !r.field && !r.fieldLayout) {
            rows.erase(rows.begin() + i);
            --i;
        }
    }
    invalidate();
}

namespace {
bool shownW(Widget* w) { return w && w->isVisible() && !w->deleted_; }
}

struct FormMetrics {
    int labelW = 0, labelMinW = 0;
    std::vector<int> heights, minHeights;
    int fieldW = 0, fieldMinW = 0, spanW = 0, spanMinW = 0;
};

static FormMetrics formMetrics(const FormLayout& f) {
    FormMetrics m;
    for (auto& r : f.rows) {
        bool vis = shownW(r.label) || shownW(r.field) || r.fieldLayout;
        if (!vis) {
            m.heights.push_back(-1);
            m.minHeights.push_back(-1);
            continue;
        }
        int h = 0, mh = 0;
        bool span = !r.label && (r.field || r.fieldLayout);
        if (shownW(r.label)) {
            Sz hs = widgetSz(r.label, true), vs = widgetSz(r.label, false);
            m.labelW = std::max(m.labelW, hs.hint);
            m.labelMinW = std::max(m.labelMinW, hs.min);
            h = std::max(h, vs.hint);
            mh = std::max(mh, vs.min);
        }
        Sz fh, fv;
        bool haveField = false;
        if (shownW(r.field)) {
            fh = widgetSz(r.field, true);
            fv = widgetSz(r.field, false);
            haveField = true;
        } else if (r.fieldLayout) {
            fh = layoutSz(r.fieldLayout.get(), true);
            fv = layoutSz(r.fieldLayout.get(), false);
            haveField = true;
        }
        if (haveField) {
            if (span) {
                m.spanW = std::max(m.spanW, fh.hint);
                m.spanMinW = std::max(m.spanMinW, fh.min);
            } else {
                m.fieldW = std::max(m.fieldW, fh.hint);
                m.fieldMinW = std::max(m.fieldMinW, fh.min);
            }
            h = std::max(h, fv.hint);
            mh = std::max(mh, fv.min);
        }
        m.heights.push_back(h);
        m.minHeights.push_back(mh);
    }
    return m;
}

Point FormLayout::sizeHint() const {
    FormMetrics m = formMetrics(*this);
    int w = m.labelW > 0 ? m.labelW + spacing + m.fieldW : m.fieldW;
    w = std::max(w, m.spanW);
    int h = 0, n = 0;
    for (int x : m.heights)
        if (x >= 0) {
            h += x;
            ++n;
        }
    if (n > 1) h += (n - 1) * spacing;
    return Point(w + margins[0] + margins[2], h + margins[1] + margins[3]);
}

Point FormLayout::minimumSize() const {
    FormMetrics m = formMetrics(*this);
    int w = m.labelMinW > 0 || m.labelW > 0 ? m.labelW + spacing + m.fieldMinW : m.fieldMinW;
    w = std::max(w, m.spanMinW);
    int h = 0, n = 0;
    for (int x : m.minHeights)
        if (x >= 0) {
            h += x;
            ++n;
        }
    if (n > 1) h += (n - 1) * spacing;
    return Point(w + margins[0] + margins[2], h + margins[1] + margins[3]);
}

int FormLayout::expandingDirections() const {
    int e = 1;  // fields grow sideways
    for (auto& r : rows) {
        if (shownW(r.field) && widgetSz(r.field, false).expand) e |= 2;
        if (r.fieldLayout) e |= r.fieldLayout->expandingDirections() & 2;
    }
    return e;
}

void FormLayout::apply(Widget* o, const Rect& area0) {
    if (o) owner = o;
    Rect area = area0.adjusted(margins[0], margins[1], -margins[2], -margins[3]);
    FormMetrics m = formMetrics(*this);
    // rows: their hints (and vertical expanding fields share what's left)
    std::vector<Sz> rs;
    std::vector<size_t> idx;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (m.heights[i] < 0) continue;
        Sz s;
        s.hint = m.heights[i];
        s.min = m.minHeights[i];
        s.max = s.hint;
        const Row& r = rows[i];
        bool grow = (shownW(r.field) && widgetSz(r.field, false).expand) ||
                    (r.fieldLayout && (r.fieldLayout->expandingDirections() & 2));
        if (grow) {
            s.max = kMax;
            s.expand = true;
        }
        rs.push_back(s);
        idx.push_back(i);
    }
    int gaps = rs.size() > 1 ? (int)(rs.size() - 1) * spacing : 0;
    std::vector<int> hs = distribute(rs, std::max(0, area.h - gaps));
    // a row that can't grow keeps its hint; what's left stays at the bottom
    int labelW = m.labelW;
    int fieldX = area.x + (labelW > 0 ? labelW + spacing : 0);
    int fieldW = std::max(0, area.right() - fieldX);
    int y = area.y;
    for (size_t k = 0; k < idx.size(); ++k) {
        Row& r = rows[idx[k]];
        int h = hs[k];
        if (r.label && shownW(r.label)) {
            int la = labelAlign;
            Rect cell(area.x, y, labelW, h);
            // labels sit level with a one-line field: centred on the field's hint height
            int fh = h;
            if (shownW(r.field)) fh = std::min(h, widgetSz(r.field, false).hint);
            else if (r.fieldLayout) fh = std::min(h, layoutSz(r.fieldLayout.get(), false).hint);
            if (fh > 0 && fh < h && !(la & AlignBottom)) cell.h = fh;
            placeWidget(r.label, cell, la | ((la & (AlignTop | AlignBottom | AlignVCenter)) ? 0 : AlignVCenter));
        }
        bool span = !r.label;
        Rect cell = span ? Rect(area.x, y, area.w, h) : Rect(fieldX, y, fieldW, h);
        if (shownW(r.field)) placeWidget(r.field, cell, 0);
        else if (r.fieldLayout) r.fieldLayout->apply(owner, cell);
        y += h + spacing;
    }
}

// ============================================================================================
// GridLayout
// ============================================================================================

void GridLayout::adoptItems() {
    for (auto& c : cells) adoptInto(owner, c.w);
}

void GridLayout::addWidget(Widget* w, int row, int col, int rowSpan, int colSpan, int align) {
    if (!w) return;
    adoptInto(owner, w);
    cells.push_back({w, row, col, std::max(1, rowSpan), std::max(1, colSpan), align});
    invalidate();
}

void GridLayout::setColumnStretch(int col, int stretch) {
    if ((int)colStretch.size() <= col) colStretch.resize(col + 1, 0);
    colStretch[col] = stretch;
    invalidate();
}

void GridLayout::setRowStretch(int row, int stretch) {
    if ((int)rowStretch.size() <= row) rowStretch.resize(row + 1, 0);
    rowStretch[row] = stretch;
    invalidate();
}

void GridLayout::forgetWidget(Widget* w) {
    cells.erase(std::remove_if(cells.begin(), cells.end(), [w](const Cell& c) { return c.w == w; }), cells.end());
    invalidate();
}

namespace {
struct GridAxis { std::vector<Sz> lines; };

GridAxis gridAxis(const GridLayout& g, bool horizontal) {
    int n = 0;
    for (auto& c : g.cells) n = std::max(n, horizontal ? c.col + c.colSpan : c.row + c.rowSpan);
    const std::vector<int>& st = horizontal ? g.colStretch : g.rowStretch;
    n = std::max(n, (int)st.size());
    GridAxis a;
    a.lines.assign(n, Sz());
    std::vector<bool> used(n, false);
    for (auto& l : a.lines) l.max = 0;
    // single cells first
    for (auto& c : g.cells) {
        if (!shownW(c.w)) continue;
        int span = horizontal ? c.colSpan : c.rowSpan;
        int at = horizontal ? c.col : c.row;
        Sz s = widgetSz(c.w, horizontal);
        if (span == 1) {
            Sz& l = a.lines[at];
            l.min = std::max(l.min, s.min);
            l.hint = std::max(l.hint, s.hint);
            l.max = std::max(l.max, s.max);
            l.expand = l.expand || s.expand;
        }
        for (int k = at; k < at + span && k < n; ++k) used[k] = true;
    }
    // spanning cells: make the spanned lines big enough (the last one takes the rest)
    int gap = horizontal ? g.hSpacing : g.vSpacing;
    for (auto& c : g.cells) {
        if (!shownW(c.w)) continue;
        int span = horizontal ? c.colSpan : c.rowSpan;
        if (span <= 1) continue;
        int at = horizontal ? c.col : c.row;
        Sz s = widgetSz(c.w, horizontal);
        int sumH = (span - 1) * gap, sumM = (span - 1) * gap;
        bool anyMax = false;
        for (int k = at; k < at + span && k < n; ++k) {
            sumH += a.lines[k].hint;
            sumM += a.lines[k].min;
            if (a.lines[k].max > 0) anyMax = true;
        }
        int last = std::min(n - 1, at + span - 1);
        if (s.hint > sumH) a.lines[last].hint += s.hint - sumH;
        if (s.min > sumM) a.lines[last].min += s.min - sumM;
        if (!anyMax) a.lines[last].max = std::max(a.lines[last].max, s.max);
        if (s.expand) a.lines[last].expand = true;
    }
    for (int k = 0; k < n; ++k) {
        Sz& l = a.lines[k];
        if (k < (int)st.size()) l.stretch = st[k];
        if (l.max < l.hint) l.max = used[k] ? std::max(l.hint, l.max) : (l.stretch > 0 ? kMax : 0);
        if (l.stretch > 0) l.max = kMax;
        l.hint = std::max(l.hint, l.min);
    }
    return a;
}

int lineGaps(const std::vector<Sz>& lines, int gap) {
    int n = 0;
    for (auto& l : lines)
        if (l.hint > 0 || l.max > 0) ++n;
    return n > 1 ? (n - 1) * gap : 0;
}
}  // namespace

Point GridLayout::sizeHint() const {
    GridAxis h = gridAxis(*this, true), v = gridAxis(*this, false);
    int w = lineGaps(h.lines, hSpacing), hh = lineGaps(v.lines, vSpacing);
    for (auto& l : h.lines) w += l.hint;
    for (auto& l : v.lines) hh += l.hint;
    return Point(w + margins[0] + margins[2], hh + margins[1] + margins[3]);
}

Point GridLayout::minimumSize() const {
    GridAxis h = gridAxis(*this, true), v = gridAxis(*this, false);
    int w = lineGaps(h.lines, hSpacing), hh = lineGaps(v.lines, vSpacing);
    for (auto& l : h.lines) w += l.min;
    for (auto& l : v.lines) hh += l.min;
    return Point(w + margins[0] + margins[2], hh + margins[1] + margins[3]);
}

int GridLayout::expandingDirections() const {
    int e = 0;
    for (auto& c : cells) {
        if (!shownW(c.w)) continue;
        if (widgetSz(c.w, true).expand) e |= 1;
        if (widgetSz(c.w, false).expand) e |= 2;
    }
    for (int s : colStretch)
        if (s > 0) e |= 1;
    for (int s : rowStretch)
        if (s > 0) e |= 2;
    return e;
}

void GridLayout::apply(Widget* o, const Rect& area0) {
    if (o) owner = o;
    Rect area = area0.adjusted(margins[0], margins[1], -margins[2], -margins[3]);
    GridAxis h = gridAxis(*this, true), v = gridAxis(*this, false);
    auto place = [](const std::vector<Sz>& lines, int start, int avail, int gap, std::vector<int>& pos, std::vector<int>& size) {
        size_t n = lines.size();
        std::vector<bool> present(n);
        int count = 0;
        for (size_t i = 0; i < n; ++i) {
            present[i] = lines[i].hint > 0 || lines[i].max > 0;
            if (present[i]) ++count;
        }
        int gaps = count > 1 ? (count - 1) * gap : 0;
        std::vector<int> s = distribute(lines, std::max(0, avail - gaps));
        pos.assign(n, start);
        size.assign(n, 0);
        int p = start;
        bool first = true;
        for (size_t i = 0; i < n; ++i) {
            if (!present[i]) {
                pos[i] = p;
                continue;
            }
            if (!first) p += gap;
            first = false;
            pos[i] = p;
            size[i] = s[i];
            p += s[i];
        }
    };
    std::vector<int> cx, cw, ry, rh;
    place(h.lines, area.x, area.w, hSpacing, cx, cw);
    place(v.lines, area.y, area.h, vSpacing, ry, rh);
    for (auto& c : cells) {
        if (!shownW(c.w)) continue;
        int c1 = std::min((int)cx.size() - 1, c.col + c.colSpan - 1);
        int r1 = std::min((int)ry.size() - 1, c.row + c.rowSpan - 1);
        if (c1 < 0 || r1 < 0) continue;
        Rect cell(cx[c.col], ry[c.row], cx[c1] + cw[c1] - cx[c.col], ry[r1] + rh[r1] - ry[c.row]);
        placeWidget(c.w, cell, c.align);
    }
}

// ============================================================================================
// helpers
// ============================================================================================

HBox* hbox(Widget* w, int margin, int spacing) {
    auto l = std::make_unique<HBox>();
    HBox* raw = l.get();
    raw->setContentsMargins(margin, margin, margin, margin);
    raw->spacing = spacing;
    raw->owner = w;
    w->setLayout(std::move(l));
    return raw;
}

VBox* vbox(Widget* w, int margin, int spacing) {
    auto l = std::make_unique<VBox>();
    VBox* raw = l.get();
    raw->setContentsMargins(margin, margin, margin, margin);
    raw->spacing = spacing;
    raw->owner = w;
    w->setLayout(std::move(l));
    return raw;
}

FormLayout* form(Widget* w, int margin, int spacing) {
    auto l = std::make_unique<FormLayout>();
    FormLayout* raw = l.get();
    raw->setContentsMargins(margin, margin, margin, margin);
    raw->spacing = spacing;
    raw->owner = w;
    w->setLayout(std::move(l));
    return raw;
}

GridLayout* grid(Widget* w, int margin, int spacing) {
    auto l = std::make_unique<GridLayout>();
    GridLayout* raw = l.get();
    raw->setContentsMargins(margin, margin, margin, margin);
    raw->spacing = spacing;
    raw->hSpacing = raw->vSpacing = spacing;
    raw->owner = w;
    w->setLayout(std::move(l));
    return raw;
}

void styleRoomOf(Layout* l, const css::Style& s) {
    if (!l) return;
    auto whole = [](double v) { return std::max(0, (int)std::lround(v)); };
    auto* grid = dynamic_cast<GridLayout*>(l);
    auto before = [&] {
        return std::vector<int>{l->margins[0], l->margins[1], l->margins[2], l->margins[3], l->spacing,
                                grid ? grid->hSpacing : 0, grid ? grid->vSpacing : 0};
    };
    std::vector<int> old = before();
    css::Edges was;
    was.left = l->margins[0], was.top = l->margins[1], was.right = l->margins[2], was.bottom = l->margins[3];
    css::Edges e = s.edges("padding", was);
    l->setContentsMargins(whole(e.left), whole(e.top), whole(e.right), whole(e.bottom));
    if (grid) {
        grid->hSpacing = whole(s.length("column-gap", grid->hSpacing));
        grid->vSpacing = whole(s.length("row-gap", grid->vSpacing));
        grid->spacing = grid->vSpacing;
    } else if (auto* b = dynamic_cast<BoxLayout*>(l)) {
        b->spacing = whole(s.length(b->dir == Direction::Horizontal ? "column-gap" : "row-gap", b->spacing));
    } else {
        l->spacing = whole(s.length("row-gap", l->spacing));
    }
    if (before() != old) l->invalidate();
}

}  // namespace ui
