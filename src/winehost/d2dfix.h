// Fills in what Wine's Direct2D lacks for plugins (see-through gradients, layers; see d2dfix.cpp).
// Call after a plugin is loaded (main thread); does something only once, and only if d2d1 is loaded.
#pragma once
#include <string>

void fixWineDirect2D();
// lets go of the bitmaps kept for Direct2D layers (the plugin's window closed: its devices may go)
void forgetLayerBitmaps();
// forgets DirectWrite's family lookups (and lets go of the font collections they were for)
void forgetFontLookups();
// gives Wine's DXGI a WaitForVBlank (see d2dfix.cpp); likewise, if dxgi is loaded
void fixWineDxgi();
// remembers DirectWrite's font family lookups (see d2dfix.cpp); likewise, if dwrite is loaded
void fixWineDirectWrite();
// (tests) closes one filled path of so many points (see d2dfix.cpp); 0 when Direct2D could be used
int pathTest(int points, int shape, bool asIs);
// (tests) a path written by EXBIT_WINE_DUMP_PATHS, given again (see d2dfix.cpp); 0 when it could be
int pathTestFile(const std::string& file);
// (tests) the outlines' straightening of a few figures, printed (see d2dfix.cpp)
int outlineTest();
