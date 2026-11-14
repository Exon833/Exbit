// A small JSON value (the stand-in for Python's json module): parse, build, write.
// Objects keep their keys in insertion order (as Python dicts do), so files written by the
// program read like the Python program's.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };
    using Members = std::vector<std::pair<std::string, Json>>;

    Json() = default;  // null
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Bool), b_(b) {}
    Json(int v) : type_(Number), n_(v), isInt_(true) {}
    Json(long v) : type_(Number), n_((double)v), isInt_(true) {}
    Json(long long v) : type_(Number), n_((double)v), isInt_(true) {}
    Json(unsigned v) : type_(Number), n_(v), isInt_(true) {}
    Json(unsigned long v) : type_(Number), n_((double)v), isInt_(true) {}
    Json(double v) : type_(Number), n_(v) {}
    Json(float v) : type_(Number), n_(v) {}
    Json(const char* s) : type_(String), s_(s) {}
    Json(const std::string& s) : type_(String), s_(s) {}
    Json(std::string&& s) : type_(String), s_(std::move(s)) {}
    static Json array(std::vector<Json> items = {});
    static Json object(Members members = {});

    Type type() const { return type_; }
    bool isNull() const { return type_ == Null; }
    bool isBool() const { return type_ == Bool; }
    bool isNumber() const { return type_ == Number; }
    bool isString() const { return type_ == String; }
    bool isArray() const { return type_ == Array; }
    bool isObject() const { return type_ == Object; }

    // values, with a default when it isn't of that type
    bool toBool(bool def = false) const { return type_ == Bool ? b_ : def; }
    double toDouble(double def = 0) const { return type_ == Number ? n_ : def; }
    int toInt(int def = 0) const { return type_ == Number ? (int)n_ : def; }
    int64_t toInt64(int64_t def = 0) const { return type_ == Number ? (int64_t)n_ : def; }
    const std::string& toString() const;  // "" when not a string
    std::string toString(const std::string& def) const { return type_ == String ? s_ : def; }

    // arrays
    size_t size() const;  // items of an array, members of an object
    const Json& operator[](size_t i) const;  // null when out of range / not an array
    Json& at(size_t i);
    void push(Json v);  // makes it an array if null
    const std::vector<Json>& items() const;  // empty when not an array
    std::vector<Json>& items();

    // objects
    bool has(const std::string& key) const;
    const Json& operator[](const std::string& key) const;  // null when missing
    const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
    Json& operator[](const std::string& key);  // inserts null (makes it an object if null)
    Json& operator[](const char* key) { return (*this)[std::string(key)]; }
    void set(const std::string& key, Json v);  // insert or replace
    void erase(const std::string& key);
    const Members& members() const;  // empty when not an object
    Members& members();
    // get(key, default) helpers
    double num(const std::string& key, double def) const { const Json& j = (*this)[key]; return j.isNumber() ? j.n_ : def; }
    int integer(const std::string& key, int def) const { const Json& j = (*this)[key]; return j.isNumber() ? (int)j.n_ : def; }
    bool flag(const std::string& key, bool def) const { const Json& j = (*this)[key]; return j.isBool() ? j.b_ : def; }
    std::string str(const std::string& key, const std::string& def = "") const { const Json& j = (*this)[key]; return j.isString() ? j.s_ : def; }

    // text. indent < 0: compact (Python's json.dumps default spacing ", " and ": "); else that many spaces
    std::string dump(int indent = -1) const;
    // parse; on error returns null and sets `error` (if given)
    static Json parse(const std::string& text, std::string* error = nullptr);

    bool operator==(const Json& o) const;
    bool operator!=(const Json& o) const { return !(*this == o); }

private:
    Type type_ = Null;
    bool b_ = false;
    double n_ = 0;
    bool isInt_ = false;  // written without a decimal point
    std::string s_;
    std::shared_ptr<std::vector<Json>> a_;
    std::shared_ptr<Members> o_;
    void dumpTo(std::string& out, int indent, int depth) const;
    void detach();  // copy-on-write of the shared array/object
};

// base64 (plugin states in projects)
std::string base64Encode(const std::string& bytes);
bool base64Decode(const std::string& text, std::string& bytes);
