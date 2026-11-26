// Exbit's Wine fixes in a prefix (the stand-ins of src/winefix: msi.dll and dxgi.dll, see
// winefix/wine_own_dll.h). Each is Wine's own DLL with a correction that some installers need:
//   - msi.dll: Wine's MsiDatabaseCommit could leave a database unreadable (Kontakt 8's setup stood still
//     at "Computing space requirements");
//   - dxgi.dll: Wine has no IDXGIOutput::WaitForVBlank, and JUCE 8 programs hung (Quanta 2's installer
//     never showed its window).
// Applying them copies the 64-bit ones into the prefix's drive_c/windows/system32 and the 32-bit ones
// into syswow64 (system32 in a 32-bit prefix), keeps what was there (Wine's placeholders) in the prefix's
// .exbit-wine-fixes folder, and tells Wine to prefer them (HKCU\Software\Wine\DllOverrides: native,builtin).
// A DLL that isn't Wine's there (DXVK's dxgi.dll, a Windows msi.dll someone put in) is left alone, with its
// override. Then wine-fixes/exbit-wine-fix-check(-32).exe is run in the prefix: a stand-in it doesn't find
// working is taken out again (the prefix goes back to Wine's own), so a prefix is never left worse.
// Exbit's own plugin hosts keep Wine's own dxgi (they have their own fix: winehost/d2dfix.cpp).
#pragma once
#include <string>
#include <vector>

namespace wine {

// Exbit's built fixes: $EXBIT_WINE_FIXES, else wine-fixes next to the program (or in ../lib/exbit);
// "" when they weren't built (no MinGW): prefixes are then left as Wine makes them
std::string fixesFolder();

// the stand-ins Exbit has (names: "msi.dll", "dxgi.dll")
std::vector<std::string> fixedDllNames();

// are all of Exbit's fixes in this prefix, as they are now (the same files, preferred by Wine)? A DLL
// left alone (not Wine's there) counts as in. False when there are none to put in (fixesFolder "")
bool prefixHasFixes(const std::string& prefix);

// puts Exbit's fixes in the prefix and checks them (see the top); `report`: what was done, for people
// (a line per DLL). True if every one it could put in works there
bool applyFixes(const std::string& prefix, std::string& report);

// takes them out: Wine's own files back, the overrides gone (the prefix as Wine made it)
bool removeFixes(const std::string& prefix, std::string& report);

// (tests) what this file is: "placeholder" (Wine's stand-in for a builtin DLL), "builtin" (Wine's own
// DLL), "exbit" (one of Exbit's fixes), "other" (a DLL from elsewhere), "" (missing)
std::string dllKind(const std::string& file);

}  // namespace wine
