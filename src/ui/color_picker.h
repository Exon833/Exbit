// Colour dialog for clips and track/group rows (color_picker.py): a colour wheel (hue around,
// saturation outward), a brightness slider, a hex field, recently used colours, and a preview with
// automatic contrast. CONTRACT written by the lead; implemented in color_picker.cpp.
//
// Recent colours are remembered between runs (settings "recent_clip_colors"), shared by clips and
// tracks.
//
// Styled (default.css): ColorWheel::marker (the chosen colour's dot: width, stroke-width; its colours are the
// colour's own); ClipPreview (background-color, padding, width, height) and its ::clip (font, min-height: the
// name's bar, border-top-width: the clip's edge; the clip's colours are its own); TrackPreview (counts as a
// ClipPreview) ::row (a box in the track's colour, inside its padding), ::stripe (background-color, width), ::name
// and ::readout (font), ::button and ::meter (background-color); Swatch (a recent colour, a Button) ::chip (border-top-color, border-radius). :root's
// --color-wheel-size and --color-swatch-size.
#pragma once
#include <string>
#include <vector>

#include "gfx/image.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

enum class ColorSubject { Clip, Track, Group };

namespace color_picker {
constexpr int RECENT_MAX = 10;
std::vector<std::string> loadRecent();       // newest first ("#rrggbb")
void remember(const std::string& hexColor);  // to the front (once), at most RECENT_MAX
// hue (0..1, 0 = red to the right, counter-clockwise) and saturation (0 centre .. 1 rim, clipped)
// at (x, y) of a wheel centred at (cx, cy) with that radius
void hueSatAt(double x, double y, double cx, double cy, double radius, double& h, double& s);
// "#rgb" / "#rrggbb" (any case) -> true and "#rrggbb" lower case
bool parseHex(const std::string& text, std::string& hexOut);
}  // namespace color_picker

// Hue goes around the circle, saturation from the centre (white/grey) to the rim.
class ColorWheel : public ui::Widget {
public:
    explicit ColorWheel(int size = 200);
    double h = 0, s = 0, v = 1;    // 0..1
    ui::Signal<> changed;
    Color color() const;           // from h, s, v
    void setColor(const Color& c);
    void setValue(double value);
    double radius() const { return width() / 2.0 - 2; }
    void pick(PointF pos);         // the colour under pos becomes h, s; emits changed
    // the wheel at the screen's own resolution (dpr device pixels a logical pixel: the window's
    // interface size), so it stays sharp when the interface is scaled
    const Image& wheelImage(float dpr = 1.0f);
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override { return true; }
private:
    Image image_;
    double imageV_ = -1;
    float imageDpr_ = 1.0f;
};

// How a clip will look in the chosen colour, after automatic contrast.
class ClipPreview : public ui::Widget {
public:
    ClipPreview();
    void setColor(const std::string& hexColor, const std::string& tone);
    void paint(Painter& p) override;
    std::string hexColor = "#808080", tone;
};

// How a track's row in the panel will look in the chosen colour, after automatic contrast.
class TrackPreview : public ClipPreview {
public:
    TrackPreview();
    void paint(Painter& p) override;
};

class ColorPickerDialog : public ui::Dialog {
public:
    // initial: "#rrggbb"; tone: "light" / "dark" / "" (automatic)
    ColorPickerDialog(ui::Window* parent, const std::string& initial, const std::string& tone = "",
                      ColorSubject subject = ColorSubject::Clip);
    // after exec() returned Accepted: "#rrggbb", or "" (Default: the standard look)
    std::string chosen;
    // "light" / "dark" / "" (automatic)
    std::string tone;

    // (the parts, for tests and the dialog itself)
    std::string wantedTone;
    ColorWheel* wheel;
    ui::Slider* brightness;
    ui::LineEdit* hexEdit;
    ClipPreview* preview;
    std::vector<ui::Button*> toneButtons;  // Auto, White, Dark grey
    std::vector<ui::Button*> recentButtons;
    ui::Button* defaultButton;
    void setColor(const Color& c);         // from a swatch or the hex field
    void setTone(const std::string& t);
    void useDefault();                     // Default: "" and closes (Accepted)
    void done(int result) override;        // OK remembers the colour among the recent ones
private:
    void fromWheel();
    void showColor(const std::string& hex);
    void refreshTones();
    std::vector<std::string> toneValues_, toneTips_;
    bool closing_ = false;
};
