// Declarations with var(), worked out once per set of custom properties (see css_var_cache.h)
#include "core/css_var_cache.h"

namespace css {
namespace detail {

VarResolution resolveVarDeclaration(const Decl& declaration, const VarLookup& lookup) {
    VarResolution result;
    result.substituted =
        substitute(declaration.value, lookup, result.substitutedValue, result.substituteProblem);
    if (!result.substituted || isCssWide(result.substitutedValue)) return result;
    result.checked = true;
    Tokens tokens;
    tokenize(result.substitutedValue, tokens);
    result.units = unitsOf(tokens, 0, tokens.size());
    if (declaration.shorthand.empty()) {
        result.checkProblem = checkValue(declaration.name, tokens, 0, tokens.size());
        result.valid = result.checkProblem.empty();
        result.propertyValue = result.substitutedValue;
        return result;
    }
    // a longhand of a shorthand with var(): the shorthand's text expanded, and this property's part taken
    std::vector<std::pair<std::string, std::string>> longhands;
    result.checkProblem = expand(declaration.shorthand, tokens, 0, tokens.size(), longhands);
    if (!result.checkProblem.empty()) return result;
    for (auto& [longhand, value] : longhands)
        if (longhand == declaration.name) {
            result.propertyValue = std::move(value);
            result.valid = true;
        }
    result.longhandIsCssWide = result.valid && isCssWide(result.propertyValue);
    return result;
}

const VarResolution& VarCache::resolve(const Decl& declaration, const std::shared_ptr<const Variables>& variables,
                                       int engineGeneration) {
    if (engineGeneration != generation_ || entries_.size() >= kMaximumEntries) {
        entries_.clear();
        generation_ = engineGeneration;
    }
    auto key = std::make_pair(&declaration, variables.get());
    auto found = entries_.find(key);
    if (found != entries_.end()) {
        hits_++;
        return found->second.resolution;
    }
    const Variables* map = variables.get();
    VarLookup lookup = [map](const std::string& name) -> const std::string* {
        if (!map) return nullptr;
        auto it = map->find(name);
        return it != map->end() ? &it->second : nullptr;
    };
    Entry entry{variables, resolveVarDeclaration(declaration, lookup)};
    return entries_.emplace(key, std::move(entry)).first->second.resolution;
}

}  // namespace detail
}  // namespace css
