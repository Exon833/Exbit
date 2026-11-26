// Exbit's Wine fixes on Windows itself (see wine_prefix_fixes.h): there is no Wine, nothing to fix.
#include "plugins/wine_prefix_fixes.h"

namespace wine {

std::string fixesFolder() { return ""; }
std::vector<std::string> fixedDllNames() { return {}; }
bool prefixHasFixes(const std::string&) { return false; }
bool applyFixes(const std::string&, std::string& report) {
    report = "Wine isn't used on Windows";
    return false;
}
bool removeFixes(const std::string&, std::string& report) {
    report = "Wine isn't used on Windows";
    return false;
}
std::string dllKind(const std::string&) { return ""; }

}  // namespace wine
