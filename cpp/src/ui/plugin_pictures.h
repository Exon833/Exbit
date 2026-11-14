// Pictures of plugins for the effects window's patch panel: how each plugin's own window looks
// (plugin_pictures.py).
//
// Where a plugin's picture comes from, best first:
//   - one the program took of its window (plugin_host.h): every time the window is open it is
//     photographed now and then, and the last picture is kept when it closes; an effect added
//     without any picture yet is photographed once, its window opened invisibly for a moment;
//   - the picture a VST3 bundle may carry of its plugin (Contents/Resources/Snapshots, part of the
//     VST3 standard; Serum has one);
//   - none: the panel draws an empty faceplate.
// Taken pictures are kept small, one per plugin (not per track: every Pro-Q 4 shows the latest one),
// in the plugin_pictures folder of your Tiny DAW folder (paths::dataDir("plugin_pictures")).
#pragma once
#include <memory>
#include <string>

#include "gfx/image.h"
#include "model/model.h"

namespace plugin_pictures {

constexpr int WIDTH = 480;  // px: pictures are kept at most this wide (the panel shows them smaller)

std::string folder();                         // paths::dataDir("plugin_pictures")
void setFolder(const std::string& folder);    // (tests: use another one; "" = the default)
// where the picture taken of this plugin is kept (readable name, plus a hash for uniqueness)
std::string fileFor(const PluginSlot& slot);
// the .vst3 bundle folder of a plugin file inside one (X.vst3/Contents/x86_64-linux/X.so), or ""
std::string bundleOf(const std::string& path);
// the picture the plugin's VST3 bundle carries of it (<class id>_snapshot[_2.0x].png), or ""
std::string snapshot(const PluginSlot& slot);
// the plugin's picture (see the top), or null; cached by file and modification time
std::shared_ptr<const Image> picture(const PluginSlot& slot);
bool hasPicture(const PluginSlot& slot);
// a picture from a capture of the plugin's window, or false if there is nothing on it yet (one
// colour all over: the plugin hasn't drawn itself)
bool fromCapture(const Image& captured, Image& out);
// keep `image` as the plugin's picture (at most WIDTH wide); true if it was saved
bool save(const PluginSlot& slot, const Image& image);

std::string sha1Hex(const std::string& data);  // (for fileFor; exposed for tests)

}  // namespace plugin_pictures
