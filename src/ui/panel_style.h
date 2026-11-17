// The stylesheets' look for what the panels, meters and editors draw that is not a widget of its own: a meter's
// bars, the piano roll's keys and notes, the patch panel's cables… (default.css's section "Panels, meters and
// editors"; docs/skins.md).
//
// Such a thing is a part of the widget that draws it (LevelMeter::bar, PianoRoll::key), in states of its own
// (LevelMeter::bar:warn, PianoRoll::key:black, PianoRoll::note:selected): only those states, not the widget's
// own (:hover…), so that what is drawn many times a frame is computed once. Parts takes them from the stylesheets
// once each and keeps them until the stylesheets change (theme::styles().generation()) or the widget is put
// somewhere else; the widgets that repaint at frame rate (meters, the scope, the CPU monitor, the piano roll)
// keep what they draw with worked out from them, again only when Parts::refresh() says so.
//
// Selectors match the part as the widget's own type, #id and classes, with the part's states: a skin can
// write `LevelMeter::bar:hot`, `#master LevelMeter::bar:hot`, or `PianoRoll:black::key` (the same).
//
// THREADS: the GUI thread (as everything that paints).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ui/toolkit/style.h"
#include "ui/toolkit/widget.h"

namespace panel_style {

// A widget as a node of the stylesheets, in states of its own instead of the widget's.
class StateNode : public css::Node {
public:
    StateNode(const ui::Widget& w, const std::string& states);  // states: names, space-separated
    const std::string& type() const override { return w_.type(); }
    bool isType(const std::string& name) const override { return w_.isType(name); }
    const std::string& id() const override { return w_.id(); }
    bool hasClass(const std::string& name) const override { return w_.hasClass(name); }
    bool hasState(const std::string& name) const override;
    const css::Node* parent() const override { return w_.parent(); }

private:
    const ui::Widget& w_;
    std::vector<std::string> states_;
};

// The computed styles of one widget's parts, each in the states asked for, kept (see the top).
class Parts {
public:
    explicit Parts(const ui::Widget& owner) : owner_(owner) {}
    Parts(const Parts&) = delete;
    Parts& operator=(const Parts&) = delete;
    // `owner::part:state…` ("" part: the widget itself in those states); the reference stays valid until the
    // next refresh() that answers true
    const css::Style& get(const std::string& part, const std::string& states = "");
    // drops what it keeps if the stylesheets changed or the widget has another parent since; true when it did
    // (and the first time): what the widget worked out from them is stale too
    bool refresh();
    void invalidate() { generation_ = -1; }

private:
    struct Entry {
        std::string part, states;
        std::unique_ptr<css::Style> style;
    };
    const ui::Widget& owner_;
    std::vector<Entry> entries_;
    int generation_ = -1;
    const ui::Widget* parent_ = nullptr;
};

// shorthands for what is read from a kept style
inline Color bg(const css::Style& s) { return ui::style::background(s); }
inline Color fg(const css::Style& s) { return ui::style::color(s, "color"); }
inline Color color(const css::Style& s, const char* property, Color def = Color(0, 0, 0, 0)) {
    return ui::style::color(s, property, def);
}
// a length in px, not rounded (line widths: 1.5px)
inline float lengthF(const css::Style& s, const char* property, double def) { return (float)s.length(property, def); }
inline int px(const css::Style& s, const char* property, int def) { return ui::style::px(s, property, def); }

}  // namespace panel_style
