// The title bar of the frame the program draws around its own windows (Window::isFramed, App::ownFrames),
// instead of the window manager's: the window's icon and title, and its buttons (minimize, maximize or restore,
// close). Dragging it moves the window (the window manager does: _NET_WM_MOVERESIZE, so it snaps and tiles it),
// a double click maximizes or restores it, a right click shows the window's menu (Minimize, Maximize / Restore,
// Close). Close is the window manager's ✕: Window::close(), so its closeRequested still asks.
//
// It is a widget of the window (Window::titleBar(), its parent()) but not one of its children: it lies above the
// window's rect() (at negative y, as wide as the window's inside), in the frame. Its style type is TitleBar; its
// parts ::icon, ::title (color, font, text-align: left, center or right), ::minimize, ::maximize, ::close (each a
// box; its glyph drawn in its color, icon-size big, stroke-width thick; width and height its size; margin the room
// around it: vertically it is centred in what margin-top and margin-bottom leave of the bar). The bar and
// its parts have the window's states: :active or :inactive (the keyboard focus is in the window or not) and
// :maximized; the bar :hover and :pressed, each button its own :hover and :pressed, and :first / :last (the first
// and the last button shown, left to right: the ends of the group). Its height is :root's
// --title-bar-height; :root's --title-buttons (right or left) is where the buttons go (the icon on the other
// side). A modal dialog has no minimize; a window that can't be resized no maximize. docs/skins.md.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "gfx/image.h"
#include "ui/toolkit/widget.h"

namespace ui {

class TitleBar : public Widget {
public:
    explicit TitleBar(Window* window);
    ~TitleBar() override;
    enum Part { NoPart = 0, Icon, Title, Minimize, Maximize, Close };
    static const char* partName(Part p);    // "icon", "title", "minimize", "maximize", "close"
    Window* frameWindow() const { return win_; }
    // its height: :root's --title-bar-height (logical px)
    static int barHeight();
    // where a part is (this widget's coordinates); empty when it isn't shown (no icon, a button it doesn't have)
    Rect partRect(Part p) const;
    Part partAt(Point p) const;
    bool hasButton(Part b) const;           // Minimize: not modal; Maximize: resizable; Close: always
    // the buttons it has, from left to right as shown (out: up to 3); how many
    int shownButtons(Part out[3]) const;
    static bool buttonsLeft();              // :root's --title-buttons is left
    Part hoverPart() const { return hover_; }
    Part pressedPart() const { return down_; }
    // the window's menu (Minimize, Maximize / Restore, Close), at a screen position; menu(): the last shown
    void showMenu(Point globalPos);
    Menu* menu() const { return menu_.get(); }

    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseDoubleClick(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    bool mouseRelease(MouseEvent& e) override;
    bool contextMenu(MouseEvent& e) override;
    void leave() override;
    void styleStates(std::vector<std::string>& out) const override;
    std::string toolTipAt(Point p) const override;

private:
    Window* win_;
    Part hover_ = NoPart, down_ = NoPart;
    mutable Part styled_ = NoPart;          // (styleStates: the part being styled, its own :hover / :pressed)
    bool armed_ = false, moving_ = false;   // a press on the bar: a move once the pointer goes far enough
    Point pressGlobal_, startPos_;          // (and where the window's inside was then: moving it by hand)
    std::unique_ptr<Menu> menu_;
    Image icon_;                            // the window's icon (WindowOptions::iconSvg), rendered
    int iconPx_ = 0;
    const css::Style& partStyle(Part p) const;
    void layoutParts(Rect out[6]) const;
    void trigger(Part b);
    void setHoverPart(Part p);
};

}  // namespace ui
