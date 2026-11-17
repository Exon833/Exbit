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

const css::Style& Parts::get(const std::string& part, const std::string& states) {
    refresh();
    for (const Entry& e : entries_)
        if (e.part == part && e.states == states) return *e.style;
    StateNode node(owner_, states);
    // (a part inherits from the widget itself: its colour, font and variables; the widget in other states from its
    // parent, as the widget does)
    const ui::Widget* up = owner_.parent();
    const css::Style* base = !part.empty() ? &owner_.computedStyle() : up ? &up->computedStyle() : nullptr;
    Entry e;
    e.part = part;
    e.states = states;
    e.style = std::make_unique<css::Style>(theme::styles().compute(node, base, part));
    entries_.push_back(std::move(e));
    return *entries_.back().style;
}

}  // namespace panel_style
