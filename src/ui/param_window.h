// A plugin's parameters as a list: the window of a plugin that has none of its own here (none at all,
// or none for this system: tinydaw --tui, without a window system, shows every VST plugin this way).
// A row per parameter, its name and its value as the plugin shows it ("Cutoff   1.20 kHz"); Up/Down
// (or typing its first letters) the parameter, Left/Right move it a step (a hundredth, or one of its
// steps), Page Up/Down a tenth, Home/End its ends, Backspace its default. Changes count as the
// plugin's own window's (Plugin::windowEdit: automation's "recently tweaked", written while armed and
// playing), and it follows the parameters as automation moves them.
//
// It behaves as EditorWindow (plugin_editor.h) and EffectWindow do for PluginHost: attach(), place(),
// position(), editorClosed, and the fields PluginHost keeps on an open window.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gfx/image.h"
#include "model/model.h"
#include "plugins/plugin.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/widget.h"

namespace automation {
class TweakWatcher;
}

class ParamWindow : public ui::Window {
public:
    ParamWindow(ui::Window* parent, SlotPtr slot, const std::string& title);
    ~ParamWindow() override;

    SlotPtr slot;
    bool attach();  // false: the plugin isn't there, or has no parameter to show
    void place(ui::Window* main, std::optional<Point> last = {});
    Point position() const;
    void follow();  // the rows' values as they are now
    void close() override;
    bool keyPress(ui::KeyEvent& e) override;

    ui::Signal<SlotPtr> editorClosed;

    // what PluginHost keeps on an open window
    std::unique_ptr<automation::TweakWatcher> watcher;
    Image picture;
    double photographed = 0;
    int tickTimer = -1;

    ui::ListWidget* list = nullptr;
    std::vector<plug::ParamInfo> params;  // the rows'

private:
    std::shared_ptr<plug::Plugin> plugin_;
    int timer_ = -1;
    bool closing_ = false;
    std::string typed_;      // first letters typed, to find a parameter
    double typedAt_ = 0;
    std::string rowText(const plug::ParamInfo& p);
    void set(int row, double normal);
};
