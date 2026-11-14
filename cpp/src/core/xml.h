// A small XML parser (the stand-in for Python's xml.etree.ElementTree, as the Ableton Live set importer
// uses it): a document becomes a tree of Elements with their tag, attributes and text.
//
// Handled: elements, attributes ('…' or "…"), character data, the five named entities and character
// references (&#65; &#x41;), CDATA sections, comments, processing instructions (<?xml …?>), a DOCTYPE
// (skipped, its internal subset too). Attribute values are normalised as XML says (tabs and line breaks
// become spaces), line breaks in text become "\n". Namespaces are not interpreted (a tag is its name as
// written, "a:b"). Encodings: UTF-8 (a BOM is skipped).
//
// It is made for big documents (an .als file can be tens of MB of XML): names and values are
// string_views into the document's own copy of the text, decoded into a side store only when they hold
// an entity or a line break, and elements live in one pool. A Document owns everything its Elements
// point to: keep it alive while using them.
#pragma once
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xml {

struct Attribute {
    std::string_view name;
    std::string_view value;  // entities decoded
};

class Element {
public:
    std::string_view tag;
    std::vector<Attribute> attributes;  // in document order
    std::vector<Element*> children;     // child elements, in document order
    Element* parent = nullptr;
    std::string_view text;              // its own character data (between its children too), decoded

    // the attribute's value, or nullptr if it has none of that name (ElementTree's get())
    const std::string_view* attr(std::string_view name) const;
    // the attribute's value, or `def`
    std::string_view get(std::string_view name, std::string_view def = {}) const;
    bool has(std::string_view name) const { return attr(name) != nullptr; }

    // ElementTree's find / findall with its simple paths: "a/b/c" (children by tag), "*" (any child),
    // "." (this element), ".//x" or "a//b" (descendants at any depth). find: the first match, or nullptr.
    const Element* find(std::string_view path) const;
    std::vector<const Element*> findAll(std::string_view path) const;
    // the first child with this tag (nullptr if none)
    const Element* child(std::string_view tag) const;
    // ElementTree's iter(): this element and every element under it, in document order; with a tag,
    // only those with it
    std::vector<const Element*> iter(std::string_view tag = {}) const;
    // the same, without making a list: fn returns false to stop
    void walk(const std::function<bool(const Element&)>& fn) const;
};

class Document {
public:
    Document() = default;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) = default;
    Document& operator=(Document&&) = default;

    // parse `text` (copied: the document keeps its own); false and `error` ("line 3: …") if it isn't
    // well-formed XML
    bool parse(std::string text, std::string& error);
    bool parse(std::string_view text, std::string& error) { return parse(std::string(text), error); }
    bool parse(const char* text, std::string& error) { return parse(std::string(text), error); }
    // the root element (nullptr before a successful parse)
    const Element* root() const { return root_; }
    size_t elementCount() const { return pool_.size(); }

private:
    std::unique_ptr<std::string> source_;  // (on the heap: its views survive moving the document)
    std::deque<Element> pool_;
    std::deque<std::string> decoded_;      // values and text that had entities or line breaks
    Element* root_ = nullptr;
    friend struct Parser;
};

// "&lt;" etc. of `s` decoded (character references as UTF-8); false if it holds a malformed or unknown one
bool decodeEntities(std::string_view s, std::string& out);
// `s` with & < > " escaped, for writing XML
std::string escape(std::string_view s);

}  // namespace xml
