// Fills in what Wine's Direct2D lacks for plugins (see-through gradients, layers; see d2dfix.cpp).
// Call after a plugin is loaded (main thread); does something only once, and only if d2d1 is loaded.
#pragma once

void fixWineDirect2D();
// gives Wine's DXGI a WaitForVBlank (see d2dfix.cpp); likewise, if dxgi is loaded
void fixWineDxgi();
