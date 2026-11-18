// Layouts: rows, columns, forms and grids that place a widget's children (QHBoxLayout,
// QVBoxLayout, QFormLayout, QGridLayout). Items are widgets (owned by the layout's widget), fixed
// spacings and stretches. Sizes come from each widget's sizeHint(), minimum/maximum and size policy.
#pragma once
#include <memory>
#include <vector>

#include "ui/toolkit/widget.h"

namespace ui {

class Layout {
public:
    Layout();  // its spacing: the stylesheets' --layout-spacing (4 px)
    virtual ~Layout() = default;
    virtual void apply(Widget* owner, const Rect& area) = 0;  // place the items inside area
    virtual Point sizeHint() const = 0;
    virtual Point minimumSize() const { return sizeHint(); }
    virtual Point maximumSize() const { return Point(1 << 24, 1 << 24); }
    // bit 1: wants to grow horizontally, bit 2: vertically (an Expanding widget or stretch inside)
    virtual int expandingDirections() const { return 0; }
    // a widget is going away: drop it (and from nested layouts)
    virtual void forgetWidget(Widget* w) {}
    void invalidate() { if (owner) owner->updateGeometry(), App::get().scheduleLayout(owner); }
    // sets the widget it lays out, making its items' widgets (and nested layouts') children of it
    void setOwner(Widget* w) { owner = w; adoptItems(); }
    virtual void adoptItems() {}
    int margins[4] = {0, 0, 0, 0};  // left, top, right, bottom
    int spacing = 4;
    void setContentsMargins(int l, int t, int r, int b) { margins[0] = l; margins[1] = t; margins[2] = r; margins[3] = b; }
    void setSpacing(int s) { spacing = s; }
    Widget* owner = nullptr;
};

enum class Direction { Horizontal, Vertical };

class BoxLayout : public Layout {
public:
    explicit BoxLayout(Direction d) : dir(d) {}
    // the widget must be a child of the layout's owner (or nullptr: made a child when added)
    void addWidget(Widget* w, int stretch = 0, int align = 0);
    void insertWidget(int index, Widget* w, int stretch = 0, int align = 0);
    void addSpacing(int px);
    void addStretch(int stretch = 1);
    void addLayout(std::unique_ptr<Layout> l, int stretch = 0);  // a nested layout (same owner)
    void removeWidget(Widget* w);
    void clear();  // removes every item (widgets stay children of the owner)
    int count() const { return (int)items.size(); }
    void apply(Widget* owner, const Rect& area) override;
    Point sizeHint() const override;
    Point minimumSize() const override;
    Point maximumSize() const override;
    int expandingDirections() const override;
    void forgetWidget(Widget* w) override;
    void adoptItems() override;
    Direction dir;
    struct Item { Widget* widget = nullptr; std::unique_ptr<Layout> layout; int spacing = 0; int stretch = 0; int align = 0; bool isStretch = false; };
    std::vector<Item> items;
};

class HBox : public BoxLayout { public: HBox() : BoxLayout(Direction::Horizontal) {} };
class VBox : public BoxLayout { public: VBox() : BoxLayout(Direction::Vertical) {} };

// rows of "label: field" (labels right-aligned in a column, as QFormLayout on Windows)
class FormLayout : public Layout {
public:
    void addRow(Widget* label, Widget* field);   // label may be nullptr (field spans)
    void addRow(const std::string& label, Widget* field);  // makes a Label child of the owner
    void addRow(Widget* spanning);
    void addRow(const std::string& label, std::unique_ptr<Layout> field);
    void apply(Widget* owner, const Rect& area) override;
    Point sizeHint() const override;
    Point minimumSize() const override;
    int expandingDirections() const override;
    void forgetWidget(Widget* w) override;
    void adoptItems() override;
    struct Row { Widget* label = nullptr; Widget* field = nullptr; std::unique_ptr<Layout> fieldLayout; };
    std::vector<Row> rows;
    int labelAlign = AlignLeft | AlignVCenter;
};

class GridLayout : public Layout {
public:
    GridLayout();  // its spacings: the stylesheets' --grid-spacing (6 px)
    void addWidget(Widget* w, int row, int col, int rowSpan = 1, int colSpan = 1, int align = 0);
    void setColumnStretch(int col, int stretch);
    void setRowStretch(int row, int stretch);
    void apply(Widget* owner, const Rect& area) override;
    Point sizeHint() const override;
    Point minimumSize() const override;
    int expandingDirections() const override;
    void forgetWidget(Widget* w) override;
    void adoptItems() override;
    struct Cell { Widget* w; int row, col, rowSpan, colSpan, align; };
    std::vector<Cell> cells;
    std::vector<int> colStretch, rowStretch;
    int hSpacing = 6, vSpacing = 6;
};

// helpers: make the layout and install it on `w`, returning it
HBox* hbox(Widget* w, int margin = 0, int spacing = 4);
VBox* vbox(Widget* w, int margin = 0, int spacing = 4);
FormLayout* form(Widget* w, int margin = 0, int spacing = 6);
GridLayout* grid(Widget* w, int margin = 0, int spacing = 6);

// A layout's room as a style gives it (a dialog's own, or a part of it: default.css): its margins are the style's
// padding, its spacing the row-gap (a column; a form: between its rows and after its labels) or the column-gap (a
// row), a grid's both. What the style doesn't set stays as the layout had it. Returns l.
void styleRoomOf(Layout* l, const css::Style& s);
template <class L>
L* styleRoom(L* l, const css::Style& s) {
    styleRoomOf(l, s);
    return l;
}

}  // namespace ui
