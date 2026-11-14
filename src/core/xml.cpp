// A small XML parser (see xml.h).
#include "core/xml.h"

#include <cstring>
#include <unordered_set>

namespace xml {

namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
inline bool isNameChar(unsigned char c) {
    return c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == ':' || c == '-' || c == '.';
}
inline bool isNameStart(unsigned char c) {
    return c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}

bool allSpace(std::string_view s) {
    for (char c : s)
        if (!isSpace(c)) return false;
    return true;
}

}  // namespace

bool decodeEntities(std::string_view s, std::string& out) {
    out.clear();
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c != '&') {
            out += c;
            continue;
        }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) return false;
        std::string_view name = s.substr(i + 1, semi - i - 1);
        if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "amp") out += '&';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name.size() >= 2 && name[0] == '#') {
            uint32_t cp = 0;
            bool hex = name[1] == 'x';
            size_t k = hex ? 2 : 1;
            if (k >= name.size()) return false;
            for (; k < name.size(); k++) {
                char d = name[k];
                int v;
                if (d >= '0' && d <= '9') v = d - '0';
                else if (hex && d >= 'a' && d <= 'f') v = d - 'a' + 10;
                else if (hex && d >= 'A' && d <= 'F') v = d - 'A' + 10;
                else return false;
                cp = cp * (hex ? 16 : 10) + v;
                if (cp > 0x10FFFF) return false;
            }
            if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
            appendUtf8(out, cp);
        } else {
            return false;
        }
        i = semi;
    }
    return true;
}

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// --- the parser ---

struct Parser {
    Document& doc;
    const char* begin;
    const char* p;
    const char* end;
    std::string error;
    std::string scratch;

    struct Open {
        Element* element;
        std::vector<std::string_view> pieces;  // its character data so far
    };
    std::vector<Open> stack;

    Parser(Document& d, const std::string& s) : doc(d), begin(s.data()), p(s.data()), end(s.data() + s.size()) {}

    int lineAt(const char* at) const {
        int line = 1;
        for (const char* q = begin; q < at && q < end; q++)
            if (*q == '\n') line++;
        return line;
    }
    bool fail(const char* at, const std::string& what) {
        if (error.empty()) error = "line " + std::to_string(lineAt(at)) + ": " + what;
        return false;
    }
    bool startsWith(const char* s) const {
        size_t n = strlen(s);
        return (size_t)(end - p) >= n && memcmp(p, s, n) == 0;
    }
    void skipSpace() {
        while (p < end && isSpace(*p)) p++;
    }
    // a view of `raw` with entities decoded and line breaks normalised (attribute: whitespace -> spaces)
    bool decoded(std::string_view raw, bool attribute, std::string_view& out, const char* at) {
        bool plain = true;
        for (char c : raw)
            if (c == '&' || c == '\r' || (attribute && (c == '\n' || c == '\t'))) {
                plain = false;
                break;
            }
        if (plain) {
            out = raw;
            return true;
        }
        std::string norm;
        norm.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); i++) {
            char c = raw[i];
            if (c == '\r') {
                if (i + 1 < raw.size() && raw[i + 1] == '\n') i++;
                c = '\n';
            }
            if (attribute && (c == '\n' || c == '\t')) c = ' ';
            norm += c;
        }
        std::string text;
        if (norm.find('&') != std::string::npos) {
            if (!decodeEntities(norm, text)) return fail(at, "a malformed or unknown entity");
        } else {
            text = std::move(norm);
        }
        doc.decoded_.push_back(std::move(text));
        out = doc.decoded_.back();
        return true;
    }
    void addText(std::string_view piece) {
        if (!stack.empty() && !piece.empty()) stack.back().pieces.push_back(piece);
    }
    bool name(std::string_view& out) {
        const char* s = p;
        if (p >= end || !isNameStart((unsigned char)*p)) return fail(p, "a name was expected");
        while (p < end && isNameChar((unsigned char)*p)) p++;
        out = std::string_view(s, p - s);
        return true;
    }
    // past `close` (e.g. "-->"), or fail
    bool skipPast(const char* close, const char* what) {
        size_t n = strlen(close);
        const char* at = p;
        for (; p + n <= end; p++)
            if (memcmp(p, close, n) == 0) {
                p += n;
                return true;
            }
        p = end;
        return fail(at, std::string("unclosed ") + what);
    }
    bool doctype() {
        const char* at = p;
        p += 9;  // "<!DOCTYPE"
        int depth = 0;
        char quote = 0;
        for (; p < end; p++) {
            char c = *p;
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '[') {
                depth++;
            } else if (c == ']') {
                depth--;
            } else if (c == '>' && depth <= 0) {
                p++;
                return true;
            }
        }
        return fail(at, "unclosed DOCTYPE");
    }
    bool startTag() {
        const char* at = p;
        p++;  // '<'
        std::string_view tag;
        if (!name(tag)) return false;
        if (stack.empty() && doc.root_) return fail(at, "junk after the document element");
        doc.pool_.emplace_back();
        Element* e = &doc.pool_.back();
        e->tag = tag;
        for (;;) {
            const char* before = p;
            skipSpace();
            if (p >= end) return fail(at, "unclosed tag <" + std::string(tag) + ">");
            if (*p == '>' || *p == '/') break;
            if (p == before) return fail(p, "a space was expected between attributes");
            Attribute a;
            if (!name(a.name)) return false;
            skipSpace();
            if (p >= end || *p != '=') return fail(p, "'=' was expected after " + std::string(a.name));
            p++;
            skipSpace();
            if (p >= end || (*p != '"' && *p != '\'')) return fail(p, "a quoted value was expected");
            char q = *p++;
            const char* s = p;
            const char* close = (const char*)memchr(p, q, end - p);
            if (!close) return fail(s, "unclosed attribute value");
            std::string_view raw(s, close - s);
            if (memchr(raw.data(), '<', raw.size())) return fail(s, "'<' in an attribute value");
            p = close + 1;
            if (!decoded(raw, true, a.value, s)) return false;
            for (auto& other : e->attributes)
                if (other.name == a.name) return fail(s, "duplicate attribute " + std::string(a.name));
            e->attributes.push_back(a);
        }
        bool empty = *p == '/';
        if (empty) {
            p++;
            if (p >= end || *p != '>') return fail(p, "'>' was expected");
        }
        p++;
        if (stack.empty()) {
            doc.root_ = e;
        } else {
            e->parent = stack.back().element;
            e->parent->children.push_back(e);
        }
        if (!empty) stack.push_back({e, {}});
        return true;
    }
    void finish(Open& open) {
        Element* e = open.element;
        // whitespace between child elements is indentation, left out
        std::vector<std::string_view>& pieces = open.pieces;
        if (!e->children.empty()) {
            bool blank = true;
            for (auto& s : pieces)
                if (!allSpace(s)) blank = false;
            if (blank) pieces.clear();
        }
        if (pieces.size() == 1) {
            e->text = pieces[0];
        } else if (pieces.size() > 1) {
            std::string joined;
            for (auto& s : pieces) joined += s;
            doc.decoded_.push_back(std::move(joined));
            e->text = doc.decoded_.back();
        }
    }
    bool endTag() {
        const char* at = p;
        p += 2;  // "</"
        std::string_view tag;
        if (!name(tag)) return false;
        skipSpace();
        if (p >= end || *p != '>') return fail(p, "'>' was expected");
        p++;
        if (stack.empty()) return fail(at, "an end tag </" + std::string(tag) + "> without its start");
        if (stack.back().element->tag != tag)
            return fail(at, "mismatched tag: </" + std::string(tag) + "> closes <" +
                                std::string(stack.back().element->tag) + ">");
        finish(stack.back());
        stack.pop_back();
        return true;
    }
    bool run() {
        if (end - p >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
            p += 3;
        while (p < end) {
            if (*p != '<') {
                const char* s = p;
                const char* lt = (const char*)memchr(p, '<', end - p);
                p = lt ? lt : end;
                std::string_view raw(s, p - s);
                if (stack.empty()) {
                    if (!allSpace(raw)) return fail(s, "text outside the document element");
                    continue;
                }
                std::string_view text;
                if (!decoded(raw, false, text, s)) return false;
                addText(text);
                continue;
            }
            if (startsWith("<!--")) {
                p += 4;
                if (!skipPast("-->", "comment")) return false;
            } else if (startsWith("<![CDATA[")) {
                const char* s = p;
                if (stack.empty()) return fail(s, "CDATA outside the document element");
                p += 9;
                const char* body = p;
                if (!skipPast("]]>", "CDATA section")) return false;
                addText(std::string_view(body, p - 3 - body));
            } else if (startsWith("<?")) {
                p += 2;
                if (!skipPast("?>", "processing instruction")) return false;
            } else if (startsWith("<!DOCTYPE")) {
                if (doc.root_) return fail(p, "a DOCTYPE after the document element");
                if (!doctype()) return false;
            } else if (startsWith("</")) {
                if (!endTag()) return false;
            } else {
                if (!startTag()) return false;
            }
        }
        if (!stack.empty())
            return fail(end, "unclosed element <" + std::string(stack.back().element->tag) + ">");
        if (!doc.root_) return fail(end, "no element found");
        return true;
    }
};

bool Document::parse(std::string text, std::string& error) {
    source_ = std::make_unique<std::string>(std::move(text));
    pool_.clear();
    decoded_.clear();
    root_ = nullptr;
    Parser parser(*this, *source_);
    if (!parser.run()) {
        error = parser.error;
        pool_.clear();
        decoded_.clear();
        root_ = nullptr;
        return false;
    }
    return true;
}

// --- Element ---

const std::string_view* Element::attr(std::string_view name) const {
    for (auto& a : attributes)
        if (a.name == name) return &a.value;
    return nullptr;
}

std::string_view Element::get(std::string_view name, std::string_view def) const {
    const std::string_view* v = attr(name);
    return v ? *v : def;
}

const Element* Element::child(std::string_view tag) const {
    for (const Element* c : children)
        if (c->tag == tag) return c;
    return nullptr;
}

namespace {

void descendants(const Element* e, std::vector<const Element*>& out) {  // (not e itself)
    for (const Element* c : e->children) {
        out.push_back(c);
        descendants(c, out);
    }
}

// one step of a path over `from`; first: stop at the first match (find)
void step(const std::vector<const Element*>& from, std::string_view seg, bool deep, std::vector<const Element*>& out) {
    out.clear();
    std::vector<const Element*> scope;
    for (const Element* e : from) {
        if (seg == ".") {
            if (deep) {  // ".//." : itself and all below
                out.push_back(e);
                descendants(e, out);
            } else {
                out.push_back(e);
            }
            continue;
        }
        if (deep) {
            scope.clear();
            descendants(e, scope);
            for (const Element* c : scope)
                if (seg == "*" || c->tag == seg) out.push_back(c);
        } else {
            for (const Element* c : e->children)
                if (seg == "*" || c->tag == seg) out.push_back(c);
        }
    }
    if (deep && from.size() > 1) {  // (overlapping subtrees: each element once, in document order)
        std::unordered_set<const Element*> seen;
        std::vector<const Element*> unique;
        for (const Element* e : out)
            if (seen.insert(e).second) unique.push_back(e);
        out.swap(unique);
    }
}

}  // namespace

std::vector<const Element*> Element::findAll(std::string_view path) const {
    std::vector<const Element*> current{this}, next;
    if (path.empty()) return {};
    if (path[0] == '/') return {};  // (ElementTree refuses absolute paths on an element)
    size_t i = 0;
    bool deep = false;
    while (i <= path.size()) {
        size_t slash = path.find('/', i);
        std::string_view seg = path.substr(i, slash == std::string_view::npos ? std::string_view::npos : slash - i);
        if (seg.empty()) {
            if (slash == std::string_view::npos) break;  // (a trailing "/")
            deep = true;
            i = slash + 1;
            continue;
        }
        step(current, seg, deep, next);
        current.swap(next);
        deep = false;
        if (current.empty() || slash == std::string_view::npos) break;
        i = slash + 1;
    }
    return current;
}

const Element* Element::find(std::string_view path) const {
    // fast path: plain "a/b/c" (the usual case) without making lists
    bool simple = !path.empty() && path.find("//") == std::string_view::npos && path.find('*') == std::string_view::npos &&
                  path[0] != '/';
    if (simple) {
        const Element* e = this;
        size_t i = 0;
        while (e) {
            size_t slash = path.find('/', i);
            std::string_view seg = path.substr(i, slash == std::string_view::npos ? std::string_view::npos : slash - i);
            if (seg != "." && !seg.empty()) {
                // the first child of that tag whose rest of the path matches
                std::string_view rest = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
                if (rest.empty()) return e->child(seg);
                for (const Element* c : e->children)
                    if (c->tag == seg)
                        if (const Element* found = c->find(rest)) return found;
                return nullptr;
            }
            if (slash == std::string_view::npos) return e;
            i = slash + 1;
        }
        return nullptr;
    }
    auto all = findAll(path);
    return all.empty() ? nullptr : all[0];
}

void Element::walk(const std::function<bool(const Element&)>& fn) const {
    // iterative, depth first, document order (deep documents don't overflow the stack)
    std::vector<std::pair<const Element*, size_t>> stack;
    if (!fn(*this)) return;
    stack.push_back({this, 0});
    while (!stack.empty()) {
        auto& top = stack.back();
        if (top.second >= top.first->children.size()) {
            stack.pop_back();
            continue;
        }
        const Element* c = top.first->children[top.second++];
        if (!fn(*c)) return;
        stack.push_back({c, 0});
    }
}

std::vector<const Element*> Element::iter(std::string_view tag) const {
    std::vector<const Element*> out;
    walk([&](const Element& e) {
        if (tag.empty() || e.tag == tag) out.push_back(&e);
        return true;
    });
    return out;
}

}  // namespace xml
