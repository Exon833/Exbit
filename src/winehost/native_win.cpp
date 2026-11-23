// tinydaw-plugin-host.exe on Windows itself (see host.cpp): what d2dfix.cpp and dragdrop.cpp do for
// Wine isn't needed there. Windows draws with its own Direct2D, DXGI and DirectWrite; drags between
// the plugin's window and Tiny DAW's are Windows' own (OLE), from window to window: nothing for the
// host to pass on.
#include "winehost/d2dfix.h"
#include "winehost/dragdrop.h"

void fixWineDirect2D() {}
void forgetLayerBitmaps() {}
void forgetFontLookups() {}
void fixWineDxgi() {}
void fixWineDirectWrite() {}
int pathTest(int, int, bool) { return 2; }  // (Wine's tests of d2dfix: not here)
int pathTestFile(const std::string&) { return 2; }
int outlineTest() { return 2; }

void catchDragsOut() {}
std::vector<std::string> takeDraggedFiles() { return {}; }
std::string dropFiles(HWND, int, int, const std::vector<std::string>&) { return ""; }
