// CSS text to tokens to rules (css.h, css_internal.h).
//
// The tokenizer follows CSS Syntax 3 §4 closely (idents with escapes, hashes, strings, numbers with units and
// percentages, url(), comments dropped), byte by byte: anything from 0x80 up is a name character, so UTF-8
// passes through untouched. It never fails: what it can't read becomes a token the parser rejects
// (bad-string, bad-url, a delimiter), with a problem noted. Every opener ( [ { and function knows its closer,
// found once, so skipping a whole block is one step.
//
// The parser follows §5 the way browsers recover: an at-rule is skipped to its ';' or past its block; a rule
// whose selector can't be read is dropped whole (its block still ends where it should, so the next rule is
// safe); a declaration that can't be read is skipped to the next ';' (or past a nested block: nested rules
// aren't supported), and the rest of the block goes on. Shorthands are expanded here, except when their value
// uses var(): then each longhand keeps the shorthand's text and is expanded on the node (css.cpp).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/css_internal.h"

namespace css {
namespace detail {

namespace {

inline bool isDigit(int c) { return c >= '0' && c <= '9'; }
inline bool isHexDigit(int c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline int hexValue(int c) { return isDigit(c) ? c - '0' : (c | 0x20) - 'a' + 10; }
inline bool isNewline(int c) { return c == '\n' || c == '\r' || c == '\f'; }
inline bool isWhite(int c) { return c == ' ' || c == '\t' || isNewline(c); }
inline bool isNameStart(int c) { return c >= 0x80 || c == 0 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
inline bool isNameChar(int c) { return isNameStart(c) || isDigit(c) || c == '-'; }

void putUtf8(std::string& out, uint32_t cp) {
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

struct Lexer {
    std::string_view s;
    Tokens& out;
    size_t p = 0;

    int peek(size_t k = 0) const { return p + k < s.size() ? (unsigned char)s[p + k] : -1; }
    void problem(size_t at, const char* msg) { out.problems.emplace_back((uint32_t)at, msg); }

    bool validEscape(size_t k) const { return peek(k) == '\\' && !isNewline(peek(k + 1)); }
    bool startsIdent(size_t k) const {
        int c = peek(k);
        if (c == '-') {
            int n = peek(k + 1);
            return (n >= 0 && isNameStart(n)) || n == '-' || validEscape(k + 1);
        }
        if (c >= 0 && isNameStart(c)) return true;
        return c == '\\' && validEscape(k);
    }
    bool startsNumber(size_t k) const {
        int c = peek(k);
        if (c == '+' || c == '-') {
            int n = peek(k + 1);
            return isDigit(n) || (n == '.' && isDigit(peek(k + 2)));
        }
        if (c == '.') return isDigit(peek(k + 1));
        return isDigit(c);
    }
    // after the backslash
    void escape(std::string& to) {
        int c = peek();
        if (c < 0) {
            putUtf8(to, 0xFFFD);
            return;
        }
        if (isHexDigit(c)) {
            uint32_t cp = 0;
            for (int n = 0; n < 6 && isHexDigit(peek()); n++) cp = cp * 16 + hexValue(s[p++]);
            if (peek() == '\r' && peek(1) == '\n') p += 2;
            else if (isWhite(peek())) p++;
            if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
            putUtf8(to, cp);
            return;
        }
        to += s[p++];  // (a UTF-8 lead byte: its continuation bytes follow as name characters)
    }
    std::string name() {
        std::string n;
        for (;;) {
            int c = peek();
            if (c >= 0 && isNameChar(c)) {
                if (c == 0) putUtf8(n, 0xFFFD);
                else n += (char)c;
                p++;
            } else if (validEscape(0)) {
                p++;
                escape(n);
            } else {
                return n;
            }
        }
    }
    double number() {
        // read by hand, not strtod: that depends on the locale's decimal point
        double sign = 1;
        if (peek() == '+' || peek() == '-') sign = s[p++] == '-' ? -1 : 1;
        double v = 0;
        while (isDigit(peek())) v = v * 10 + (s[p++] - '0');
        if (peek() == '.' && isDigit(peek(1))) {
            p++;
            double scale = 0.1;
            while (isDigit(peek())) {
                v += (s[p++] - '0') * scale;
                scale *= 0.1;
            }
        }
        if ((peek() == 'e' || peek() == 'E') &&
            (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))))) {
            p++;
            int esign = 1;
            if (peek() == '+' || peek() == '-') esign = s[p++] == '-' ? -1 : 1;
            int e = 0;
            while (isDigit(peek())) {
                if (e < 10000) e = e * 10 + (s[p] - '0');
                p++;
            }
            v *= std::pow(10.0, esign * std::min(e, 400));
        }
        v *= sign;
        if (!std::isfinite(v)) v = sign * 1e300;  // (CSS clamps huge numbers too)
        return v;
    }
    void push(Token&& t) { out.list.push_back(std::move(t)); }

    void stringToken(size_t start) {
        int q = s[p++];
        Token t;
        t.type = Tok::String;
        for (;;) {
            int c = peek();
            if (c < 0) {
                problem(start, "unterminated string");
                break;
            }
            if (c == q) {
                p++;
                break;
            }
            if (isNewline(c)) {
                problem(start, "unterminated string (a line break in it)");
                t.type = Tok::BadString;
                break;
            }
            if (c == '\\') {
                if (peek(1) < 0) {
                    p++;
                } else if (isNewline(peek(1))) {  // a line continuation
                    p += (peek(1) == '\r' && peek(2) == '\n') ? 3 : 2;
                } else {
                    p++;
                    escape(t.text);
                }
                continue;
            }
            t.text += (char)c;
            p++;
        }
        t.start = (uint32_t)start;
        t.end = (uint32_t)p;
        push(std::move(t));
    }

    void urlToken(size_t start) {  // after "url("
        Token t;
        t.type = Tok::Url;
        while (isWhite(peek())) p++;
        for (;;) {
            int c = peek();
            if (c < 0) {
                problem(start, "unterminated url()");
                break;
            }
            if (c == ')') {
                p++;
                break;
            }
            if (isWhite(c)) {
                while (isWhite(peek())) p++;
                if (peek() == ')' || peek() < 0) continue;
                t.type = Tok::BadUrl;
            } else if (c == '"' || c == '\'' || c == '(' || c < 0x20 || c == 0x7F) {
                t.type = Tok::BadUrl;
            } else if (c == '\\') {
                if (validEscape(0)) {
                    p++;
                    escape(t.text);
                    continue;
                }
                t.type = Tok::BadUrl;
            } else {
                t.text += (char)c;
                p++;
                continue;
            }
            // a bad url: its remnants up to ')'
            problem(start, "bad url()");
            while (peek() >= 0 && peek() != ')') {
                if (validEscape(0)) {
                    p++;
                    std::string dummy;
                    escape(dummy);
                } else {
                    p++;
                }
            }
            if (peek() == ')') p++;
            break;
        }
        t.start = (uint32_t)start;
        t.end = (uint32_t)p;
        push(std::move(t));
    }

    void identLike(size_t start) {
        std::string n = name();
        Token t;
        if (ieq(n, "url") && peek() == '(') {
            p++;
            size_t q = p;
            while (isWhite(q < s.size() ? (unsigned char)s[q] : -1)) q++;
            int c = q < s.size() ? (unsigned char)s[q] : -1;
            if (c != '"' && c != '\'') {
                urlToken(start);
                return;
            }
            t.type = Tok::Function;
        } else if (peek() == '(') {
            p++;
            t.type = Tok::Function;
        } else {
            t.type = Tok::Ident;
        }
        t.text = std::move(n);
        t.start = (uint32_t)start;
        t.end = (uint32_t)p;
        push(std::move(t));
    }

    void numeric(size_t start) {
        Token t;
        t.num = number();
        if (startsIdent(0)) {
            t.type = Tok::Dimension;
            t.text = lower(name());
        } else if (peek() == '%') {
            p++;
            t.type = Tok::Percentage;
        } else {
            t.type = Tok::Number;
        }
        t.start = (uint32_t)start;
        t.end = (uint32_t)p;
        push(std::move(t));
    }

    void simple(Tok type, size_t len, char delim = 0) {
        Token t;
        t.type = type;
        t.delim = delim;
        t.start = (uint32_t)p;
        p += len;
        t.end = (uint32_t)p;
        push(std::move(t));
    }

    void run() {
        while (p < s.size()) {
            size_t start = p;
            int c = peek();
            if (c == '/' && peek(1) == '*') {
                size_t endc = s.find("*/", p + 2);
                if (endc == std::string_view::npos) {
                    problem(start, "unterminated comment");
                    p = s.size();
                } else {
                    p = endc + 2;
                }
                continue;
            }
            if (isWhite(c)) {
                while (isWhite(peek())) p++;
                Token t;
                t.type = Tok::Whitespace;
                t.start = (uint32_t)start;
                t.end = (uint32_t)p;
                push(std::move(t));
                continue;
            }
            switch (c) {
                case '"':
                case '\'': stringToken(start); continue;
                case '#':
                    if ((peek(1) >= 0 && isNameChar(peek(1))) || validEscape(1)) {
                        Token t;
                        t.type = Tok::Hash;
                        t.idHash = startsIdent(1);
                        p++;
                        t.text = name();
                        t.start = (uint32_t)start;
                        t.end = (uint32_t)p;
                        push(std::move(t));
                    } else {
                        simple(Tok::Delim, 1, '#');
                    }
                    continue;
                case '(': simple(Tok::LParen, 1); continue;
                case ')': simple(Tok::RParen, 1); continue;
                case '[': simple(Tok::LBracket, 1); continue;
                case ']': simple(Tok::RBracket, 1); continue;
                case '{': simple(Tok::LBrace, 1); continue;
                case '}': simple(Tok::RBrace, 1); continue;
                case ',': simple(Tok::Comma, 1); continue;
                case ':': simple(Tok::Colon, 1); continue;
                case ';': simple(Tok::Semicolon, 1); continue;
                case '+':
                case '.':
                    if (startsNumber(0)) numeric(start);
                    else simple(Tok::Delim, 1, (char)c);
                    continue;
                case '-':
                    if (startsNumber(0)) numeric(start);
                    else if (peek(1) == '-' && peek(2) == '>') simple(Tok::CDC, 3);
                    else if (startsIdent(0)) identLike(start);
                    else simple(Tok::Delim, 1, '-');
                    continue;
                case '<':
                    if (s.substr(p, 4) == "<!--") simple(Tok::CDO, 4);
                    else simple(Tok::Delim, 1, '<');
                    continue;
                case '@':
                    if (startsIdent(1)) {
                        p++;
                        Token t;
                        t.type = Tok::AtKeyword;
                        t.text = name();
                        t.start = (uint32_t)start;
                        t.end = (uint32_t)p;
                        push(std::move(t));
                    } else {
                        simple(Tok::Delim, 1, '@');
                    }
                    continue;
                case '\\':
                    if (validEscape(0)) {
                        identLike(start);
                    } else {
                        problem(start, "a backslash before a line break");
                        simple(Tok::Delim, 1, '\\');
                    }
                    continue;
                default: break;
            }
            if (isDigit(c)) numeric(start);
            else if (isNameStart(c)) identLike(start);
            else simple(Tok::Delim, 1, (char)c);
        }
    }
};

}  // namespace

void tokenize(std::string_view src, Tokens& out) {
    out.src = src;
    out.list.clear();
    out.problems.clear();
    if (src.size() > 0xFFFFFFF0u) src = src.substr(0, 0xFFFFFFF0u);  // (offsets are 32-bit)
    Lexer lx{src, out};
    lx.run();
    // every opener's closer; a closer that doesn't close the innermost open block is an ordinary token there
    size_t n = out.list.size();
    std::vector<uint32_t> open;
    for (size_t i = 0; i < n; i++) {
        Token& k = out.list[i];
        switch (k.type) {
            case Tok::Function:
            case Tok::LParen:
            case Tok::LBracket:
            case Tok::LBrace:
                k.close = (uint32_t)n;
                open.push_back((uint32_t)i);
                break;
            case Tok::RParen:
            case Tok::RBracket:
            case Tok::RBrace: {
                if (open.empty()) break;
                Tok o = out.list[open.back()].type;
                bool match = (k.type == Tok::RParen && (o == Tok::LParen || o == Tok::Function)) ||
                             (k.type == Tok::RBracket && o == Tok::LBracket) ||
                             (k.type == Tok::RBrace && o == Tok::LBrace);
                if (match) {
                    out.list[open.back()].close = (uint32_t)i;
                    open.pop_back();
                }
                break;
            }
            default: break;
        }
    }
}

std::string Tokens::text(size_t b, size_t e) const {
    trim(b, e);
    std::string r;
    for (size_t i = b; i < e; i++) {
        const Token& k = list[i];
        if (k.type == Tok::Whitespace) {
            r += ' ';
            continue;
        }
        // (a comment between two tokens: keep them apart)
        if (i > b && list[i - 1].type != Tok::Whitespace && list[i - 1].end != k.start) r += ' ';
        r.append(src.data() + k.start, k.end - k.start);
    }
    return r;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c += 32;
    return r;
}

std::vector<std::pair<size_t, size_t>> splitTop(const Tokens& t, size_t b, size_t e, Tok sep) {
    std::vector<std::pair<size_t, size_t>> r;
    size_t from = b;
    for (size_t i = b; i < e;) {
        if (t[i].type == sep) {
            size_t pb = from, pe = i;
            t.trim(pb, pe);
            r.emplace_back(pb, pe);
            from = ++i;
            continue;
        }
        i = t.skip(i, e);
    }
    size_t pb = from, pe = e;
    t.trim(pb, pe);
    r.emplace_back(pb, pe);
    return r;
}

std::vector<std::pair<size_t, size_t>> components(const Tokens& t, size_t b, size_t e) {
    std::vector<std::pair<size_t, size_t>> r;
    for (size_t i = b; i < e;) {
        if (t[i].type == Tok::Whitespace) {
            i++;
            continue;
        }
        size_t j = t.skip(i, e);
        r.emplace_back(i, j);
        i = j;
    }
    return r;
}

bool usesVar(const Tokens& t, size_t b, size_t e) {
    for (size_t i = b; i < e; i++)
        if (t[i].type == Tok::Function && ieq(t[i].text, "var")) return true;
    return false;
}

bool isCssWide(std::string_view text) {
    return ieq(text, "inherit") || ieq(text, "initial") || ieq(text, "unset") || ieq(text, "revert");
}

namespace {

// byte offsets to 1-based lines and columns (columns count characters, not UTF-8 bytes)
struct Lines {
    std::string_view s;
    std::vector<uint32_t> starts{0};
    size_t lastOffset = 0, lastLine = 0;
    int lastCol = 1;
    explicit Lines(std::string_view text) : s(text) {
        for (size_t i = 0; i < s.size(); i++) {
            char c = s[i];
            if (c == '\n' || c == '\f' || (c == '\r' && (i + 1 >= s.size() || s[i + 1] != '\n')))
                starts.push_back((uint32_t)(i + 1));
        }
    }
    void at(size_t offset, int& line, int& col) {
        offset = std::min(offset, s.size());
        size_t l = std::upper_bound(starts.begin(), starts.end(), (uint32_t)offset) - starts.begin();
        line = (int)l;
        // (problems come mostly in order: carry on counting from the last one on the same line)
        size_t from = starts[l - 1];
        int c = 1;
        if (l == lastLine && offset >= lastOffset) {
            from = lastOffset;
            c = lastCol;
        }
        for (size_t i = from; i < offset; i++)
            if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
        col = c;
        lastLine = l;
        lastOffset = offset;
        lastCol = c;
    }
};

const size_t kMaxProblems = 1000;

struct SheetParser {
    const Tokens& t;
    Stylesheet& sheet;
    std::vector<Problem>& problems;
    Lines lines;
    size_t reported = 0;

    void problem(size_t offset, std::string msg) {
        if (reported > kMaxProblems) return;
        Problem p;
        p.source = sheet.name;
        lines.at(offset, p.line, p.column);
        p.message = reported == kMaxProblems ? "too many problems; the rest aren't listed" : std::move(msg);
        reported++;
        problems.push_back(std::move(p));
    }
    size_t offsetOf(size_t i) const { return i < t.size() ? t[i].start : t.src.size(); }
    std::string shown(size_t b, size_t e) const {  // a short quote of tokens [b, e)
        std::string s = t.text(b, e);
        if (s.size() > 60) s = s.substr(0, 57) + "...";
        return s;
    }

    // --- selectors ---
    bool selector(size_t b, size_t e, Selector& sel, size_t& errAt, std::string& why) {
        size_t i = b;
        unsigned ids = 0, classes = 0, types = 0;
        bool nextChild = false;
        auto fail = [&](size_t at, std::string w) {
            errAt = at;
            why = std::move(w);
            return false;
        };
        for (;;) {
            Compound c;
            c.child = nextChild;
            bool any = false;
            if (i < e && t[i].type == Tok::Ident) {
                c.type = t[i].text;
                types++;
                any = true;
                i++;
            } else if (i < e && t[i].type == Tok::Delim && t[i].delim == '*') {
                any = true;
                i++;
            }
            for (;;) {
                if (i >= e) break;
                const Token& k = t[i];
                if (k.type == Tok::Hash) {
                    if (!k.idHash) return fail(i, "'#" + k.text + "' is not an id");
                    if (!sel.part.empty()) return fail(i, "only states can follow a part (::" + sel.part + ")");
                    c.ids.push_back(k.text);
                    ids++;
                    i++;
                } else if (k.type == Tok::Delim && k.delim == '.') {
                    if (i + 1 >= e || t[i + 1].type != Tok::Ident) return fail(i, "expected a class name after '.'");
                    if (!sel.part.empty()) return fail(i, "only states can follow a part (::" + sel.part + ")");
                    c.classes.push_back(t[i + 1].text);
                    classes++;
                    i += 2;
                } else if (k.type == Tok::Colon) {
                    if (i + 1 < e && t[i + 1].type == Tok::Colon) {
                        if (i + 2 >= e || t[i + 2].type != Tok::Ident) return fail(i, "expected a part name after '::'");
                        if (!sel.part.empty()) return fail(i, "a selector can have only one part");
                        sel.part = lower(t[i + 2].text);
                        types++;
                        i += 3;
                    } else if (i + 1 < e && t[i + 1].type == Tok::Ident) {
                        std::string st = lower(t[i + 1].text);
                        if (st == "root") c.root = true;
                        else c.states.push_back(st);
                        classes++;
                        i += 2;
                    } else if (i + 1 < e && t[i + 1].type == Tok::Function) {
                        return fail(i, "':" + t[i + 1].text + "()' is not supported");
                    } else {
                        return fail(i, "expected a state name after ':'");
                    }
                } else {
                    break;
                }
                any = true;
            }
            if (!any) return fail(i, i < e ? "unexpected '" + shown(i, i + 1) + "' in a selector" : "a selector is missing");
            sel.compounds.push_back(std::move(c));
            if (sel.compounds.size() > 32) return fail(i, "selector too long");
            bool ws = false;
            while (i < e && t[i].type == Tok::Whitespace) {
                ws = true;
                i++;
            }
            if (i >= e) break;
            nextChild = false;
            if (t[i].type == Tok::Delim && t[i].delim == '>') {
                nextChild = true;
                i++;
                while (i < e && t[i].type == Tok::Whitespace) i++;
                if (i >= e) return fail(i, "a selector ends with '>'");
            } else if (t[i].type == Tok::Delim && (t[i].delim == '+' || t[i].delim == '~')) {
                return fail(i, std::string("the '") + t[i].delim + "' combinator is not supported");
            } else if (!ws) {
                return fail(i, "unexpected '" + shown(i, i + 1) + "' in a selector");
            }
            if (!sel.part.empty()) return fail(i, "a part (::" + sel.part + ") must be on the last compound");
        }
        sel.specificity = std::min(ids, 1023u) << 20 | std::min(classes, 1023u) << 10 | std::min(types, 1023u);
        return true;
    }

    bool selectorList(size_t b, size_t e, std::vector<Selector>& out) {
        for (auto [sb, se] : splitTop(t, b, e, Tok::Comma)) {
            Selector sel;
            size_t errAt = 0;
            std::string why;
            if (sb >= se) {
                problem(offsetOf(sb < e ? sb : b), "an empty selector (the rule is skipped)");
                return false;
            }
            if (!selector(sb, se, sel, errAt, why)) {
                problem(offsetOf(errAt), why + " (the rule is skipped)");
                return false;
            }
            out.push_back(std::move(sel));
        }
        return true;
    }

    // --- declarations ---
    void declaration(Rule& rule, const Token& nameTok, size_t b, size_t e) {
        t.trim(b, e);
        bool important = false;
        if (e > b && t[e - 1].type == Tok::Ident && ieq(t[e - 1].text, "important")) {
            size_t j = e - 1;
            while (j > b && t[j - 1].type == Tok::Whitespace) j--;
            if (j > b && t[j - 1].type == Tok::Delim && t[j - 1].delim == '!') {
                important = true;
                e = j - 1;
                t.trim(b, e);
            }
        }
        const std::string& written = nameTok.text;
        bool custom = written.size() >= 2 && written[0] == '-' && written[1] == '-';
        std::string name = custom ? written : lower(written);
        for (size_t i = b; i < e; i++)
            if (t[i].type == Tok::BadString || t[i].type == Tok::BadUrl) {
                problem(nameTok.start, "a broken value for '" + name + "' (left out)");
                return;
            }
        for (size_t i = b; i < e; i = t.skip(i, e)) {
            Tok k = t[i].type;
            if (k == Tok::RParen || k == Tok::RBracket || k == Tok::RBrace) {
                problem(t[i].start, "an unmatched '" + shown(i, i + 1) + "' in the value of '" + name + "' (left out)");
                return;
            }
        }
        Decl d;
        d.important = important;
        d.source = &sheet.name;
        lines.at(nameTok.start, d.line, d.column);
        d.hasVar = usesVar(t, b, e);
        if (custom) {
            d.custom = true;
            d.name = name;
            d.value = t.text(b, e);
            rule.decls.push_back(std::move(d));
            return;
        }
        if (b >= e) {
            problem(nameTok.start, "no value for '" + name + "' (left out)");
            return;
        }
        std::string text = t.text(b, e);
        if (isShorthand(name)) {
            if (d.hasVar) {
                if (name == "background") {
                    Decl whole = d;
                    whole.name = name;
                    whole.value = text;
                    rule.decls.push_back(std::move(whole));
                }
                for (const std::string& l : longhands(name)) {
                    Decl x = d;
                    x.name = l;
                    x.value = text;
                    x.shorthand = name;
                    rule.decls.push_back(std::move(x));
                }
                return;
            }
            std::vector<std::pair<std::string, std::string>> parts;
            std::string why = expand(name, t, b, e, parts);
            if (!why.empty()) {
                problem(nameTok.start, why + " (left out)");
                return;
            }
            for (auto& [l, v] : parts) {
                Decl x = d;
                x.name = l;
                x.value = std::move(v);
                rule.decls.push_back(std::move(x));
            }
            return;
        }
        if (!d.hasVar) {
            std::string why = checkValue(name, t, b, e);
            if (!why.empty()) {
                problem(nameTok.start, why + " (left out)");
                return;
            }
        }
        d.name = std::move(name);
        d.value = std::move(text);
        rule.decls.push_back(std::move(d));
    }

    // skip an at-rule at i: to its ';' or past its block
    size_t atRule(size_t i, size_t e) {
        const Token& k = t[i];
        size_t j = i + 1;
        while (j < e && t[j].type != Tok::Semicolon && t[j].type != Tok::LBrace) j = t.skip(j, e);
        if (j < e) j = t[j].type == Tok::LBrace ? t.skip(j, e) : j + 1;
        if (!ieq(k.text, "charset")) problem(k.start, "@" + k.text + " rules are not supported (skipped)");
        return j;
    }

    void declarations(Rule& rule, size_t b, size_t e) {
        size_t i = b;
        while (i < e) {
            const Token& k = t[i];
            if (k.type == Tok::Whitespace || k.type == Tok::Semicolon) {
                i++;
                continue;
            }
            if (k.type == Tok::AtKeyword) {
                i = atRule(i, e);
                continue;
            }
            if (k.type == Tok::Ident) {
                size_t j = i + 1;
                while (j < e && t[j].type == Tok::Whitespace) j++;
                if (j < e && t[j].type == Tok::Colon) {
                    size_t m = j + 1;
                    while (m < e && t[m].type != Tok::Semicolon) m = t.skip(m, e);
                    declaration(rule, k, j + 1, m);
                    i = m + 1;
                    continue;
                }
            }
            // not a declaration: a nested rule (skipped whole) or something to skip to the next ';'
            size_t m = i;
            while (m < e && t[m].type != Tok::Semicolon && t[m].type != Tok::LBrace) m = t.skip(m, e);
            if (m < e && t[m].type == Tok::LBrace) {
                problem(k.start, "nested rules are not supported (skipped)");
                i = t.skip(m, e);
            } else {
                if (k.type == Tok::Ident) problem(k.start, "expected ':' after '" + k.text + "' (skipped)");
                else problem(k.start, "expected a declaration, not '" + shown(i, i + 1) + "' (skipped)");
                i = m + 1;
            }
        }
    }

    void run() {
        size_t n = t.size(), i = 0;
        while (i < n) {
            const Token& k = t[i];
            if (k.type == Tok::Whitespace || k.type == Tok::CDO || k.type == Tok::CDC) {
                i++;
                continue;
            }
            if (k.type == Tok::AtKeyword) {
                i = atRule(i, n);
                continue;
            }
            size_t j = i;
            while (j < n && t[j].type != Tok::LBrace) j = t.skip(j, n);
            if (j >= n) {
                problem(k.start, "a rule without a { block (ignored)");
                break;
            }
            size_t close = t[j].close;
            if (close >= n) problem(t.src.size(), "missing '}' at the end");
            Rule rule;
            size_t pe = j;
            size_t pb = i;
            t.trim(pb, pe);
            if (selectorList(pb, pe, rule.selectors)) {
                declarations(rule, j + 1, std::min(close, n));
                if (!rule.decls.empty()) sheet.rules.push_back(std::move(rule));
            }
            i = close >= n ? n : close + 1;
        }
    }
};

}  // namespace
}  // namespace detail

SheetPtr parse(const std::string& text, const std::string& sourceName, std::vector<Problem>& problems,
               const ParseOptions& /*options*/) {
    auto sheet = std::make_shared<Stylesheet>();
    sheet->name = sourceName;
    try {
        detail::Tokens t;
        detail::tokenize(text, t);
        size_t first = problems.size();
        detail::SheetParser sp{t, *sheet, problems, detail::Lines(t.src)};
        for (auto& [offset, msg] : t.problems) sp.problem(offset, msg);
        sp.run();
        // (the tokenizer's problems came first: all in source order)
        std::stable_sort(problems.begin() + first, problems.end(), [](const Problem& a, const Problem& b) {
            return a.line != b.line ? a.line < b.line : a.column < b.column;
        });
    } catch (...) {  // (out of memory: what was read so far stands)
        Problem p;
        p.source = sourceName;
        p.message = "the stylesheet could not be read whole";
        problems.push_back(std::move(p));
    }
    return sheet;
}

}  // namespace css
