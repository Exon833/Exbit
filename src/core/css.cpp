// The cascade (css.h): which rules match a node, in which order they apply, inheritance and var().
//
// MATCHING. setSheets() (and setMedia(), when the @media blocks that apply change) files every selector of
// the rules whose @media applies under its last compound's most telling simple selector: its id, else its
// first class, else its type, else with the rest (`*`, states, :not() / :is() / the place pseudo-classes
// only), and all of that per part (Slider::handle's rules are apart from Slider's). Rules whose @media doesn't
// apply aren't in the index at all: they cost nothing. compute() then looks only where the node can match:
// its id's list, the classes it has, the types it counts as, and the rest; each candidate is matched right to
// left. A descendant combinator that runs out of ancestors gives up on the whole selector (no other way of
// matching the compounds to its right can succeed either), and a ~ that runs out of siblings gives up on the
// earlier siblings (they see fewer), which keeps long selectors on deep trees and long sibling lists linear.
//
// MEDIA. Each sheet's @media blocks are evaluated against the Media (applying()); setMedia() rebuilds the
// index and bumps the generation only when that set changes, or when the size changes and a sheet uses vw /
// vh (their values were made px in computed styles).
//
// ORDER. The matching selectors sort by specificity, then by their place in the sheets (earlier sheets
// first, then source order); normal declarations apply in that order, later ones overwriting, then the
// !important ones the same way, over them.
//
// VALUES. The node starts with its parent's custom properties and inherited properties (css.h). Its own
// custom properties go on top, and those that use var() are resolved there, depth first: a property met
// again while it is being resolved closes a cycle, and every property on the cycle becomes invalid (it is
// then as if not set, so a var() of it takes its fallback). Then each property's winning declaration has its
// var()s substituted from the node's custom properties; one that can't be (no such property, no fallback) or
// whose result isn't a valid value is reported and left unset: inherited from the parent if it inherits,
// else not there. Shorthands whose value used var() are expanded now. Last, font-size becomes px (em and %
// of the parent's, rem of :root's, ch of the parent's font), then rem, vw, vh and ch in the other properties
// (and rem, vw, vh in the node's own custom properties) become px, letter-spacing in em becomes px, and a
// `color` that uses currentColor becomes the colour it is (the parent's colour is its currentColor), so what
// children inherit is already settled.
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/css_internal.h"

namespace css {

using namespace detail;

std::string Problem::text() const {
    return source + ":" + std::to_string(line) + ":" + std::to_string(column) + ": " + message;
}

bool SimpleNode::isType(const std::string& name) const {
    return name == typeName || std::find(types.begin(), types.end(), name) != types.end();
}
bool SimpleNode::hasClass(const std::string& name) const {
    return std::find(classes.begin(), classes.end(), name) != classes.end();
}
bool SimpleNode::hasState(const std::string& name) const {
    return std::find(states.begin(), states.end(), name) != states.end();
}

struct Engine::Index {
    struct Entry {
        const Selector* selector;
        const Rule* rule;
        uint64_t order;  // specificity << 32 | place in the sheets
    };
    struct Bucket {
        std::unordered_map<std::string, std::vector<uint32_t>> ids, classes, types;
        std::vector<uint32_t> rest;
    };
    std::vector<Entry> entries;
    std::unordered_map<std::string, Bucket> parts;  // "" for the node itself
};

namespace {

bool matches(const Selector& s, const Node& n);

// is `position` (1-based) a*k + b for some k >= 0?
bool nthMatches(long long a, long long b, long long position) {
    if (a == 0) return position == b;
    long long d = position - b;
    return d % a == 0 && d / a >= 0;
}

bool matchPseudo(const Pseudo& p, const Node& n) {
    switch (p.kind) {
        case Pseudo::Not:
            for (const Selector& s : p.args)
                if (matches(s, n)) return false;
            return true;
        case Pseudo::Is:
            for (const Selector& s : p.args)
                if (matches(s, n)) return true;
            return false;
        default: break;
    }
    int i = n.childIndex();
    if (i < 0) return false;
    int count = n.siblingCount();
    switch (p.kind) {
        case Pseudo::FirstChild: return i == 0;
        case Pseudo::LastChild: return i == count - 1;
        case Pseudo::OnlyChild: return i == 0 && count == 1;
        case Pseudo::NthChild: return nthMatches(p.a, p.b, i + 1);
        case Pseudo::NthLastChild: return nthMatches(p.a, p.b, (long long)count - i);
        default: return false;
    }
}

bool matchCompound(const Compound& c, const Node& n) {
    if (!c.type.empty() && !n.isType(c.type)) return false;
    if (c.root && n.parent()) return false;
    for (const std::string& id : c.ids)
        if (n.id() != id) return false;
    for (const std::string& cl : c.classes)
        if (!n.hasClass(cl)) return false;
    for (const std::string& st : c.states)
        if (!n.hasState(st)) return false;
    for (const Pseudo& p : c.pseudos)
        if (!matchPseudo(p, n)) return false;
    return true;
}

// Why matching failed, so that the searches around it can give up early and stay linear:
// - NoHere: not at this node;
// - NoSibling: not at this node, nor at any sibling before it (a ~ search that ran out of siblings: one from an
//   earlier sibling sees fewer of them; or the parent didn't match, which is every sibling's parent);
// - NoAnywhere: not at this node, nor at any of its ancestors or siblings (a descendant search that ran out of
//   ancestors: one from higher up sees fewer; siblings have the same ancestors).
enum class Match { Yes, NoHere, NoSibling, NoAnywhere };

const Node* previousSibling(const Node& n) {
    int i = n.childIndex();
    return i > 0 ? n.sibling(i - 1) : nullptr;
}

Match matchFrom(const Selector& s, size_t idx, const Node& n) {
    const Compound& c = s.compounds[idx];
    if (!matchCompound(c, n)) return Match::NoHere;
    if (idx == 0) return Match::Yes;
    switch (c.combinator) {
        case '>': {
            const Node* p = n.parent();
            if (!p) return Match::NoAnywhere;
            Match m = matchFrom(s, idx - 1, *p);
            return m == Match::Yes || m == Match::NoAnywhere ? m : Match::NoSibling;
        }
        case '+': {
            const Node* p = previousSibling(n);
            return p ? matchFrom(s, idx - 1, *p) : Match::NoSibling;
        }
        case '~':
            for (const Node* p = previousSibling(n); p; p = previousSibling(*p)) {
                Match m = matchFrom(s, idx - 1, *p);
                if (m != Match::NoHere) return m;
            }
            return Match::NoSibling;
        default:
            for (const Node* p = n.parent(); p; p = p->parent()) {
                Match m = matchFrom(s, idx - 1, *p);
                if (m == Match::Yes || m == Match::NoAnywhere) return m;
            }
            return Match::NoAnywhere;
    }
}

bool matches(const Selector& s, const Node& n) {
    return !s.compounds.empty() && matchFrom(s, s.compounds.size() - 1, n) == Match::Yes;
}

bool featureMatches(const MediaFeature& f, const Media& m) {
    double v = 0;
    switch (f.name) {
        case MediaFeature::Width: v = m.width; break;
        case MediaFeature::Height: v = m.height; break;
        case MediaFeature::Resolution: v = m.scale; break;
        case MediaFeature::Orientation: return f.orientation == 0 || (f.orientation == 1) == (m.height >= m.width);
    }
    if (f.loStrict ? !(v > f.lo) : !(v >= f.lo)) return false;
    if (f.hiStrict ? !(v < f.hi) : !(v <= f.hi)) return false;
    return true;
}

// (rem, vw, vh: see a value that might use them: a digit before an r or a v)
bool maybeUnits(const std::string& s) {
    for (size_t i = 0; i + 1 < s.size(); i++)
        if (s[i] >= '0' && s[i] <= '9') {
            char c = (char)(s[i + 1] | 0x20);
            if (c == 'r' || c == 'v') return true;
        }
    return false;
}

bool mentions(const std::string& text, const char* what) { return lower(text).find(what) != std::string::npos; }

}  // namespace

namespace detail {
bool mediaMatches(const MediaQuery& q, const Media& m) {
    if (q.never) return q.negated;
    bool all = true;
    for (const MediaFeature& f : q.features)
        if (!featureMatches(f, m)) {
            all = false;
            break;
        }
    return q.negated ? !all : all;
}
bool mediaMatches(const std::vector<MediaQuery>& list, const Media& m) {
    if (list.empty()) return true;
    for (const MediaQuery& q : list)
        if (mediaMatches(q, m)) return true;
    return false;
}
}  // namespace detail

void Engine::setSheets(std::vector<SheetPtr> sheets) {
    sheets_ = std::move(sheets);
    generation_++;
    root_.reset();
    problems_.clear();
    reported_.clear();
    active_ = applying();
    rebuild();
}

std::vector<char> Engine::applying() const {
    std::vector<char> on;
    for (const SheetPtr& sheet : sheets_) {
        if (!sheet) continue;
        size_t base = on.size();
        for (const MediaBlock& b : sheet->media)
            on.push_back(mediaMatches(b.queries, media_) && (b.parent < 0 || on[base + (size_t)b.parent]));
    }
    return on;
}

void Engine::setMedia(const Media& media) {
    bool resized = media.width != media_.width || media.height != media_.height;
    media_ = media;
    std::vector<char> now = applying();
    bool changed = now != active_;
    if (changed) {
        active_ = std::move(now);
        rebuild();
    }
    bool viewport = false;
    for (const SheetPtr& sheet : sheets_) viewport = viewport || (sheet && sheet->viewport);
    if (changed || (resized && viewport)) {
        generation_++;
        root_.reset();
    }
}

void Engine::setCharWidth(std::function<double(const std::string& family, double px)> width) {
    charWidth_ = std::move(width);
    generation_++;
    root_.reset();
}

void Engine::rebuild() {
    auto index = std::make_shared<Index>();
    uses_ = 0;
    uint32_t place = 0;
    size_t base = 0;
    for (const SheetPtr& sheet : sheets_) {
        if (!sheet) continue;
        for (const Rule& rule : sheet->rules) {
            if (rule.media >= 0 && !active_[base + (size_t)rule.media]) {
                place += (uint32_t)rule.selectors.size();
                continue;
            }
            for (const Selector& sel : rule.selectors) {
                if (sel.compounds.empty()) continue;
                uses_ |= sel.uses;
                uint32_t i = (uint32_t)index->entries.size();
                index->entries.push_back({&sel, &rule, (uint64_t)sel.specificity << 32 | place++});
                Index::Bucket& b = index->parts[sel.part];
                const Compound& last = sel.compounds.back();
                if (!last.ids.empty()) b.ids[last.ids[0]].push_back(i);
                else if (!last.classes.empty()) b.classes[last.classes[0]].push_back(i);
                else if (!last.type.empty()) b.types[last.type].push_back(i);
                else b.rest.push_back(i);
            }
        }
        base += sheet->media.size();
    }
    index_ = std::move(index);
}

const Style& Engine::root() const {
    if (!root_) {
        SimpleNode r;
        try {
            root_ = std::make_unique<Style>(computeWith(r, nullptr, ""));
        } catch (...) {
            root_ = std::make_unique<Style>();
        }
    }
    return *root_;
}

Style Engine::compute(const Node& node, const Style* parentStyle, const std::string& part) const {
    const Style* parent = parentStyle ? parentStyle : &root();
    try {
        return computeWith(node, parent, part);
    } catch (...) {  // (out of memory: what it inherits)
        Style s;
        s.vars_ = parent->vars_;
        s.emPx_ = parent->emPx_;
        return s;
    }
}

void Engine::checkVarUses() const {
    // the custom properties declared anywhere but in rules of :root alone: a var() of one of those may mean
    // something else on the nodes it is used on (not checked here)
    auto rootOnly = [](const Rule& rule) {
        for (const Selector& sel : rule.selectors) {
            if (sel.compounds.size() != 1 || !sel.part.empty()) return false;
            const Compound& c = sel.compounds[0];
            if (!c.root || !c.type.empty() || !c.ids.empty() || !c.classes.empty() || !c.states.empty() ||
                !c.pseudos.empty())
                return false;
        }
        return !rule.selectors.empty();
    };
    std::set<std::string> elsewhere;
    for (const SheetPtr& sheet : sheets_) {
        if (!sheet) continue;
        for (const Rule& rule : sheet->rules) {
            if (rootOnly(rule)) continue;
            for (const Decl& d : rule.decls)
                if (d.custom) elsewhere.insert(d.name);
        }
    }
    const std::map<std::string, std::string>& vars = root().variables();
    VarLookup lookup = [&](const std::string& n) -> const std::string* {
        auto it = vars.find(n);
        return it != vars.end() ? &it->second : nullptr;
    };
    auto report = [&](const Decl& d, const std::string& message) {
        Problem p;
        p.source = d.source ? *d.source : std::string();
        p.line = d.line;
        p.column = d.column;
        p.message = message;
        if (reported_.insert(p.text()).second) problems_.push_back(std::move(p));
    };
    size_t base = 0;
    for (const SheetPtr& sheet : sheets_) {
        if (!sheet) continue;
        for (const Rule& rule : sheet->rules) {
            if (rule.media >= 0 && !active_[base + (size_t)rule.media]) continue;
            for (const Decl& d : rule.decls) {
                if (!d.hasVar || d.custom) continue;
                Tokens names;
                tokenize(d.value, names);
                bool known = true;
                for (size_t i = 0; i < names.size() && known; i++)
                    if (names[i].type == Tok::Ident && elsewhere.count(names[i].text)) known = false;
                if (!known) continue;
                // (as computeWith does it, with the same messages: each reported once)
                std::string val, why;
                if (!substitute(d.value, lookup, val, why)) {
                    report(d, why + " ('" + d.name + "' is left unset)");
                    continue;
                }
                if (isCssWide(val)) continue;
                Tokens t;
                tokenize(val, t);
                if (!d.shorthand.empty()) {
                    std::vector<std::pair<std::string, std::string>> out;
                    why = expand(d.shorthand, t, 0, t.size(), out);
                } else {
                    why = checkValue(d.name, t, 0, t.size());
                }
                if (!why.empty()) report(d, why + " (after var(); '" + d.name + "' is left unset)");
            }
        }
        base += sheet->media.size();
    }
}

std::vector<Problem> Engine::takeProblems() const {
    std::vector<Problem> r;
    r.swap(problems_);
    return r;
}

Style Engine::computeWith(const Node& node, const Style* parent, const std::string& part) const {
    static const char* const kInherited[] = {"color",          "font-family", "font-size",
                                             "font-weight",    "font-style",  "letter-spacing",
                                             "font-smooth",    "-webkit-font-smoothing",
                                             "image-rendering"};
    Style st;
    if (parent) {
        st.emPx_ = parent->emPx_;
        st.vars_ = parent->vars_;
        for (const char* p : kInherited) {
            auto it = parent->props_.find(p);
            if (it != parent->props_.end()) st.props_.emplace(it->first, it->second);
        }
    }
    if (!index_) return st;
    const Index& ix = *index_;
    auto pit = ix.parts.find(part);
    if (pit == ix.parts.end()) return st;
    const Index::Bucket& bk = pit->second;

    // the rules that match, in cascade order
    std::vector<uint32_t> candidates;
    candidates.reserve(32);
    auto add = [&](const std::vector<uint32_t>& l) { candidates.insert(candidates.end(), l.begin(), l.end()); };
    if (!node.id().empty()) {
        auto it = bk.ids.find(node.id());
        if (it != bk.ids.end()) add(it->second);
    }
    for (const auto& [name, l] : bk.classes)
        if (node.hasClass(name)) add(l);
    for (const auto& [name, l] : bk.types)
        if (node.isType(name)) add(l);
    add(bk.rest);
    std::vector<std::pair<uint64_t, const Rule*>> matched;
    matched.reserve(candidates.size());
    for (uint32_t i : candidates) {
        const Index::Entry& e = ix.entries[i];
        if (matches(*e.selector, node)) matched.emplace_back(e.order, e.rule);
    }
    std::sort(matched.begin(), matched.end(),
              [](const std::pair<uint64_t, const Rule*>& a, const std::pair<uint64_t, const Rule*>& b) { return a.first < b.first; });

    // each property's winning declaration
    std::unordered_map<std::string_view, const Decl*> win, customWin;
    for (int important = 0; important < 2; important++)
        for (const auto& m : matched)
            for (const Decl& d : m.second->decls)
                if (d.important == (bool)important) (d.custom ? customWin : win)[d.name] = &d;
    if (win.empty() && customWin.empty()) return st;

    auto report = [&](const Decl& d, const std::string& message) {
        Problem p;
        p.source = d.source ? *d.source : std::string();
        p.line = d.line;
        p.column = d.column;
        p.message = message;
        if (reported_.insert(p.text()).second) problems_.push_back(std::move(p));
    };

    // custom properties: the node's own on top of the inherited ones, var() resolved depth first; a copy only
    // when it has some (else it shares its parent's)
    std::map<std::string, std::string> vars;
    if (!customWin.empty() && st.vars_) vars = *st.vars_;
    std::map<std::string, const Decl*> pending;
    for (const auto& [name, d] : customWin) {
        std::string key(name);
        if (isCssWide(d->value)) {
            if (ieq(d->value, "initial")) vars.erase(key);  // (inherit, unset, revert: the parent's, already there)
            continue;
        }
        vars[key] = d->value;
        if (d->hasVar) pending[key] = d;
    }
    if (!pending.empty()) {
        enum { InProgress = 1, Done, Invalid };
        std::map<std::string, int> state;
        std::vector<std::string> stack;
        std::set<std::string> cyclic;
        std::function<void(const std::string&)> resolve;
        VarLookup lookup = [&](const std::string& n) -> const std::string* {
            if (pending.count(n)) {
                resolve(n);
                if (state[n] != Done) return nullptr;
            }
            auto it = vars.find(n);
            return it != vars.end() ? &it->second : nullptr;
        };
        resolve = [&](const std::string& n) {
            int s = state[n];
            if (s == Done || s == Invalid) return;
            if (s == InProgress) {  // a cycle: everything from n up the stack is on it
                auto at = std::find(stack.begin(), stack.end(), n);
                for (; at != stack.end(); ++at) cyclic.insert(*at);
                return;
            }
            const Decl& d = *pending[n];
            if (stack.size() > 64) {
                state[n] = Invalid;
                report(d, "var() references nested too deeply (" + n + ")");
                vars.erase(n);
                return;
            }
            state[n] = InProgress;
            stack.push_back(n);
            std::string text = vars[n], out, why;
            bool ok = substitute(text, lookup, out, why);
            stack.pop_back();
            if (cyclic.count(n)) {
                state[n] = Invalid;
                report(d, "a var() cycle through " + n + " (it is left unset)");
                vars.erase(n);
            } else if (!ok) {
                state[n] = Invalid;
                report(d, why + " (" + n + " is left unset)");
                vars.erase(n);
            } else {
                state[n] = Done;
                vars[n] = std::move(out);
            }
        };
        for (const auto& p : pending) resolve(p.first);
    }

    // the properties
    const std::map<std::string, std::string>& variablesForProperties = customWin.empty() ? st.variables() : vars;
    VarLookup plain = [&](const std::string& n) -> const std::string* {
        auto it = variablesForProperties.find(n);
        return it != variablesForProperties.end() ? &it->second : nullptr;
    };
    auto unset = [&](const std::string& prop) {
        if (parent && isInherited(prop)) {
            auto it = parent->props_.find(prop);
            if (it != parent->props_.end()) {
                st.props_[prop] = it->second;
                return;
            }
        }
        st.props_.erase(prop);
    };
    std::vector<std::pair<std::string_view, uint8_t>> unitProps;  // (properties with rem, vw, vh, ch)
    for (const auto& [name, dp] : win) {
        const Decl& d = *dp;
        std::string prop(name), val, why;
        bool ok = true;
        uint8_t units = d.units;
        if (d.hasVar) {
            ok = substitute(d.value, plain, val, why);
            if (!ok) report(d, why + " ('" + prop + "' is left unset)");
        } else {
            val = d.value;
        }
        if (ok && isCssWide(val)) {
            if (ieq(val, "inherit")) {
                auto it = parent ? parent->props_.find(prop) : st.props_.end();
                if (parent && it != parent->props_.end()) st.props_[prop] = it->second;
                else st.props_.erase(prop);
            } else if (ieq(val, "initial")) {
                st.props_.erase(prop);
            } else {
                unset(prop);
            }
            continue;
        }
        if (ok && d.hasVar) {
            Tokens t;
            tokenize(val, t);
            units = unitsOf(t, 0, t.size());
            if (!d.shorthand.empty()) {
                std::vector<std::pair<std::string, std::string>> out;
                why = expand(d.shorthand, t, 0, t.size(), out);
                ok = why.empty();
                if (ok) {
                    ok = false;
                    for (auto& [l, v] : out)
                        if (l == prop) {
                            val = std::move(v);
                            ok = true;
                        }
                    if (ok && isCssWide(val)) {
                        unset(prop);
                        continue;
                    }
                }
            } else {
                why = checkValue(prop, t, 0, t.size());
                ok = why.empty();
            }
            if (!ok) report(d, why + " (after var(); '" + prop + "' is left unset)");
        }
        if (ok) {
            if (units) unitProps.emplace_back(name, units);
            st.props_[prop] = std::move(val);
        } else {
            unset(prop);
        }
    }

    // rem, vw, vh and ch made px here (font-size's first: its ch, and on the root its rem, are of the parent's
    // font), so that getters and children see lengths that don't depend on the node any more
    UnitPx upx;
    upx.vw = media_.width / 100;
    upx.vh = media_.height / 100;
    auto chOf = [&](const Style& s, double em) {
        std::vector<std::string> families = s.list("font-family");
        double w = charWidth_ ? charWidth_(families.empty() ? std::string() : families[0], em) : em * 0.5;
        return std::isfinite(w) && w >= 0 ? w : em * 0.5;
    };
    double baseEm = parent ? parent->emPx_ : st.emPx_;
    auto fs = st.props_.find("font-size");
    if (fs != st.props_.end()) {
        for (const auto& [p, u] : unitProps)
            if (p == "font-size") {
                upx.rem = parent ? root().emPx_ : baseEm;
                if (u & UCh) upx.ch = chOf(parent ? *parent : st, baseEm);
                fs->second = resolveUnits(fs->second, upx, u);
            }
        LenCtx ctx;
        ctx.em = baseEm;
        ctx.percentOf = baseEm;
        double px;
        if (lengthText(fs->second, ctx, px) && px >= 0) {
            fs->second = formatNumber(px) + "px";
            st.emPx_ = px;
        }
    }
    bool remSet = false, chSet = false;
    auto ownUnits = [&](std::string& text, uint8_t u) {
        if (!remSet) {
            upx.rem = parent ? root().emPx_ : st.emPx_;
            remSet = true;
        }
        if ((u & UCh) && !chSet) {
            upx.ch = chOf(st, st.emPx_);
            chSet = true;
        }
        text = resolveUnits(text, upx, u);
    };
    for (const auto& [p, u] : unitProps) {
        if (p == "font-size") continue;
        auto it = st.props_.find(std::string(p));
        if (it != st.props_.end()) ownUnits(it->second, u);
    }
    // (custom properties: rem, vw and vh, which are the same wherever the variable is used; ch is of the font of
    // the node that uses it)
    for (const auto& [name, d] : customWin) {
        auto it = vars.find(std::string(name));
        if (it == vars.end()) continue;
        uint8_t u = (d->hasVar ? (maybeUnits(it->second) ? (uint8_t)(URem | UVw | UVh) : 0) : d->units) & (URem | UVw | UVh);
        if (u) ownUnits(it->second, u);
    }
    if (!customWin.empty()) st.vars_ = std::make_shared<const std::map<std::string, std::string>>(std::move(vars));
    auto ls = st.props_.find("letter-spacing");
    if (ls != st.props_.end() && win.count("letter-spacing") && (mentions(ls->second, "em") || mentions(ls->second, "%"))) {
        LenCtx ctx;
        ctx.em = st.emPx_;
        ctx.percentOf = st.emPx_;
        double px;
        if (lengthText(ls->second, ctx, px)) ls->second = formatNumber(px) + "px";
    }
    auto col = st.props_.find("color");
    if (col != st.props_.end() && win.count("color") && mentions(col->second, "currentcolor")) {
        Rgba current, out;
        bool have = false;
        if (parent) {
            auto pc = parent->props_.find("color");
            have = pc != parent->props_.end() && colorText(pc->second, nullptr, current);
        }
        if (colorText(col->second, have ? &current : nullptr, out)) col->second = hex(out);
        else unset("color");
    }
    return st;
}

}  // namespace css
