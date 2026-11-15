// Tiny Test Installer: a Windows program (built with MinGW: make win-plugins) that installs the
// Windows builds of the test plugins the way a plugin's installer does, for test_wine's Install
// Windows plugin… test. From the folder it is in it copies:
//   TinyTestFx.vst3 (+ TinyTestFx.moduleinfo.json) → %CommonProgramFiles%\VST3\TinyTestFx.vst3\Contents:
//       x86_64-win\TinyTestFx.vst3 and Resources\moduleinfo.json, a VST3 bundle
//   TinyTestSynth.dll → %ProgramFiles%\VstPlugins\Tiny Test\TinyTestSynth.dll (a VST2 plugin in a
//       vendor's folder inside a usual VST2 folder)
// and, where they were built (make win-plugins with MinGW's i686 compiler), the 32-bit ones where a
// 32-bit installer puts them: TinyTestFx32.vst3 → %CommonProgramFiles(x86)%\VST3\TinyTestFx32.vst3 (its
// Contents\x86-win; no moduleinfo: the 32-bit host is asked), TinyTestSynth32.dll → %ProgramFiles(x86)%
// VstPlugins\Tiny Test.
// Exit status 0 when all is in place, else 1 (and why, on stderr).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

std::wstring env(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

// makes every folder of `path` (a full Windows path) that is missing
bool makeDirs(const std::wstring& path) {
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1))
        CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool install(const std::wstring& from, const std::wstring& toDir, const std::wstring& name) {
    if (!makeDirs(toDir)) {
        fwprintf(stderr, L"cannot make %ls\n", toDir.c_str());
        return false;
    }
    if (!CopyFileW(from.c_str(), (toDir + L"\\" + name).c_str(), FALSE)) {
        fwprintf(stderr, L"cannot copy %ls (error %lu)\n", from.c_str(), GetLastError());
        return false;
    }
    return true;
}

}  // namespace

int main() {
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring here(self, n);
    here = here.substr(0, here.find_last_of(L'\\'));
    std::wstring common = env(L"CommonProgramFiles"), programs = env(L"ProgramFiles");
    if (common.empty() || programs.empty()) {
        fwprintf(stderr, L"no CommonProgramFiles / ProgramFiles\n");
        return 1;
    }
    std::wstring bundle = common + L"\\VST3\\TinyTestFx.vst3\\Contents";
    bool ok = install(here + L"\\TinyTestFx.vst3", bundle + L"\\x86_64-win", L"TinyTestFx.vst3") &&
              install(here + L"\\TinyTestFx.moduleinfo.json", bundle + L"\\Resources", L"moduleinfo.json") &&
              install(here + L"\\TinyTestSynth.dll", programs + L"\\VstPlugins\\Tiny Test", L"TinyTestSynth.dll");
    // the 32-bit ones, if built
    std::wstring common32 = env(L"CommonProgramFiles(x86)"), programs32 = env(L"ProgramFiles(x86)");
    if (ok && GetFileAttributesW((here + L"\\TinyTestFx32.vst3").c_str()) != INVALID_FILE_ATTRIBUTES)
        ok = !common32.empty() && install(here + L"\\TinyTestFx32.vst3",
                                          common32 + L"\\VST3\\TinyTestFx32.vst3\\Contents\\x86-win", L"TinyTestFx32.vst3");
    if (ok && GetFileAttributesW((here + L"\\TinyTestSynth32.dll").c_str()) != INVALID_FILE_ATTRIBUTES)
        ok = !programs32.empty() &&
             install(here + L"\\TinyTestSynth32.dll", programs32 + L"\\VstPlugins\\Tiny Test", L"TinyTestSynth32.dll");
    return ok ? 0 : 1;
}
