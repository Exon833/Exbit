// The program's environment variables: EXBIT_<NAME>, or, when that isn't set, the same one under the program's
// old name (old_name::ENVIRONMENT_PREFIX), so scripts and setups made before the rename keep working. Header
// only: the plugin hosts (src/winehost) read them too.
#pragma once
#include <cstdlib>
#include <string>

#include "core/old_name.h"

namespace environment {

constexpr const char* PREFIX = "EXBIT_";

// the value of EXBIT_<nameWithoutPrefix>, else of its old name's; nullptr when neither is set
inline const char* readProgramVariable(const char* nameWithoutPrefix) {
    std::string currentName = std::string(PREFIX) + nameWithoutPrefix;
    if (const char* value = getenv(currentName.c_str())) return value;
    std::string oldName = std::string(old_name::ENVIRONMENT_PREFIX) + nameWithoutPrefix;
    return getenv(oldName.c_str());
}

}  // namespace environment
