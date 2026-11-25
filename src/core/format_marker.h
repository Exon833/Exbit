// The "format" marker at the top of the program's JSON files ("exbit-project", "exbit-synth"…): a file
// marked with the same format under the program's old name (old_name::NAME_PREFIX) is one too.
#pragma once
#include <string>

#include "core/old_name.h"

namespace format_marker {

// is `marker` the format `currentFormat` ("exbit-<kind>"), or that format under the old name?
inline bool isFormat(const std::string& marker, const std::string& currentFormat) {
    if (marker == currentFormat) return true;
    const std::string currentPrefix = "exbit-";
    if (currentFormat.compare(0, currentPrefix.size(), currentPrefix) != 0) return false;
    return marker == old_name::NAME_PREFIX + currentFormat.substr(currentPrefix.size());
}

}  // namespace format_marker
