#include "ui/color_picker.h"

#include <algorithm>
#include <cmath>

#include "core/util.h"
#include "ui/colors.h"
#include "ui/paint_helpers.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/style.h"

using namespace ui;

namespace color_picker {

std::vector<std::string> loadRecent() {
    Json stored = settings::get("recent_clip_colors");
    std::vector<std::string> raw;
    if (stored.isString()) raw.push_back(stored.toString());  // (a one-item list stored as a plain string)
    for (auto& item : stored.items())
        if (item.isString()) raw.push_back(item.toString());
    std::vector<std::string> out;
    for (auto& c : raw) {
        std::string hex;
        if (parseHex(c, hex)) out.push_back(hex);
    }
    return out;
}

void remember(const std::string& hexColor) {
    if (hexColor.empty()) return;
    std::vector<std::string> recent{hexColor};
    for (auto& c : loadRecent())
        if (c != hexColor) recent.push_back(c);
    if ((int)recent.size() > RECENT_MAX) recent.resize(RECENT_MAX);
    Json list = Json::array();
    for (auto& c : recent) list.push(c);
    settings::set("recent_clip_colors", list);
}

void hueSatAt(double x, double y, double cx, double cy, double radius, double& h, double& s) {
    double dx = x - cx, dy = y - cy;
    h = std::atan2(-dy, dx) / (2 * M_PI);
    h -= std::floor(h);  // (Python's % 1.0)
    if (h >= 1.0) h = 0;
    s = std::min(1.0, std::hypot(dx, dy) / radius);
}

bool parseHex(const std::string& text, std::string& hexOut) {
    std::string t = util::trim(text);
    if (t.empty() || t[0] != '#') return false;
    for (size_t i = 1; i < t.size(); i++)
        if (!isxdigit((unsigned char)t[i])) return false;
    if (t.size() == 4) t = std::string("#") + t[1] + t[1] + t[2] + t[2] + t[3] + t[3];
    if (t.size() != 7) return false;
    hexOut = util::lower(t);
    return true;
}

}  // namespace color_picker

// ---------------------------------------------------------------------------------------------
// ColorWheel

ColorWheel::ColorWheel(int size) {
    setStyleType("ColorWheel");
    setFixedSize(size, size);
    setCursor(Cursor::Cross);
}

Color ColorWheel::color() const { return Color::fromHsv((float)(h * 360.0), (float)s, (float)v); }

void ColorWheel::setColor(const Color& c) {
    float hh, ss, vv;
    c.toHsv(hh, ss, vv);
    h = std::max(hh, 0.0f) / 360.0;  // grey has no hue (-1)
    if (h >= 1.0) h = 0;
    s = ss;
    v = vv;
    update();
}

void ColorWheel::setValue(double value) {
    v = value;
    update();
}

const Image& ColorWheel::wheelImage(float dpr) {
    int n = width();
    int size = std::max(1, (int)std::lround(n * dpr));  // in device pixels
    if (image_.width() != size || imageV_ != v || imageDpr_ != dpr) {
        double r = radius();
        image_ = Image(size, size);
        for (int j = 0; j < size; j++) {
            uint32_t* row = image_.row(j);
            double y = (j + 0.5) / dpr - n / 2.0;  // each device pixel's centre, in logical pixels
            for (int i = 0; i < size; i++) {
                double x = (i + 0.5) / dpr - n / 2.0;
                double dist = std::hypot(x, y);
                double alpha = std::min(1.0, std::max(0.0, (r - dist) * dpr + 0.5));  // soft 1 (device) px rim
                if (alpha <= 0) {
                    row[i] = 0;
                    continue;
                }
                double hue = std::atan2(-y, x) / (2 * M_PI);
                hue -= std::floor(hue);
                Color c = Color::fromHsv((float)(hue * 360.0), (float)std::min(1.0, dist / r), (float)v);
                row[i] = c.withAlpha((int)std::lround(alpha * 255)).premul();
            }
        }
        imageV_ = v;
        imageDpr_ = dpr;
    }
    return image_;
}

void ColorWheel::paint(Painter& p) {
    float dpr = window() ? window()->scale() : 1.0f;
    const Image& wheel = wheelImage(dpr > 0 ? dpr : 1.0f);
    if (wheel.width() == width()) p.drawImage(Point(0, 0), wheel);
    else p.drawImage(RectF(0, 0, (float)width(), (float)height()), wheel);
    double angle = 2 * M_PI * h, c = width() / 2.0;
    PointF marker((float)(c + std::cos(angle) * s * radius()), (float)(c - std::sin(angle) * s * radius()));
    Color col = color();
    const css::Style& st = computedStyle("marker");  // (its colours: the colour chosen, and a readable edge)
    float size = (float)st.length("width", 12);
    RectF dot(marker.x - size / 2, marker.y - size / 2, size, size);
    p.fillEllipse(dot, col);
    p.strokeEllipse(dot, colors::readableText(col), (float)st.length("stroke-width", 2));
}

void ColorWheel::pick(PointF pos) {
    double c = width() / 2.0;
    color_picker::hueSatAt(pos.x, pos.y, c, c, radius(), h, s);
    update();
    changed.emit();
}

bool ColorWheel::mousePress(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    pick(PointF((float)e.pos.x + 0.5f, (float)e.pos.y + 0.5f));
    return true;
}

bool ColorWheel::mouseMove(MouseEvent& e) {
    if (!(e.buttons & plat::LeftButton)) return false;
    pick(PointF((float)e.pos.x + 0.5f, (float)e.pos.y + 0.5f));
    return true;
}

// ---------------------------------------------------------------------------------------------
// previews

ClipPreview::ClipPreview() {
    setStyleType("ClipPreview");
    const css::Style& st = computedStyle();
    setFixedSize(style::px(st, "width", 170), style::px(st, "height", 64));
}

TrackPreview::TrackPreview() { setStyleType("TrackPreview"); }

void ClipPreview::setColor(const std::string& hex, const std::string& t) {
    hexColor = hex;
    tone = t;
    update();
}

void ClipPreview::paint(Painter& p) {
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    colors::ClipColors cc = colors::clipColors(hexColor, true, tone);
    style::Insets pad = style::insets(st, "padding");
    RectF r((float)pad.left, (float)pad.top, (float)(width() - pad.w()), (float)(height() - pad.h()));
    const css::Style& clip = computedStyle("clip");  // (its colours are the clip's own)
    float handle = (float)clip.length("min-height", 16);  // (its name's bar)
    p.fillRect(r, cc.fill);
    p.fillRect(RectF(r.x, r.y, r.w, handle), cc.handle);
    Font f = style::font(clip, theme::uiFont());
    p.drawText(RectF(r.x + 6, r.y + 1, r.w - 6, handle - 1), AlignLeft | AlignVCenter, "Clip name", f, cc.text);
    double mid = r.y + handle + (r.h - handle) / 2;
    int i = 0;
    for (int x = (int)r.x + 4; x < (int)(r.x + r.w) - 4; x += 2, ++i) {
        double a = (std::sin(i * 0.7) * 0.5 + 0.5) * (r.h - 24) / 2;
        p.drawLine(PointF(x + 0.5f, (float)(mid - a)), PointF(x + 0.5f, (float)(mid + a)), cc.content);
    }
    p.strokeRect(r, cc.border, (float)clip.length("border-top-width", 1.5));
}

void TrackPreview::paint(Painter& p) {
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    Color background, text;
    colors::trackColors(hexColor, tone, background, text);
    Rect r = style::inside(rect(), style::insets(st, "padding"));
    // (the row's colours are the track's own: ::row is the box drawn in them, as a panel row's TrackRow)
    style::box(p, r, paint::withBackground(computedStyle("row"), background));
    const css::Style& stripe = computedStyle("stripe");
    style::box(p, Rect(r.x, r.y, style::px(stripe, "width", 3), r.h), stripe);
    p.drawText(RectF(r.x + 10, r.y + 6, r.w - 10, 16), AlignLeft | AlignVCenter, "Track name",
               style::font(computedStyle("name"), theme::uiFont()), text);
    p.drawTextBlock(RectF(r.x, r.y, r.w - 6, r.h - 6), AlignRight | AlignBottom, "S -14.2\nI -16.0",
                    style::font(computedStyle("readout"), theme::monoFont(8)), text, false);
    int right = r.x + r.w - 1;
    const css::Style& button = computedStyle("button");
    style::box(p, Rect(right - 54, r.y + 6, 22, 16), button);  // an M / S button
    style::box(p, Rect(right - 28, r.y + 6, 22, 16), button);
    style::box(p, Rect(r.x + 10, r.y + r.h - 1 - 20, 60, 14), computedStyle("meter"));  // a level meter
}

// ---------------------------------------------------------------------------------------------
// the dialog

namespace {
struct Subject {
    const char* title;
    const char* content;
    const char* contentTip;
    const char* defaultTip;
};
const Subject& subjectOf(ColorSubject s) {
    static const Subject clip{"Clip colour", "Waveform / notes and name:", "waveform/notes and name",
                              "Back to the standard colour for audio / MIDI clips"};
    static const Subject track{"Track colour", "Name and readouts:", "name and LUFS readout",
                               "Back to the standard look of track rows"};
    static const Subject group{"Group colour", "Name:", "name", "Back to the standard look of group rows"};
    return s == ColorSubject::Clip ? clip : s == ColorSubject::Track ? track : group;
}

// a recent colour: a small square in that colour
class Swatch : public Button {
public:
    explicit Swatch(const std::string& hex) : hex_(hex) {
        setStyleType("Swatch");
        int size = theme::metric("color-swatch-size", 22);
        setFixedSize(size, size);
        setToolTip(hex);
        setFocusPolicy(FocusPolicy::Tab);
    }
    void paint(Painter& p) override {
        // (the square's own part: what it shares with buttons doesn't change it)
        const css::Style& st = computedStyle("chip");
        style::frame(p, rect(), Color::fromHex(hex_), style::color(st, "border-top-color"), style::radius(st));
    }
    std::string hex_;
};

// a row of widgets (the grid's cells that hold several): the room of the dialog's part (ColorPickerDialog::row,
// ::recent)
Widget* row(Widget* parent, HBox*& layout, const char* part = "row", int gap = 6) {
    Widget* w = parent->add<Widget>();
    layout = styleRoom(hbox(w, 0, gap), parent->computedStyle(part));
    return w;
}
}  // namespace

ColorPickerDialog::ColorPickerDialog(Window* parent, const std::string& initial, const std::string& tone_,
                                     ColorSubject subject)
    : Dialog(parent, subjectOf(subject).title, 420, 420) {
    setStyleType("ColorPickerDialog");
    const Subject& sub = subjectOf(subject);
    toneValues_ = {"", "light", "dark"};
    toneTips_ = {"Picked automatically for the best contrast", std::string("White ") + sub.contentTip,
                 std::string("Dark grey ") + sub.contentTip};
    std::string init;
    if (!color_picker::parseHex(initial, init)) init = "#808080";
    chosen = init;
    tone = tone_;
    wantedTone = tone_;

    wheel = add<ColorWheel>(theme::metric("color-wheel-size", 200));
    brightness = add<Slider>(Orientation::Horizontal);
    brightness->setRange(0, 100);
    brightness->setToolTip("Brightness");
    brightness->setFixedWidth(std::max(0, style::px(computedStyle("brightness"), "width", 200)));
    hexEdit = add<LineEdit>();
    hexEdit->setMaxLength(7);
    hexEdit->setFixedWidth(std::max(0, style::px(computedStyle("hex"), "width", 80)));
    if (subject == ColorSubject::Clip) preview = add<ClipPreview>();
    else preview = add<TrackPreview>();

    GridLayout* g = styleRoom(grid(this, 11, 6), computedStyle());
    g->addWidget(wheel, 0, 0, 3, 1);
    g->addWidget(add<Label>("Preview (contrast is\nadjusted automatically)"), 0, 1);
    g->addWidget(preview, 1, 1);
    HBox* hl;
    Widget* hexRow = row(this, hl);
    Label* hexLabel = hexRow->add<Label>("Hex");
    hl->addWidget(hexLabel);
    hexRow->adopt(release(hexEdit));
    hl->addWidget(hexEdit);
    hl->addStretch(1);
    g->addWidget(hexRow, 2, 1);
    g->addWidget(brightness, 3, 0);

    // White / dark grey are only offered when they contrast enough with the chosen colour
    HBox* tl;
    Widget* toneRow = row(this, tl);
    tl->addWidget(toneRow->add<Label>(sub.content));
    const char* labels[] = {"Auto", "White", "Dark grey"};
    for (int i = 0; i < 3; i++) {
        Button* b = toneRow->add<Button>(labels[i]);
        b->setCheckable(true);
        b->setToolTip(toneTips_[i]);
        b->setProperty("accent", "true");  // lit in the skin's accent colour when chosen
        std::string value = toneValues_[i];
        b->clicked.connect([this, value] { setTone(value); });
        toneButtons.push_back(b);
        tl->addWidget(b);
    }
    tl->addStretch(1);
    g->addWidget(toneRow, 4, 0, 1, 2);

    g->addWidget(add<Label>("Recent"), 5, 0, 1, 2);
    HBox* rl;
    Widget* recentRow = row(this, rl, "recent", 4);
    std::vector<std::string> recent = color_picker::loadRecent();
    for (auto& hex : recent) {
        Swatch* sw = recentRow->add<Swatch>(hex);
        sw->clicked.connect([this, hex] { setColor(Color::fromHex(hex)); });
        recentButtons.push_back(sw);
        rl->addWidget(sw);
    }
    if (recent.empty()) rl->addWidget(recentRow->add<Label>("(colours you use show up here)"));
    rl->addStretch(1);
    g->addWidget(recentRow, 6, 0, 1, 2);

    HBox* bl;
    Widget* buttonRow = row(this, bl);
    defaultButton = buttonRow->add<Button>("Default");
    defaultButton->setToolTip(sub.defaultTip);
    defaultButton->clicked.connect([this] { useDefault(); });
    Button* ok = buttonRow->add<Button>("OK");
    Button* cancel = buttonRow->add<Button>("Cancel");
    ok->setDefault(true);
    dialogButton(ok);
    dialogButton(cancel);
    ok->clicked.connect([this] { accept(); });
    cancel->clicked.connect([this] { reject(); });
    bl->addWidget(defaultButton);
    bl->addStretch(1);
    bl->addWidget(cancel);
    bl->addWidget(ok);
    g->addWidget(buttonRow, 7, 0, 1, 2);

    wheel->changed.connect([this] { fromWheel(); });
    brightness->valueChanged.connect([this](int v) {
        wheel->setValue(v / 100.0);
        fromWheel();
    });
    hexEdit->editingFinished.connect([this] {
        if (closing_) return;  // (closing ends its editing, which would set its colour again)
        std::string hex;
        if (color_picker::parseHex(hexEdit->text(), hex)) setColor(Color::fromHex(hex));
        else hexEdit->setTextSilently(chosen);
    });
    setColor(Color::fromHex(init));
    adjustSize();
}

void ColorPickerDialog::setColor(const Color& c) {
    wheel->setColor(c);
    brightness->setValueSilently((int)std::lround(wheel->v * 100));
    showColor(c.hexRgb());
}

void ColorPickerDialog::fromWheel() { showColor(wheel->color().hexRgb()); }

void ColorPickerDialog::showColor(const std::string& hex) {
    chosen = hex;
    hexEdit->setTextSilently(hex);
    refreshTones();
}

void ColorPickerDialog::setTone(const std::string& t) {
    wantedTone = t;
    refreshTones();
}

// Enable White / Dark grey only where they contrast enough. A wanted tone that does not fit the
// current colour shows as Auto, and comes back when you move to a colour that fits it.
void ColorPickerDialog::refreshTones() {
    tone = !wantedTone.empty() && colors::toneAvailable(chosen, wantedTone) ? wantedTone : "";
    for (size_t i = 0; i < toneButtons.size(); i++) {
        const std::string& t = toneValues_[i];
        bool ok = t.empty() || colors::toneAvailable(chosen, t);
        toneButtons[i]->setEnabled(ok);
        toneButtons[i]->setCheckedSilently(t == tone);
        if (!t.empty()) toneButtons[i]->setToolTip(ok ? toneTips_[i] : "Not enough contrast on this colour");
    }
    preview->setColor(chosen, tone);
}

void ColorPickerDialog::useDefault() {
    closing_ = true;
    chosen.clear();
    tone.clear();
    Dialog::done(Accepted);
}

void ColorPickerDialog::done(int result) {
    if (closing_) {  // (Default already decided)
        Dialog::done(result);
        return;
    }
    closing_ = true;
    if (result == Accepted) color_picker::remember(chosen);
    Dialog::done(result);
}
