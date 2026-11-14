#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace {
const Json kNull;
const std::string kEmpty;
const std::vector<Json> kNoItems;
const Json::Members kNoMembers;

void writeString(std::string& out, const std::string& s) {
    // as Python's json.dumps with ensure_ascii=False: only the control characters, " and \ escaped
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

void writeNumber(std::string& out, double v, bool isInt) {
    if (!std::isfinite(v)) {
        out += std::isnan(v) ? "NaN" : (v > 0 ? "Infinity" : "-Infinity");
        return;
    }
    char buf[40];
    if (isInt && std::fabs(v) < 9.007e15) {
        snprintf(buf, sizeof buf, "%lld", (long long)v);
        out += buf;
        return;
    }
    // the shortest text that reads back as the same double (as Python's repr: positional
    // notation from 1e-4 up to 1e16, exponents outside that)
    int prec = 1;
    for (; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
        if (strtod(buf, nullptr) == v) break;
    }
    double mag = std::fabs(v);
    if (mag == 0 || (mag >= 1e-4 && mag < 1e16)) {
        int exp10 = mag == 0 ? 0 : (int)std::floor(std::log10(mag));
        if (mag != 0 && std::pow(10.0, exp10) > mag) exp10--;  // (log10 rounding)
        int decimals = std::max(0, prec - 1 - exp10);
        snprintf(buf, sizeof buf, "%.*f", decimals, v);
    } else {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
        // Python writes 1e+16 / 1e-05: drop the exponent's leading zeros
        std::string t = buf;
        size_t e = t.find('e');
        if (e != std::string::npos) {
            std::string mant = t.substr(0, e), ex = t.substr(e + 1);
            char sign = ex[0];
            ex = ex.substr(1);
            while (ex.size() > 2 && ex[0] == '0') ex.erase(0, 1);
            snprintf(buf, sizeof buf, "%se%c%s", mant.c_str(), sign, ex.c_str());
        }
        out += buf;
        return;
    }
    out += buf;
    // keep it a float in the file ("2.0", not "2"), as Python writes floats
    if (!strpbrk(buf, ".eEn")) out += ".0";
}

struct Parser {
    const char* p;
    const char* end;
    std::string err;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool fail(const char* why) {
        if (err.empty()) err = why;
        return false;
    }
    static void putUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 63)); s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63)); }
    }
    bool hex4(uint32_t& v) {
        if (end - p < 4) return fail("bad \\u escape");
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool str(std::string& s) {
        p++;  // "
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { s += c; continue; }
            if (p >= end) return fail("unfinished string");
            char e = *p++;
            switch (e) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(s, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        if (p >= end) return fail("unfinished string");
        p++;
        return true;
    }
    bool value(Json& v, int depth) {
        if (depth > 500) return fail("nested too deeply");
        ws();
        if (p >= end) return fail("unexpected end");
        char c = *p;
        if (c == '{') {
            p++;
            v = Json::object();
            ws();
            if (p < end && *p == '}') { p++; return true; }
            for (;;) {
                ws();
                if (p >= end || *p != '"') return fail("expected a key");
                std::string key;
                if (!str(key)) return false;
                ws();
                if (p >= end || *p != ':') return fail("expected ':'");
                p++;
                Json item;
                if (!value(item, depth + 1)) return false;
                v.set(key, std::move(item));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            p++;
            v = Json::array();
            ws();
            if (p < end && *p == ']') { p++; return true; }
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) return false;
                v.push(std::move(item));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!str(s)) return false;
            v = Json(std::move(s));
            return true;
        }
        auto word = [&](const char* w) {
            size_t n = strlen(w);
            if ((size_t)(end - p) >= n && memcmp(p, w, n) == 0) { p += n; return true; }
            return false;
        };
        if (word("true")) { v = Json(true); return true; }
        if (word("false")) { v = Json(false); return true; }
        if (word("null")) { v = Json(); return true; }
        if (word("NaN")) { v = Json(NAN); return true; }
        if (word("Infinity")) { v = Json(INFINITY); return true; }
        if (word("-Infinity")) { v = Json(-INFINITY); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* s = p;
            bool isInt = true;
            if (*p == '-') p++;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
                if (*p == '.' || *p == 'e' || *p == 'E') isInt = false;
                p++;
            }
            std::string num(s, p);
            char* stop = nullptr;
            double d = strtod(num.c_str(), &stop);
            if (!stop || *stop) return fail("bad number");
            v = isInt ? Json((long long)d) : Json(d);
            if (isInt && std::fabs(d) >= 9.007e15) v = Json(d);
            return true;
        }
        return fail("unexpected character");
    }
};
}  // namespace

Json Json::array(std::vector<Json> items) {
    Json j;
    j.type_ = Array;
    j.a_ = std::make_shared<std::vector<Json>>(std::move(items));
    return j;
}

Json Json::object(Members members) {
    Json j;
    j.type_ = Object;
    j.o_ = std::make_shared<Members>(std::move(members));
    return j;
}

void Json::detach() {
    if (a_ && a_.use_count() > 1) a_ = std::make_shared<std::vector<Json>>(*a_);
    if (o_ && o_.use_count() > 1) o_ = std::make_shared<Members>(*o_);
}

const std::string& Json::toString() const { return type_ == String ? s_ : kEmpty; }

size_t Json::size() const {
    if (type_ == Array) return a_->size();
    if (type_ == Object) return o_->size();
    return 0;
}

const Json& Json::operator[](size_t i) const {
    if (type_ != Array || i >= a_->size()) return kNull;
    return (*a_)[i];
}

Json& Json::at(size_t i) {
    detach();
    return a_->at(i);
}

void Json::push(Json v) {
    if (type_ != Array) *this = array();
    detach();
    a_->push_back(std::move(v));
}

const std::vector<Json>& Json::items() const { return type_ == Array ? *a_ : kNoItems; }

std::vector<Json>& Json::items() {
    if (type_ != Array) *this = array();
    detach();
    return *a_;
}

bool Json::has(const std::string& key) const {
    if (type_ != Object) return false;
    for (auto& m : *o_)
        if (m.first == key) return true;
    return false;
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Object) return kNull;
    for (auto& m : *o_)
        if (m.first == key) return m.second;
    return kNull;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Object) *this = object();
    detach();
    for (auto& m : *o_)
        if (m.first == key) return m.second;
    o_->emplace_back(key, Json());
    return o_->back().second;
}

void Json::set(const std::string& key, Json v) { (*this)[key] = std::move(v); }

void Json::erase(const std::string& key) {
    if (type_ != Object) return;
    detach();
    for (auto it = o_->begin(); it != o_->end(); ++it)
        if (it->first == key) { o_->erase(it); return; }
}

const Json::Members& Json::members() const { return type_ == Object ? *o_ : kNoMembers; }

Json::Members& Json::members() {
    if (type_ != Object) *this = object();
    detach();
    return *o_;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        out += '\n';
        out.append((size_t)(indent * d), ' ');
    };
    switch (type_) {
        case Null: out += "null"; break;
        case Bool: out += b_ ? "true" : "false"; break;
        case Number: writeNumber(out, n_, isInt_); break;
        case String: writeString(out, s_); break;
        case Array: {
            if (a_->empty()) { out += "[]"; break; }
            out += '[';
            bool first = true;
            for (auto& item : *a_) {
                if (!first) out += indent < 0 ? ", " : ",";
                first = false;
                if (indent >= 0) newline(depth + 1);
                item.dumpTo(out, indent, depth + 1);
            }
            if (indent >= 0) newline(depth);
            out += ']';
            break;
        }
        case Object: {
            if (o_->empty()) { out += "{}"; break; }
            out += '{';
            bool first = true;
            for (auto& m : *o_) {
                if (!first) out += indent < 0 ? ", " : ",";
                first = false;
                if (indent >= 0) newline(depth + 1);
                writeString(out, m.first);
                out += ": ";
                m.second.dumpTo(out, indent, depth + 1);
            }
            if (indent >= 0) newline(depth);
            out += '}';
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

Json Json::parse(const std::string& text, std::string* error) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    // a UTF-8 byte order mark is allowed
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    Json v;
    if (!ps.value(v, 0)) {
        if (error) *error = ps.err.empty() ? "invalid JSON" : ps.err;
        return Json();
    }
    ps.ws();
    if (ps.p != ps.end) {
        if (error) *error = "extra data after the JSON value";
        return Json();
    }
    if (error) error->clear();
    return v;
}

bool Json::operator==(const Json& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Null: return true;
        case Bool: return b_ == o.b_;
        case Number: return n_ == o.n_;
        case String: return s_ == o.s_;
        case Array: return *a_ == *o.a_;
        case Object: return *o_ == *o.o_;
    }
    return false;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += kB64[v >> 18]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += kB64[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        out += kB64[v >> 18]; out += kB64[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
        out += kB64[v >> 18]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += '=';
    }
    return out;
}

bool base64Decode(const std::string& text, std::string& out) {
    int map[256];
    for (int& m : map) m = -1;
    for (int i = 0; i < 64; i++) map[(uint8_t)kB64[i]] = i;
    map[(uint8_t)'-'] = 62;  // (url-safe alphabet too)
    map[(uint8_t)'_'] = 63;
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : text) {
        if (c == '=' ) break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = map[c];
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((acc >> bits) & 0xFF);
        }
    }
    return true;
}
