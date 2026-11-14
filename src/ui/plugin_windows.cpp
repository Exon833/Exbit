// Photographing a plugin's window (plugin_windows.py), through the platform layer.
#include "ui/plugin_windows.h"

namespace plugin_windows {

bool available() { return !ui::App::get().headless() && plat::hasDisplay(); }

bool capture(ui::Window* window, Image& out) {
    if (!window || !window->native() || !available()) return false;
    Rect r = window->native()->geometry();
    if (r.w < 2 || r.h < 2) return false;
    Image img;
    if (!window->native()->capture(img) || img.width() < 2 || img.height() < 2) return false;
    out = std::move(img);
    return true;
}

}  // namespace plugin_windows
