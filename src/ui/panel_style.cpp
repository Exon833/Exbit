// What the panels, meters and editors draw, styled by the stylesheets (see panel_style.h).
#include "ui/panel_style.h"

#include "ui/theme.h"

namespace panel_style {

StateNode::StateNode(const ui::Widget& w, const std::string& states) : w_(w) {
    size_t i = 0;
    while (i < states.size()) {
        size_t j = states.find(' ', i);
        if (j == std::string::npos) j = states.size();
        if (j > i) states_.push_back(states.substr(i, j - i));
        i = j + 1;
    }
}

bool StateNode::hasState(const std::string& name) const {
    for (const std::string& s : states_)
        if (s == name) return true;
    return false;
}

bool Parts::refresh() {
    int gen = theme::styles().generation();
    if (gen == generation_ && owner_.parent() == parent_) return false;
    entries_.clear();
    generation_ = gen;
    parent_ = owner_.parent();
    return true;
}

const css::Style& Parts::get(const std::string& part, const std::string& states, const std::string& from) {
    refresh();
    const std::string& under = part.empty() ? part : from;  // (the widget itself inherits from its parent)
    for (const Entry& e : entries_)
        if (e.part == part && e.states == states && e.from == under) return *e.style;
    // (a part inherits from the widget itself: its colour, font and variables; the widget in other states from its
    // parent, as the widget does; or from the part named: kept as its own entry, its address stays put)
    const css::Style* base = nullptr;
    if (!under.empty()) {
        base = &get(under);
    } else {
        const ui::Widget* up = owner_.parent();
        base = !part.empty() ? &owner_.computedStyle() : up ? &up->computedStyle() : nullptr;
    }
    StateNode node(owner_, states);
    Entry e;
    e.part = part;
    e.states = states;
    e.from = under;
    e.style = std::make_unique<css::Style>(theme::styles().compute(node, base, part));
    entries_.push_back(std::move(e));
    return *entries_.back().style;
}

css::Style declared(const ui::Widget& w, const std::string& part) {
    const css::Style& own = w.computedStyle();
    css::Style vars;  // (only what var() and em need: no property to inherit)
    vars.vars_ = own.vars_;
    vars.emPx_ = own.emPx_;
    return theme::styles().compute(w, &vars, part);
}

}  // namespace panel_style
