// A declaration whose value has var(), worked out with one set of custom properties: the substitution, then the
// value checked as its property's (a shorthand's text expanded to its longhands first). Computing styles needs
// this for every node and declaration; it depends only on the declaration and the variables, so the engine keeps
// it per (declaration, variables) in a VarCache instead of tokenizing the same text again for each node.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "core/css_internal.h"

namespace css {
namespace detail {

using Variables = std::map<std::string, std::string>;

// What a declaration with var() comes to with one set of custom properties.
struct VarResolution {
    bool substituted = false;        // every var() was replaced (else substituteProblem says why)
    std::string substitutedValue;    // the declaration's text after var() (when substituted)
    std::string substituteProblem;   // why var() couldn't be replaced
    bool checked = false;            // the value was checked as the property's (not done for inherit / initial / unset)
    bool valid = false;              // it is a valid value for the property (else checkProblem says why)
    bool longhandIsCssWide = false;  // a shorthand's part for this property came out as inherit / initial / unset
    uint8_t units = 0;               // the relative units it uses (rem, vw, vh, ch): worked out on the node
    std::string propertyValue;       // the property's value (for a shorthand: its part for this property)
    std::string checkProblem;        // why the value isn't valid for the property
};

// Works out what `declaration` comes to with the custom properties `lookup` finds (for one property: the
// declaration's own name).
VarResolution resolveVarDeclaration(const Decl& declaration, const VarLookup& lookup);

// The resolutions already worked out, per declaration and set of custom properties. A set is known by the
// identity of its shared map, which each entry keeps alive so that its address can't be reused by another.
// Emptied when the engine's sheets or media change (a new generation) and when it holds too many.
class VarCache {
public:
    // the resolution of `declaration` with `variables` (null: none), worked out the first time it is asked for
    const VarResolution& resolve(const Decl& declaration, const std::shared_ptr<const Variables>& variables,
                                 int engineGeneration);
    // how many resolutions were taken from the cache instead of being worked out
    size_t hits() const { return hits_; }

private:
    struct Entry {
        std::shared_ptr<const Variables> keptVariables;  // (keeps the map, so its address names only it)
        VarResolution resolution;
    };
    struct KeyHash {
        size_t operator()(const std::pair<const Decl*, const Variables*>& key) const {
            return std::hash<const void*>()(key.first) * 31u ^ std::hash<const void*>()(key.second);
        }
    };
    static constexpr size_t kMaximumEntries = 50000;  // (variable sets of nodes gone: dropped all at once)
    std::unordered_map<std::pair<const Decl*, const Variables*>, Entry, KeyHash> entries_;
    int generation_ = -1;
    size_t hits_ = 0;
};

}  // namespace detail
}  // namespace css
