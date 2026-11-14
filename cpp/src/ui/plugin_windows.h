// Photographing a plugin's window, for the effects window's patch panel (plugin_windows.py).
// In the Python program this was Windows-only (PrintWindow); here the platform layer captures a
// window, child windows (the plugin's view) included, also an offscreen one that is never shown
// (an invisible window opened only to be photographed: PluginHost::photograph).
#pragma once
#include "gfx/image.h"
#include "ui/toolkit/widget.h"

namespace plugin_windows {

// can windows be photographed here? (a display, not the headless test mode)
bool available();
// a picture of a window's inside (without its title bar); false if it can't be taken (gone,
// not shown, too small)
bool capture(ui::Window* window, Image& out);

}  // namespace plugin_windows
