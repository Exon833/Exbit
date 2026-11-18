// CSS text to tokens to rules (css.h, css_internal.h).
//
// The tokenizer follows CSS Syntax 3 §4 closely (idents with escapes, hashes, strings, numbers with units and
// percentages, url(), comments dropped), byte by byte: anything from 0x80 up is a name character, so UTF-8
// passes through untouched. It never fails: what it can't read becomes a token the parser rejects
// (bad-string, bad-url, a delimiter), with a problem noted. Every opener ( [ { and function knows its closer,
// found once, so skipping a whole block is one step.
//
// The parser follows §5 the way browsers recover: an at-rule it doesn't know is skipped to its ';' or past its
// block; a rule whose selector can't be read is dropped whole (its block still ends where it should, so the
// next rule is safe); a declaration that can't be read is skipped to the next ';' (or past a nested block:
// nested rules aren't supported), and the rest of the block goes on. Shorthands are expanded here, except when
// their value uses var(): then each longhand keeps the shorthand's text and is expanded on the node (css.cpp).
// Each declaration notes the units that are worked out on the node (rem, vw, vh, ch), and the sheet whether
// it uses vw / vh at all.
//
// Selectors: :not() / :is() / :where() hold selector lists, parsed by the same code (with a depth); the
// combinators + and ~ and the place pseudo-classes mark the selector (Selector::uses, Engine::uses()).
// @media blocks are numbered (Stylesheet::media, each knowing the block it is in) and their rules carry the
// number: the engine decides which apply. @import parses the file there and then (its own folder as baseDir,
// the chain of files being read to catch loops) and moves its rules and @media blocks into this sheet, inside
// a block of the @import's own media list if it has one; its problems come before this sheet's own.
// @font-face reads its declarations as a rule's and keeps the family and the first usable file.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <tuple>

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

// --- paths (@import, @font-face's src) ---
// "a/./b/../c" -> "a/c" ("/" kept in front; ".." that can't go up stays)
std::string normalPath(const std::string& path) {
    bool absolute = !path.empty() && path[0] == '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string p = path.substr(i, j - i);
        if (p == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!absolute) parts.push_back(p);
        } else if (!p.empty() && p != ".") {
            parts.push_back(p);
        }
        i = j + 1;
    }
    std::string out = absolute ? "/" : "";
    for (size_t k = 0; k < parts.size(); k++) out += (k ? "/" : "") + parts[k];
    return out.empty() ? "." : out;
}
// a URL with a scheme (http:, data:…) other than file:
bool remote(const std::string& url) {
    size_t colon = url.find(':');
    if (colon == std::string::npos || colon < 2) return false;  // (C:\…: a drive)
    for (size_t i = 0; i < colon; i++) {
        char c = url[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i && ((c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-'))))
            return false;
    }
    return !ieq(url.substr(0, colon), "file");
}
// a url() of a stylesheet in `dir` as a path
std::string resolvePath(const std::string& dir, std::string url) {
    if (url.compare(0, 7, "file://") == 0) url = url.substr(7);
    else if (url.compare(0, 5, "file:") == 0) url = url.substr(5);
    if (url.empty()) return url;
    if (url[0] == '/' || dir.empty()) return normalPath(url);
    return normalPath(dir + "/" + url);
}
std::string folderOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    return slash == 0 ? "/" : path.substr(0, slash);
}
std::string fileOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::shared_ptr<Stylesheet> parseSheet(const std::string& text, const std::string& sourceName,
                                       std::vector<Problem>& problems, const ParseOptions& options,
                                       std::vector<std::string>& reading, int depth);

struct SheetParser {
    const Tokens& t;
    Stylesheet& sheet;
    std::vector<Problem>& problems;
    Lines lines;
    const ParseOptions& options;
    std::vector<std::string>& reading;   // the files being read (the @import chain to this one)
    int depth;                           // how deep in @imports this sheet is (0: the one parsed)
    std::vector<Problem>& importProblems;  // (what the imports found: they come before this sheet's own)
    size_t reported = 0;
    bool seenRule = false;  // (an @import must come before any rule)
    bool missingReported = false;
    int mediaDepth = 0;  // (@media inside @media…)
    bool inFontFace = false;  // (its src's url()s are its own: not images, not resolved here)

    SheetParser(const Tokens& tokens, Stylesheet& s, std::vector<Problem>& own, const ParseOptions& o,
                std::vector<std::string>& r, int d, std::vector<Problem>& imports)
        : t(tokens), sheet(s), problems(own), lines(tokens.src), options(o), reading(r), depth(d),
          importProblems(imports) {}

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
    // :nth-child()'s argument: an+b, odd, even
    static bool nth(const std::string& written, int& a, int& b, std::string& why) {
        std::string s;
        for (char ch : lower(written))
            if (ch != ' ') s += ch;
        why = "a bad argument for :nth-child(): '" + written + "'";
        if (s == "odd") return a = 2, b = 1, true;
        if (s == "even") return a = 2, b = 0, true;
        // [sign]digits from p (to the end or 'n'); false if there are none
        auto number = [&](size_t& p, long& v, bool needSign) {
            long sign = 1;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) sign = s[p++] == '-' ? -1 : 1;
            else if (needSign) return false;
            size_t start = p;
            long r = 0;
            for (; p < s.size() && isDigit(s[p]); p++) r = std::min(r * 10 + (s[p] - '0'), 1000000L);
            v = sign * r;
            return p > start;
        };
        size_t p = 0, n = s.find('n');
        long A = 0, B = 0;
        if (n == std::string::npos) {
            if (!number(p, B, false) || p != s.size()) return false;
        } else {
            std::string head = s.substr(0, n);
            if (head.empty() || head == "+") A = 1;
            else if (head == "-") A = -1;
            else if (!number(p, A, false) || p != n) return false;
            p = n + 1;
            if (p < s.size() && (!number(p, B, true) || p != s.size())) return false;
        }
        a = (int)A;
        b = (int)B;
        return true;
    }

    // a selector list inside :not() / :is() / :where()
    bool selectorArgs(size_t b, size_t e, std::vector<Selector>& out, size_t& errAt, std::string& why, int depth) {
        for (auto [sb, se] : splitTop(t, b, e, Tok::Comma)) {
            if (sb >= se) {
                errAt = sb < e ? sb : b;
                why = "an empty selector in a selector list";
                return false;
            }
            Selector s;
            if (!selector(sb, se, s, errAt, why, depth)) return false;
            out.push_back(std::move(s));
        }
        return true;
    }

    // a complex selector in [b, e); depth > 0: inside :not() / :is() / :where() (no part there)
    bool selector(size_t b, size_t e, Selector& sel, size_t& errAt, std::string& why, int depth = 0) {
        size_t i = b;
        unsigned ids = 0, classes = 0, types = 0;
        char nextCombinator = ' ';
        auto fail = [&](size_t at, std::string w) {
            errAt = at;
            why = std::move(w);
            return false;
        };
        for (;;) {
            Compound c;
            c.combinator = nextCombinator;
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
                        if (depth > 0) return fail(i, "a part (::" + t[i + 2].text + ") can't be inside :not(), :is() or :where()");
                        if (!sel.part.empty()) return fail(i, "a selector can have only one part");
                        sel.part = lower(t[i + 2].text);
                        types++;
                        i += 3;
                    } else if (i + 1 < e && t[i + 1].type == Tok::Ident) {
                        std::string st = lower(t[i + 1].text);
                        Pseudo::Kind kind = st == "first-child" ? Pseudo::FirstChild
                                          : st == "last-child"  ? Pseudo::LastChild
                                          : st == "only-child"  ? Pseudo::OnlyChild
                                                                : Pseudo::Is;
                        if (st == "root") {
                            c.root = true;
                        } else if (kind != Pseudo::Is) {
                            Pseudo p;
                            p.kind = kind;
                            c.pseudos.push_back(std::move(p));
                            sel.uses |= Engine::UsesPosition;
                        } else {
                            c.states.push_back(st);
                        }
                        classes++;
                        i += 2;
                    } else if (i + 1 < e && t[i + 1].type == Tok::Function) {
                        std::string fn = lower(t[i + 1].text);
                        size_t close = t[i + 1].close;
                        if (close >= e) return fail(i, "':" + t[i + 1].text + "(' is not closed");
                        Pseudo p;
                        if (fn == "not" || fn == "is" || fn == "where") {
                            if (depth >= 16) return fail(i, "':" + fn + "()' nested too deeply");
                            p.kind = fn == "not" ? Pseudo::Not : Pseudo::Is;
                            if (!selectorArgs(i + 2, close, p.args, errAt, why, depth + 1)) return false;
                            uint32_t most = 0;  // (the most specific argument counts; none for :where())
                            for (const Selector& a : p.args) {
                                most = std::max(most, a.specificity);
                                sel.uses |= a.uses;
                            }
                            if (fn != "where") {
                                ids += most >> 20;
                                classes += (most >> 10) & 1023;
                                types += most & 1023;
                            }
                        } else if (fn == "nth-child" || fn == "nth-last-child") {
                            p.kind = fn == "nth-child" ? Pseudo::NthChild : Pseudo::NthLastChild;
                            if (!nth(t.text(i + 2, close), p.a, p.b, why)) {
                                if (t.text(i + 2, close).find(" of ") != std::string::npos)
                                    why = "':" + fn + "(… of …)' is not supported";
                                errAt = i + 2 < close ? i + 2 : i;
                                return false;
                            }
                            sel.uses |= Engine::UsesPosition;
                            classes++;
                        } else {
                            return fail(i, "':" + t[i + 1].text + "()' is not supported");
                        }
                        c.pseudos.push_back(std::move(p));
                        i = close + 1;
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
            nextCombinator = ' ';
            if (t[i].type == Tok::Delim && (t[i].delim == '>' || t[i].delim == '+' || t[i].delim == '~')) {
                nextCombinator = t[i].delim;
                if (nextCombinator != '>') sel.uses |= Engine::UsesSiblings;
                i++;
                while (i < e && t[i].type == Tok::Whitespace) i++;
                if (i >= e) return fail(i, std::string("a selector ends with '") + nextCombinator + "'");
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
        // url()s: each file checked, and (a sheet from a file) resolved against its folder: the value is then read
        // from the text with those paths in
        Tokens resolved;
        const Tokens& t = urls(name, b, e, resolved) ? resolved : this->t;
        if (&t == &resolved) {
            b = 0;
            e = resolved.size();
            t.trim(b, e);
        }
        Decl d;
        d.important = important;
        d.source = &sheet.name;
        lines.at(nameTok.start, d.line, d.column);
        d.hasVar = usesVar(t, b, e);
        d.units = unitsOf(t, b, e);
        if (d.units & (UVw | UVh)) sheet.viewport = true;
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

    // the url()s of a declaration's value [b, e): each a file of the sheet (Stylesheet::images), checked
    // (ParseOptions::checkImage) when the sheet is from a file. With a baseDir, true and `out`: the value's text
    // with each url() as url("resolved path"), tokenized.
    bool urls(const std::string& property, size_t b, size_t e, Tokens& out) {
        if (inFontFace) return false;
        std::string text;
        size_t from = b < e ? t[b].start : 0;  // (the source's bytes copied up to here)
        bool changed = false;
        for (size_t i = b; i < e; i++) {
            const Token& k = t[i];
            if (k.type != Tok::Url && !(k.type == Tok::Function && ieq(k.text, "url"))) continue;
            size_t end = k.type == Tok::Url ? i + 1 : std::min((size_t)k.close + 1, e);
            std::string url;
            if (!urlAt(t, i, end, url)) {
                i = end - 1;
                continue;
            }
            if (remote(url)) {
                problem(k.start, "'" + property + "': url(" + url + "): only files can be used (skipped)");
                i = end - 1;
                continue;
            }
            std::string path = resolvePath(options.baseDir, url);
            if (std::find(sheet.images.begin(), sheet.images.end(), path) == sheet.images.end()) sheet.images.push_back(path);
            if (!options.baseDir.empty() || options.checkImage) {
                std::string why;
                bool ok;
                if (options.checkImage) {
                    ok = options.checkImage(path, why);
                } else {
                    FILE* f = fopen(path.c_str(), "rb");
                    ok = f != nullptr;
                    if (f) fclose(f);
                    if (!ok) why = "it can't be opened";
                }
                if (!ok)
                    problem(k.start, "'" + property + "': can't use " + url + " (" + path + "): " +
                                         (why.empty() ? std::string("it can't be read") : why) + " (left out when drawn)");
            }
            if (!options.baseDir.empty() && path != url) {
                text.append(t.src.data() + from, k.start - from);
                text += "url(\"";
                for (char ch : path) {
                    if (ch == '"' || ch == '\\') text += '\\';
                    if (ch == '\n') {
                        text += "\\a ";
                        continue;
                    }
                    text += ch;
                }
                text += "\")";
                from = t[end - 1].end;
                changed = true;
            }
            i = end - 1;
        }
        if (!changed) return false;
        text.append(t.src.data() + from, t[e - 1].end - from);
        out.problems.clear();
        // (the text outlives the tokens: kept with them)
        out.own = std::make_shared<std::string>(std::move(text));
        tokenize(*out.own, out);
        return true;
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

    // --- @media ---
    // the value of a width / height (px; em and rem are 16px, as in CSS) or a resolution (dppx)
    bool mediaValue(MediaFeature::Name name, size_t b, size_t e, double& v, std::string& why) {
        t.trim(b, e);
        if (b >= e) {
            why = "a media feature without a value";
            return false;
        }
        if (name == MediaFeature::Resolution) {
            const Token& k = t[b];
            if (e == b + 1 && k.type == Tok::Dimension) {
                if (k.text == "dppx" || k.text == "x") return v = k.num, true;
                if (k.text == "dpi") return v = k.num / 96, true;
                if (k.text == "dpcm") return v = k.num * 2.54 / 96, true;
            }
            why = "a resolution is in dppx, x, dpi or dpcm, not '" + shown(b, e) + "'";
            return false;
        }
        if (unitsOf(t, b, e) & (UVw | UVh | UCh)) {
            why = "vw, vh and ch can't be used in a media query ('" + shown(b, e) + "')";
            return false;
        }
        LenCtx ctx;
        ctx.em = ctx.rem = 16;
        ctx.percent = false;
        if (evalLength(t, b, e, ctx, v)) return true;
        why = "'" + shown(b, e) + "' is not a length";
        return false;
    }

    // a (feature) of a media query: [b, e) inside its parentheses
    bool mediaFeature(size_t b, size_t e, MediaFeature& f, size_t& at, std::string& why) {
        t.trim(b, e);
        at = b;
        // the parts: values and the feature's name, with < <= > >= = between them
        struct Part {
            size_t b, e;
            std::string op;  // (an operator: its text)
        };
        std::vector<Part> parts;
        size_t colon = e;
        for (size_t i = b; i < e;) {
            const Token& k = t[i];
            if (k.type == Tok::Whitespace) {
                i++;
            } else if (k.type == Tok::Colon) {
                colon = i;
                break;
            } else if (k.type == Tok::Delim && (k.delim == '<' || k.delim == '>' || k.delim == '=')) {
                std::string op(1, k.delim);
                if (k.delim != '=' && i + 1 < e && t[i + 1].type == Tok::Delim && t[i + 1].delim == '=') {
                    op += '=';
                    i++;
                }
                parts.push_back({i, i + 1, op});
                i++;
            } else {
                size_t j = i;
                while (j < e && t[j].type != Tok::Whitespace && t[j].type != Tok::Colon &&
                       !(t[j].type == Tok::Delim && (t[j].delim == '<' || t[j].delim == '>' || t[j].delim == '=')))
                    j = t.skip(j, e);
                parts.push_back({i, j, ""});
                i = j;
            }
        }
        auto nameOf = [&](const Part& p, std::string& n) {
            if (!p.op.empty() || p.e != p.b + 1 || t[p.b].type != Tok::Ident) return false;
            n = lower(t[p.b].text);
            return true;
        };
        auto known = [&](const std::string& n, MediaFeature::Name& out, int& minMax) {
            minMax = 0;
            std::string base = n;
            if (n.compare(0, 4, "min-") == 0) minMax = -1, base = n.substr(4);
            else if (n.compare(0, 4, "max-") == 0) minMax = 1, base = n.substr(4);
            if (base == "width") out = MediaFeature::Width;
            else if (base == "height") out = MediaFeature::Height;
            else if (base == "resolution") out = MediaFeature::Resolution;
            else if (base == "orientation" && !minMax) out = MediaFeature::Orientation;
            else return false;
            return true;
        };
        std::string name;
        int minMax = 0;
        if (colon < e) {  // (name: value)
            if (parts.size() != 1 || !nameOf(parts[0], name)) {
                why = "expected a media feature's name before ':'";
                return false;
            }
            if (!known(name, f.name, minMax)) {
                why = "unknown media feature '" + name + "'";
                return false;
            }
            if (f.name == MediaFeature::Orientation) {
                size_t vb = colon + 1, ve = e;
                t.trim(vb, ve);
                if (ve == vb + 1 && t[vb].type == Tok::Ident && (ieq(t[vb].text, "portrait") || ieq(t[vb].text, "landscape"))) {
                    f.orientation = ieq(t[vb].text, "portrait") ? 1 : 2;
                    return true;
                }
                at = vb < e ? vb : at;
                why = "orientation is portrait or landscape, not '" + shown(vb, ve) + "'";
                return false;
            }
            double v;
            if (!mediaValue(f.name, colon + 1, e, v, why)) return false;
            if (minMax <= 0) f.lo = v;
            if (minMax >= 0) f.hi = v;
            return true;
        }
        if (parts.size() == 1) {  // (name): true unless it is 0 (or "none")
            if (!nameOf(parts[0], name) || !known(name, f.name, minMax) || minMax) {
                why = name.empty() ? "expected a media feature, not '" + shown(b, e) + "'" : "unknown media feature '" + name + "'";
                return false;
            }
            if (f.name != MediaFeature::Orientation) {
                f.lo = 0;
                f.loStrict = true;
            }
            return true;
        }
        // ranges: name op value, value op name, value op name op value
        bool nameFirst = parts.size() == 3 && nameOf(parts[0], name);
        if (!nameFirst) {
            if ((parts.size() != 3 && parts.size() != 5) || !nameOf(parts[2], name)) {
                why = "a media feature that can't be read: '" + shown(b, e) + "'";
                return false;
            }
        }
        if (!known(name, f.name, minMax) || minMax || f.name == MediaFeature::Orientation) {
            why = "unknown media feature '" + name + "'";
            if (!minMax && f.name == MediaFeature::Orientation) why = "orientation can't be a range";
            return false;
        }
        // each operator with its value; `valueLeft`: the value is left of the name
        auto apply = [&](const Part& opPart, const Part& value, bool valueLeft) {
            const std::string& op = opPart.op;
            if (op.empty() || !value.op.empty()) {
                why = "a media feature that can't be read: '" + shown(b, e) + "'";
                return false;
            }
            double v;
            if (!mediaValue(f.name, value.b, value.e, v, why)) return false;
            char c = op[0];
            if (valueLeft && c != '=') c = c == '<' ? '>' : '<';  // (600px < width: width > 600px)
            bool strict = op.size() == 1;
            if (c == '=') {
                f.lo = f.hi = v;
            } else if (c == '>') {
                f.lo = v;
                f.loStrict = strict;
            } else {
                f.hi = v;
                f.hiStrict = strict;
            }
            return true;
        };
        if (nameFirst) return apply(parts[1], parts[2], false);
        if (!apply(parts[1], parts[0], true)) return false;
        if (parts.size() == 5) {
            if (parts[1].op[0] == '=' || parts[3].op[0] == '=' || parts[1].op[0] != parts[3].op[0]) {
                why = "a range's two comparisons must point the same way: '" + shown(b, e) + "'";
                return false;
            }
            return apply(parts[3], parts[4], false);
        }
        return true;
    }

    bool mediaQuery(size_t b, size_t e, MediaQuery& q, size_t& at, std::string& why) {
        auto comps = components(t, b, e);
        at = b;
        if (comps.empty()) {
            why = "an empty media query";
            return false;
        }
        auto word = [&](size_t k) -> std::string {
            if (k >= comps.size() || comps[k].second != comps[k].first + 1 || t[comps[k].first].type != Tok::Ident) return "";
            return lower(t[comps[k].first].text);
        };
        size_t k = 0;
        bool typed = false;
        std::string w = word(0);
        if (w == "not" || w == "only") {
            q.negated = w == "not";
            k = 1;
            if (word(1).empty() && (w == "only" || k >= comps.size())) {
                at = k < comps.size() ? comps[k].first : b;
                why = "expected a media type after '" + w + "'";
                return false;
            }
            w = word(1);
        }
        if (!w.empty() && w != "and" && w != "or") {
            typed = true;
            // (all and screen are what the program is; print, speech… never)
            if (w != "all" && w != "screen") q.never = true;
            k++;
        }
        bool needAnd = typed;
        for (; k < comps.size(); k++) {
            auto [cb, ce] = comps[k];
            if (needAnd) {
                std::string a = word(k);
                if (a != "and") {
                    at = cb;
                    why = a == "or" ? "'or' in media queries is not supported (use a comma)"
                                    : "expected 'and' in a media query, not '" + shown(cb, ce) + "'";
                    return false;
                }
                if (++k >= comps.size()) {
                    why = "a media query ends with 'and'";
                    at = cb;
                    return false;
                }
                std::tie(cb, ce) = comps[k];
            }
            if (t[cb].type != Tok::LParen || t[cb].close != ce - 1) {
                at = cb;
                why = "expected a (feature) in a media query, not '" + shown(cb, ce) + "'";
                return false;
            }
            MediaFeature f;
            if (!mediaFeature(cb + 1, ce - 1, f, at, why)) return false;
            q.features.push_back(f);
            needAnd = true;
        }
        return true;
    }

    // a query list ("" none: always); a query that can't be read is false (reported)
    std::vector<MediaQuery> mediaQueries(size_t b, size_t e) {
        std::vector<MediaQuery> out;
        t.trim(b, e);
        if (b >= e) return out;
        for (auto [qb, qe] : splitTop(t, b, e, Tok::Comma)) {
            MediaQuery q;
            size_t at = qb;
            std::string why;
            if (!mediaQuery(qb, qe, q, at, why)) {
                problem(offsetOf(at), why + " (that query is false)");
                q = MediaQuery();
                q.never = true;
            }
            out.push_back(std::move(q));
        }
        return out;
    }

    // --- @import ---
    void importRule(const Token& at, size_t b, size_t e, int media) {
        if (media >= 0) {
            problem(at.start, "@import can't be inside @media (skipped)");
            return;
        }
        if (seenRule) {
            problem(at.start, "@import must come before the rules (skipped)");
            return;
        }
        t.trim(b, e);
        std::string url;
        size_t k = b;
        if (k < e && (t[k].type == Tok::String || t[k].type == Tok::Url)) {
            url = t[k].text;
            k++;
        } else if (k < e && t[k].type == Tok::Function && ieq(t[k].text, "url") && t[k].close < e) {
            size_t ib = k + 1, ie = t[k].close;
            t.trim(ib, ie);
            if (ie == ib + 1 && t[ib].type == Tok::String) url = t[ib].text;
            k = t[k].close + 1;
        }
        if (url.empty()) {
            problem(at.start, "@import needs url(\"file\") or \"file\" (skipped)");
            return;
        }
        for (size_t i = k; i < e; i++)
            if ((t[i].type == Tok::Function || t[i].type == Tok::Ident) && (ieq(t[i].text, "layer") || ieq(t[i].text, "supports"))) {
                problem(t[i].start, "@import with " + t[i].text + " is not supported (skipped)");
                return;
            }
        std::vector<MediaQuery> queries = mediaQueries(k, e);
        if (!options.readFile) {
            problem(at.start, "@import of '" + url + "': files can't be read here (skipped)");
            return;
        }
        if (remote(url)) {
            problem(at.start, "@import of '" + url + "': only files can be imported (skipped)");
            return;
        }
        std::string path = resolvePath(options.baseDir, url);
        if (std::find(reading.begin(), reading.end(), path) != reading.end()) {
            problem(at.start, "@import of '" + url + "': a loop (it is being read already; skipped)");
            return;
        }
        if (depth + 1 > options.maxDepth) {
            problem(at.start, "@import of '" + url + "': imports nested more than " + std::to_string(options.maxDepth) +
                                  " deep (skipped)");
            return;
        }
        sheet.imports.push_back(path);
        std::string text;
        if (!options.readFile(path, text)) {
            problem(at.start, "@import of '" + url + "': can't read " + path + " (skipped)");
            return;
        }
        ParseOptions sub = options;
        sub.baseDir = folderOf(path);
        reading.push_back(path);
        std::shared_ptr<Stylesheet> child = parseSheet(text, fileOf(path), importProblems, sub, reading, depth + 1);
        reading.pop_back();
        // its rules and @media blocks here (in a block of the import's own media list, if it has one)
        int base = (int)sheet.media.size(), wrap = -1;
        if (!queries.empty()) {
            MediaBlock mb;
            mb.queries = std::move(queries);
            sheet.media.push_back(std::move(mb));
            wrap = base++;
        }
        for (MediaBlock& mb : child->media) {
            mb.parent = mb.parent >= 0 ? mb.parent + base : wrap;
            sheet.media.push_back(std::move(mb));
        }
        for (Rule& r : child->rules) {
            r.media = r.media >= 0 ? r.media + base : wrap;
            sheet.rules.push_back(std::move(r));
        }
        child->rules.clear();
        child->media.clear();
        sheet.imports.insert(sheet.imports.end(), child->imports.begin(), child->imports.end());
        for (const std::string& img : child->images)
            if (std::find(sheet.images.begin(), sheet.images.end(), img) == sheet.images.end()) sheet.images.push_back(img);
        sheet.fonts.insert(sheet.fonts.end(), child->fonts.begin(), child->fonts.end());
        sheet.viewport = sheet.viewport || child->viewport;
        sheet.imported.push_back(std::move(child));
    }

    // --- @font-face ---
    void fontFaceRule(const Token& at, size_t b, size_t e, int media) {
        if (media >= 0) {
            problem(at.start, "@font-face inside @media is not supported (skipped)");
            return;
        }
        Rule r;
        inFontFace = true;
        declarations(r, b, e);
        inFontFace = false;
        FontFace face;
        std::string src;
        for (const Decl& d : r.decls) {
            Tokens v;
            tokenize(d.value, v);
            size_t vb = 0, ve = v.size();
            v.trim(vb, ve);
            bool ident = ve == vb + 1 && v[vb].type == Tok::Ident;
            if (d.name == "font-family") {
                face.family = unquote(d.value);
            } else if (d.name == "src") {
                src = d.value;
            } else if (d.name == "font-weight") {
                face.bold = (ident && (ieq(v[vb].text, "bold") || ieq(v[vb].text, "bolder"))) ||
                            (vb < ve && v[vb].type == Tok::Number && v[vb].num >= 600);
            } else if (d.name == "font-style") {
                face.italic = vb < ve && v[vb].type == Tok::Ident && (ieq(v[vb].text, "italic") || ieq(v[vb].text, "oblique"));
            }
        }
        if (face.family.empty()) {
            problem(at.start, "@font-face without a font-family (skipped)");
            return;
        }
        // src: the first url() of a file that can be read (TrueType / OpenType; local() names are the system's)
        Tokens v;
        tokenize(src, v);
        for (auto [pb, pe] : splitTop(v, 0, v.size(), Tok::Comma)) {
            auto comps = components(v, pb, pe);
            if (comps.empty()) continue;
            const Token& u = v[comps[0].first];
            std::string url;
            if (u.type == Tok::Url) {
                url = u.text;
            } else if (u.type == Tok::Function && ieq(u.text, "url") && u.close < v.size()) {
                size_t ib = comps[0].first + 1, ie = u.close;
                v.trim(ib, ie);
                if (ie == ib + 1 && v[ib].type == Tok::String) url = v[ib].text;
            }
            if (url.empty() || remote(url)) continue;
            std::string format;
            for (size_t c = 1; c < comps.size(); c++) {
                const Token& f = v[comps[c].first];
                if (f.type == Tok::Function && ieq(f.text, "format") && f.close < v.size()) {
                    size_t ib = comps[c].first + 1, ie = f.close;
                    v.trim(ib, ie);
                    if (ib < ie) format = lower(v[ib].text);
                }
            }
            std::string ext = lower(url.substr(url.find_last_of('.') == std::string::npos ? url.size() : url.find_last_of('.')));
            bool readable = format.empty() ? ext != ".woff" && ext != ".woff2" && ext != ".eot" && ext != ".svg"
                                           : format == "truetype" || format == "opentype" || format == "collection" ||
                                                 format == "truetype-variations" || format == "opentype-variations";
            if (!readable) continue;
            face.file = resolvePath(options.baseDir, url);
            break;
        }
        if (face.file.empty()) {
            problem(at.start, "@font-face for '" + face.family + "' has no font file in its src that can be read (url() of a .ttf, .otf or .ttc; skipped)");
            return;
        }
        sheet.fonts.push_back(std::move(face));
    }

    // an at-rule among the rules at i (in @media block `media`): what it is, done; returns where the next starts
    size_t atRuleAt(size_t i, size_t e, int media) {
        const Token& k = t[i];
        size_t j = i + 1;
        while (j < e && t[j].type != Tok::Semicolon && t[j].type != Tok::LBrace) j = t.skip(j, e);
        bool block = j < e && t[j].type == Tok::LBrace;
        size_t next = j < e ? (block ? t.skip(j, e) : j + 1) : e;
        std::string name = lower(k.text);
        if (name == "charset") return next;
        if (name == "import") {
            if (block) problem(k.start, "@import with a { block (skipped)");
            else importRule(k, i + 1, j, media);
            return next;
        }
        seenRule = true;
        if ((name == "media" || name == "font-face") && !block) {
            problem(k.start, "@" + k.text + " without a { block (skipped)");
            return next;
        }
        if (name == "media") {
            if (mediaDepth >= 32) {
                problem(k.start, "@media nested too deeply (skipped)");
                return next;
            }
            int index = (int)sheet.media.size();
            MediaBlock mb;
            mb.queries = mediaQueries(i + 1, j);
            mb.parent = media;
            sheet.media.push_back(std::move(mb));
            size_t close = t[j].close;
            if (close >= t.size()) missingBrace();
            mediaDepth++;
            rules(j + 1, std::min(close, e), index);
            mediaDepth--;
            return next;
        }
        if (name == "font-face") {
            size_t close = t[j].close;
            fontFaceRule(k, j + 1, std::min(close, e), media);
            return next;
        }
        problem(k.start, "@" + k.text + " rules are not supported (skipped)");
        return next;
    }

    void missingBrace() {
        if (!missingReported) problem(t.src.size(), "missing '}' at the end");
        missingReported = true;
    }

    // the rules of [b, e) (in @media block `media`, -1 none)
    void rules(size_t b, size_t e, int media) {
        size_t i = b;
        while (i < e) {
            const Token& k = t[i];
            if (k.type == Tok::Whitespace || k.type == Tok::CDO || k.type == Tok::CDC) {
                i++;
                continue;
            }
            if (k.type == Tok::AtKeyword) {
                i = atRuleAt(i, e, media);
                continue;
            }
            seenRule = true;
            size_t j = i;
            while (j < e && t[j].type != Tok::LBrace) j = t.skip(j, e);
            if (j >= e) {
                problem(k.start, "a rule without a { block (ignored)");
                break;
            }
            size_t close = t[j].close;
            if (close >= t.size()) missingBrace();
            Rule rule;
            rule.media = media;
            size_t pe = j;
            size_t pb = i;
            t.trim(pb, pe);
            if (selectorList(pb, pe, rule.selectors)) {
                declarations(rule, j + 1, std::min(close, e));
                if (!rule.decls.empty()) sheet.rules.push_back(std::move(rule));
            }
            i = close >= e ? e : close + 1;
        }
    }

    void run() { rules(0, t.size(), -1); }
};

std::shared_ptr<Stylesheet> parseSheet(const std::string& text, const std::string& sourceName,
                                       std::vector<Problem>& problems, const ParseOptions& options,
                                       std::vector<std::string>& reading, int depth) {
    auto sheet = std::make_shared<Stylesheet>();
    sheet->name = sourceName;
    std::vector<Problem> own, fromImports;
    try {
        Tokens t;
        tokenize(text, t);
        SheetParser sp(t, *sheet, own, options, reading, depth, fromImports);
        for (auto& [offset, msg] : t.problems) sp.problem(offset, msg);
        sp.run();
        // (the tokenizer's problems came first: all in source order)
        std::stable_sort(own.begin(), own.end(), [](const Problem& a, const Problem& b) {
            return a.line != b.line ? a.line < b.line : a.column < b.column;
        });
    } catch (...) {  // (out of memory: what was read so far stands)
        Problem p;
        p.source = sourceName;
        p.message = "the stylesheet could not be read whole";
        own.push_back(std::move(p));
    }
    // (an import's problems come first, as its rules do)
    problems.insert(problems.end(), fromImports.begin(), fromImports.end());
    problems.insert(problems.end(), own.begin(), own.end());
    return sheet;
}

}  // namespace
}  // namespace detail

SheetPtr parse(const std::string& text, const std::string& sourceName, std::vector<Problem>& problems,
               const ParseOptions& options) {
    std::vector<std::string> reading;
    // (the sheet's own file, as far as it can be told, so that importing itself is a loop at once)
    if (!options.baseDir.empty() && !sourceName.empty()) reading.push_back(detail::resolvePath(options.baseDir, sourceName));
    try {
        return detail::parseSheet(text, sourceName, problems, options, reading, 0);
    } catch (...) {
        auto sheet = std::make_shared<Stylesheet>();
        sheet->name = sourceName;
        return sheet;
    }
}

std::vector<std::string> importedFiles(const SheetPtr& sheet) { return sheet ? sheet->imports : std::vector<std::string>(); }

std::vector<std::string> imageFiles(const SheetPtr& sheet) { return sheet ? sheet->images : std::vector<std::string>(); }

std::vector<FontFace> fontFaces(const SheetPtr& sheet) { return sheet ? sheet->fonts : std::vector<FontFace>(); }

}  // namespace css
