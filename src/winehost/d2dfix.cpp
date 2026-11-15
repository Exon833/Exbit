// A correction to Wine's Direct2D, for plugins drawn with it (Serum 2, Quanta 2…): gradients with
// see-through colours came out too bright and too solid (Serum 2's waveforms: a bright fill where
// Windows shows a faint one).
//
// Direct2D's gradient stops are straight colours (r, g, b, alpha). Wine's d2d1 keeps them that way,
// interpolates them that way and outputs them from its shader unchanged, but blends everything it
// draws as premultiplied colour (D3D11_BLEND_ONE, INV_SRC_ALPHA): a stop's colour is added at full
// strength whatever its alpha. (Solid colours it does premultiply.) So here the stops are
// premultiplied before Wine sees them: Wine's interpolation and blending then come out as
// Windows' (which interpolates premultiplied colour too).
//
// Also (the same way): Direct2D layers, which Wine leaves undone (PushLayer and PopLayer are stubs:
// what should be clipped to a shape filled its whole box, Quanta 2's red rectangles), are done here
// as Direct2D itself describes them: what is drawn inside a layer goes to a bitmap of its own, which
// PopLayer paints onto the target through the layer's mask shape and with its opacity. And DXGI's
// IDXGIOutput::WaitForVBlank, which Wine doesn't have (E_NOTIMPL), waits for the next refresh: JUCE 8
// plugins repaint only after it succeeds, and their windows stood still.
//
// And outlines (DrawGeometry): where a path goes on in a straight line through a point (or turns
// straight back), Wine's outline adds a piece 25 stroke widths long there (it takes it for a turn
// back): lines shot out of the waveforms that plugins draw from many points. Such paths are given to
// Wine flattened, with those points merged (or the path split where it turns back).
//
// And DirectWrite: finding a font family by name (IDWriteFontCollection::FindFamilyName), which
// every text layout does, reads the names of every family Wine knows, and Wine knows every font on
// the Linux system (thousands): Serum 2 spent most of a tab change there. The answers are kept: a
// collection's families don't change.
//
// How: Wine's device contexts share one table of methods. One is made here (on a Direct3D 11
// device) to find that table, and its two CreateGradientStopCollection entries (Direct2D 1.0's and
// 1.1's) are pointed at wrappers that premultiply the stops and call Wine's own. Wine's hwnd and
// DC render targets pass their calls to such a device context, so every way a plugin draws is
// covered. Only Wine's built-in d2d1 is corrected (a Windows d2d1.dll is left alone), only once,
// only when a plugin has d2d1 loaded; TINYDAW_WINE_D2D_AS_IS=1 leaves it as it is.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "winehost/d2dfix.h"

namespace {

typedef LONG HRESULT_;
struct Guid {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
};
struct GradientStop {
    float position;
    float r, g, b, a;
};

// (the interfaces' identifiers and the places of their methods, from the Direct2D, Direct3D 11 and
// DXGI headers' interface order)
const Guid IID_ID2D1Factory1 = {0xbb12d362, 0xdaee, 0x4b9a, {0xaa, 0x1d, 0x14, 0xba, 0x40, 0x1c, 0xfa, 0x1f}};
const Guid IID_IDXGIDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
constexpr int UNKNOWN_QUERY = 0, UNKNOWN_RELEASE = 2;
constexpr int FACTORY1_CREATE_DEVICE = 17;          // ID2D1Factory1::CreateDevice
constexpr int DEVICE_CREATE_CONTEXT = 4;            // ID2D1Device::CreateDeviceContext
constexpr int RT_CREATE_GRADIENT_STOPS = 9;         // ID2D1RenderTarget::CreateGradientStopCollection
constexpr int DC_CREATE_GRADIENT_STOPS = 64;        // ID2D1DeviceContext::CreateGradientStopCollection

using CreateStops10 = HRESULT_(WINAPI*)(void* self, const GradientStop* stops, UINT count, int gamma, int extend,
                                        void** collection);
using CreateStops11 = HRESULT_(WINAPI*)(void* self, const GradientStop* stops, UINT count, int preSpace,
                                        int postSpace, int precision, int extend, int interpolation, void** collection);
CreateStops10 wine10 = nullptr;
CreateStops11 wine11 = nullptr;

std::vector<GradientStop> premultiplied(const GradientStop* stops, UINT count) {
    std::vector<GradientStop> out(stops, stops + (stops ? count : 0));
    for (auto& s : out) {
        float a = s.a < 0 ? 0 : s.a > 1 ? 1 : s.a;
        s.r *= a, s.g *= a, s.b *= a;
    }
    return out;
}

HRESULT_ WINAPI createStops10(void* self, const GradientStop* stops, UINT count, int gamma, int extend, void** collection) {
    auto p = premultiplied(stops, count);
    return wine10(self, stops ? p.data() : nullptr, count, gamma, extend, collection);
}

HRESULT_ WINAPI createStops11(void* self, const GradientStop* stops, UINT count, int preSpace, int postSpace,
                              int precision, int extend, int interpolation, void** collection) {
    auto p = premultiplied(stops, count);
    return wine11(self, stops ? p.data() : nullptr, count, preSpace, postSpace, precision, extend, interpolation,
                  collection);
}

template <class F>
F method(void* object, int index) {
    return (F)(*(void***)object)[index];
}

void release(void* object) {
    if (object)
        method<ULONG(WINAPI*)(void*)>(object, UNKNOWN_RELEASE)(object);
}

// Wine's own DLLs say so in their headers ("Wine builtin DLL" where a DOS stub would be)
bool wineBuiltin(HMODULE module) { return std::memcmp((const char*)module + 0x40, "Wine builtin DLL", 16) == 0; }

bool inside(HMODULE module, void* fn) {
    const unsigned char* base = (const unsigned char*)module;
    uint32_t pe = *(const uint32_t*)(base + 0x3C);
    uint32_t size = *(const uint32_t*)(base + pe + 24 + 56);  // (OptionalHeader.SizeOfImage)
    return (const unsigned char*)fn >= base && (const unsigned char*)fn < base + size;
}

bool patch(void** table, int index, void* to, void** was) {
    DWORD old = 0;
    if (!VirtualProtect(&table[index], sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *was = table[index];
    table[index] = to;
    VirtualProtect(&table[index], sizeof(void*), old, &old);
    return true;
}


// --- layers ---

struct RectF {
    float left, top, right, bottom;
};
struct Matrix {
    float m11, m12, m21, m22, m31, m32;
};
struct SizeU {
    UINT width, height;
};
struct PixelFormat {
    UINT format, alphaMode;
};
struct ColorF {
    float r, g, b, a;
};
// D2D1_LAYER_PARAMETERS and …PARAMETERS1 (the same layout)
struct LayerParameters {
    RectF contentBounds;
    void* geometricMask;
    int maskAntialiasMode;
    Matrix maskTransform;
    float opacity;
    void* opacityBrush;
    int layerOptions;
};
struct BitmapProperties1 {
    PixelFormat pixelFormat;
    float dpiX, dpiY;
    UINT options;
    void* colorContext;
};
struct BrushProperties {
    float opacity;
    Matrix transform;
};

// (places in ID2D1RenderTarget / ID2D1DeviceContext's table, ID2D1Factory's, IDXGI*'s)
constexpr int RESOURCE_GET_FACTORY = 3;
constexpr int RT_CREATE_BITMAP_BRUSH = 7, RT_FILL_GEOMETRY = 23, RT_SET_TRANSFORM = 30, RT_GET_TRANSFORM = 31,
              RT_PUSH_LAYER = 40, RT_POP_LAYER = 41, RT_PUSH_CLIP = 45, RT_POP_CLIP = 46, RT_CLEAR = 47,
              RT_GET_PIXEL_FORMAT = 50, RT_GET_DPI = 52, RT_GET_PIXEL_SIZE = 54;
constexpr int DC_CREATE_BITMAP = 57, DC_GET_DEVICE = 73, DC_SET_TARGET = 74, DC_GET_TARGET = 75, DC_PUSH_LAYER = 86;
constexpr int FACTORY_CREATE_RECTANGLE = 5, FACTORY_CREATE_TRANSFORMED = 9;
constexpr float HUGE_EXTENT = 1e30f;

// a layer's bitmap: what it is for (one is used again only by a context of the same device, size,
// resolution and pixel format)
struct LayerBitmap {
    void* bitmap = nullptr;  // (held)
    void* device = nullptr;  // (held) the context's ID2D1Device
    SizeU size{};
    float dpiX = 96, dpiY = 96;
    UINT format = 0;
    bool fits(const LayerBitmap& o) const {
        return device == o.device && size.width == o.size.width && size.height == o.size.height && dpiX == o.dpiX &&
               dpiY == o.dpiY && format == o.format;
    }
};

struct Layer {
    void* previousTarget = nullptr;  // (held)
    LayerBitmap drawn;               // what is drawn inside the layer (bitmap null: nothing done)
    Matrix transform{};              // the transform when it was pushed
    LayerParameters parameters{};    // (geometricMask and opacityBrush held)
};

CRITICAL_SECTION_* layersLock = nullptr;
std::map<void*, std::vector<Layer>>* layers = nullptr;  // each device context's layers pushed (none: no entry)
// bitmaps to use again, the latest last; a few at most, of any context (a context's address is
// given again once it is gone: what is kept is matched by what it is, not by whose it was); none
// kept once the plugin's window is closed (forgetLayerBitmaps)
std::vector<LayerBitmap>* spareBitmaps = nullptr;
constexpr size_t SPARE_BITMAPS = 8;

void releaseBitmap(LayerBitmap& b) {
    release(b.bitmap);
    release(b.device);
    b.bitmap = b.device = nullptr;
}

void addRef(void* object) {
    if (object)
        method<ULONG(WINAPI*)(void*)>(object, 1)(object);
}

const Matrix IDENTITY = {1, 0, 0, 1, 0, 0};

void firstCall(const char* what, bool& said) {
    if (!said && getenv("TINYDAW_WINE_TRACE"))
        fprintf(stderr, "tinydaw-wine-host: first %s\n", what);
    said = true;
}

void pushLayer(void* dc, const LayerParameters* given) {
    static bool said = false;
    firstCall("PushLayer", said);
    Layer layer;
    if (given) {
        layer.parameters = *given;
        addRef(layer.parameters.geometricMask);
        addRef(layer.parameters.opacityBrush);
    } else {
        layer.parameters.contentBounds = {-HUGE_EXTENT, -HUGE_EXTENT, HUGE_EXTENT, HUGE_EXTENT};
        layer.parameters.maskTransform = IDENTITY;
        layer.parameters.opacity = 1;
    }
    method<void(WINAPI*)(void*, Matrix*)>(dc, RT_GET_TRANSFORM)(dc, &layer.transform);
    method<void(WINAPI*)(void*, void**)>(dc, DC_GET_TARGET)(dc, &layer.previousTarget);
    LayerBitmap& b = layer.drawn;
    if (layer.previousTarget)
        method<void(WINAPI*)(void*, void**)>(dc, DC_GET_DEVICE)(dc, &b.device);
    if (layer.previousTarget && b.device) {
        BitmapProperties1 props{};
        method<SizeU*(WINAPI*)(void*, SizeU*)>(dc, RT_GET_PIXEL_SIZE)(dc, &b.size);
        method<PixelFormat*(WINAPI*)(void*, PixelFormat*)>(dc, RT_GET_PIXEL_FORMAT)(dc, &props.pixelFormat);
        method<void(WINAPI*)(void*, float*, float*)>(dc, RT_GET_DPI)(dc, &b.dpiX, &b.dpiY);
        b.format = props.pixelFormat.format;
        EnterCriticalSection_(layersLock);
        auto& spare = *spareBitmaps;
        for (size_t i = spare.size(); i-- > 0;)
            if (spare[i].fits(b)) {
                release(b.device);  // (the spare's holds it too)
                b = spare[i];
                spare.erase(spare.begin() + (long)i);
                break;
            }
        LeaveCriticalSection_(layersLock);
        if (!b.bitmap && b.size.width && b.size.height) {
            props.pixelFormat.alphaMode = 1;  // (premultiplied: it is drawn onto the target)
            props.dpiX = b.dpiX, props.dpiY = b.dpiY;
            props.options = 1;  // (D2D1_BITMAP_OPTIONS_TARGET)
            using CreateBitmap = LONG(WINAPI*)(void*, SizeU, const void*, UINT, const BitmapProperties1*, void**);
            if (method<CreateBitmap>(dc, DC_CREATE_BITMAP)(dc, b.size, nullptr, 0, &props, &b.bitmap) < 0)
                b.bitmap = nullptr;
        }
        if (b.bitmap) {
            method<void(WINAPI*)(void*, void*)>(dc, DC_SET_TARGET)(dc, b.bitmap);
            ColorF clear{0, 0, 0, 0};
            method<void(WINAPI*)(void*, const ColorF*)>(dc, RT_CLEAR)(dc, &clear);
        }
    }
    if (!b.bitmap)
        releaseBitmap(b);  // (nothing done: no device held either)
    EnterCriticalSection_(layersLock);
    (*layers)[dc].push_back(layer);
    LeaveCriticalSection_(layersLock);
}

void popLayer(void* dc) {
    Layer layer;
    EnterCriticalSection_(layersLock);
    auto found = layers->find(dc);
    bool any = found != layers->end() && !found->second.empty();
    if (any) {
        layer = found->second.back();
        found->second.pop_back();
    }
    if (found != layers->end() && found->second.empty())
        layers->erase(found);  // (a context with no layers has no entry: it may be gone soon)
    LeaveCriticalSection_(layersLock);
    if (!any)
        return;
    if (layer.drawn.bitmap) {
        // back to the target, and the layer's content painted onto it: under the transform of when
        // it was pushed, as the layer's mask, content bounds and opacity brush are in its
        // coordinates; the bitmap brush undoes that transform, so the bitmap lands pixel for pixel
        method<void(WINAPI*)(void*, void*)>(dc, DC_SET_TARGET)(dc, layer.previousTarget);
        Matrix now{};
        method<void(WINAPI*)(void*, Matrix*)>(dc, RT_GET_TRANSFORM)(dc, &now);
        const Matrix& t = layer.transform;
        method<void(WINAPI*)(void*, const Matrix*)>(dc, RT_SET_TRANSFORM)(dc, &t);
        void* factory = nullptr;
        method<void(WINAPI*)(void*, void**)>(dc, RESOURCE_GET_FACTORY)(dc, &factory);
        void *shape = nullptr, *brush = nullptr;
        const LayerParameters& p = layer.parameters;
        float det = t.m11 * t.m22 - t.m12 * t.m21;
        Matrix inverse = IDENTITY;
        if (std::fabs(det) > 1e-12f)
            inverse = {t.m22 / det, -t.m12 / det, -t.m21 / det, t.m11 / det, (t.m21 * t.m32 - t.m22 * t.m31) / det,
                       (t.m12 * t.m31 - t.m11 * t.m32) / det};
        if (factory) {
            if (p.geometricMask) {
                method<LONG(WINAPI*)(void*, void*, const Matrix*, void**)>(factory, FACTORY_CREATE_TRANSFORMED)(
                    factory, p.geometricMask, &p.maskTransform, &shape);
            } else {
                // (the whole target, in the layer's coordinates: its corners through the inverse)
                float dpiX = 96, dpiY = 96;
                method<void(WINAPI*)(void*, float*, float*)>(dc, RT_GET_DPI)(dc, &dpiX, &dpiY);
                float w = layer.drawn.size.width * 96.0f / dpiX, h = layer.drawn.size.height * 96.0f / dpiY;
                float xs[4] = {0, w, 0, w}, ys[4] = {0, 0, h, h};
                RectF all{HUGE_EXTENT, HUGE_EXTENT, -HUGE_EXTENT, -HUGE_EXTENT};
                for (int i = 0; i < 4; i++) {
                    float x = xs[i] * inverse.m11 + ys[i] * inverse.m21 + inverse.m31;
                    float y = xs[i] * inverse.m12 + ys[i] * inverse.m22 + inverse.m32;
                    all.left = std::min(all.left, x), all.right = std::max(all.right, x);
                    all.top = std::min(all.top, y), all.bottom = std::max(all.bottom, y);
                }
                method<LONG(WINAPI*)(void*, const RectF*, void**)>(factory, FACTORY_CREATE_RECTANGLE)(factory, &all, &shape);
            }
        }
        BrushProperties bp{p.opacity, inverse};
        method<LONG(WINAPI*)(void*, void*, const void*, const BrushProperties*, void**)>(dc, RT_CREATE_BITMAP_BRUSH)(
            dc, layer.drawn.bitmap, nullptr, &bp, &brush);
        bool bounded = p.contentBounds.left > -HUGE_EXTENT / 2 && p.contentBounds.right < HUGE_EXTENT / 2;
        if (bounded)
            method<void(WINAPI*)(void*, const RectF*, int)>(dc, RT_PUSH_CLIP)(dc, &p.contentBounds, 1);
        if (shape && brush)
            method<void(WINAPI*)(void*, void*, void*, void*)>(dc, RT_FILL_GEOMETRY)(dc, shape, brush, p.opacityBrush);
        if (bounded)
            method<void(WINAPI*)(void*)>(dc, RT_POP_CLIP)(dc);
        method<void(WINAPI*)(void*, const Matrix*)>(dc, RT_SET_TRANSFORM)(dc, &now);
        release(brush);
        release(shape);
        release(factory);
        LayerBitmap oldest;
        EnterCriticalSection_(layersLock);
        auto& spare = *spareBitmaps;
        if (spare.size() >= SPARE_BITMAPS) {
            oldest = spare.front();
            spare.erase(spare.begin());
        }
        spare.push_back(layer.drawn);
        LeaveCriticalSection_(layersLock);
        releaseBitmap(oldest);
    }
    release(layer.previousTarget);
    release(layer.parameters.geometricMask);
    release(layer.parameters.opacityBrush);
}

void WINAPI pushLayerRT(void* dc, const LayerParameters* parameters, void* /*layer*/) { pushLayer(dc, parameters); }
void WINAPI pushLayerDC(void* dc, const LayerParameters* parameters, void* /*layer*/) { pushLayer(dc, parameters); }
void WINAPI popLayerRT(void* dc) { popLayer(dc); }

// --- vertical blank ---

const Guid IID_IDXGIFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
constexpr int FACTORY_ENUM_ADAPTERS = 7, ADAPTER_ENUM_OUTPUTS = 7, OUTPUT_WAIT_FOR_VBLANK = 10;
constexpr double REFRESH_HZ = 60;

// until the next refresh of a 60 Hz screen (Wine can't tell the real one here)
LONG WINAPI waitForVBlank(void*) {
    static bool said = false;
    firstCall("WaitForVBlank", said);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    double t = now.tv_sec + now.tv_nsec * 1e-9, period = 1 / REFRESH_HZ;
    double next = (std::floor(t / period) + 1) * period;
    timespec until{(time_t)next, (long)((next - std::floor(next)) * 1e9)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR) {
    }
    return 0;
}


// --- outlines ---

struct Point2F {
    float x, y;
};
struct Polyline {
    std::vector<Point2F> points;
    bool closed = false;
};

// an ID2D1SimplifiedGeometrySink that keeps the figures as polylines (Simplify(LINES) writes into it)
struct LineSink {
    void** vtable;
    std::vector<Polyline> figures;
    bool beziers = false;
};

LONG WINAPI sinkQuery(void* self, const Guid*, void** out) {
    *out = self;  // (only Wine's Simplify holds it, for the call)
    return 0;
}
ULONG WINAPI sinkRef(void*) { return 1; }
void WINAPI sinkFillMode(void*, int) {}
void WINAPI sinkSegmentFlags(void*, int) {}
void WINAPI sinkBegin(void* self, Point2F start, int) {
    auto* s = (LineSink*)self;
    s->figures.push_back({});
    s->figures.back().points.push_back(start);
}
void WINAPI sinkLines(void* self, const Point2F* points, UINT count) {
    auto* s = (LineSink*)self;
    if (s->figures.empty())
        s->figures.push_back({});
    s->figures.back().points.insert(s->figures.back().points.end(), points, points + count);
}
void WINAPI sinkBeziers(void* self, const void*, UINT) { ((LineSink*)self)->beziers = true; }
void WINAPI sinkEnd(void* self, int end) {
    auto* s = (LineSink*)self;
    if (!s->figures.empty())
        s->figures.back().closed = end == 1;  // (D2D1_FIGURE_END_CLOSED)
}
LONG WINAPI sinkClose(void*) { return 0; }
void* sinkTable[] = {(void*)sinkQuery, (void*)sinkRef,  (void*)sinkRef,      (void*)sinkFillMode, (void*)sinkSegmentFlags,
                     (void*)sinkBegin, (void*)sinkLines, (void*)sinkBeziers, (void*)sinkEnd,      (void*)sinkClose};

constexpr int GEOMETRY_SIMPLIFY = 9, FACTORY_CREATE_PATH = 10, PATH_OPEN = 17;
constexpr int SINK_BEGIN = 5, SINK_ADD_LINES = 6, SINK_END = 8, SINK_CLOSE = 9;
constexpr int RT_DRAW_GEOMETRY = 22;
using DrawGeometryFn = void(WINAPI*)(void* dc, void* geometry, void* brush, float width, void* style);
DrawGeometryFn wineDrawGeometry = nullptr;

// splits and merges a figure so no point has its neighbours in line with it; true if it changed
bool straighten(const Polyline& in, std::vector<Polyline>& out) {
    std::vector<Point2F> p;
    for (const auto& q : in.points)  // (repeated points go)
        if (p.empty() || q.x != p.back().x || q.y != p.back().y)
            p.push_back(q);
    if (in.closed && p.size() > 1 && p.front().x == p.back().x && p.front().y == p.back().y)
        p.pop_back();
    bool changed = p.size() != in.points.size();
    // (a closed figure that turns straight back somewhere is opened there: begun at that point and
    // ended at it again, so its closing side is drawn as any other, and it is split as an open one)
    if (in.closed && p.size() == 2) {  // (there and straight back: a line)
        out.push_back({p, false});
        return true;
    }
    if (in.closed && p.size() >= 3)
        for (size_t i = 0, n = p.size(); i < n; i++) {
            const Point2F &a = p[(i + n - 1) % n], &b = p[i], &c = p[(i + 1) % n];
            float ux = b.x - a.x, uy = b.y - a.y, vx = c.x - b.x, vy = c.y - b.y;
            if (ux * vy - uy * vx == 0.0f && ux * vx + uy * vy < 0) {
                Polyline open;
                open.points.assign(p.begin() + (long)i, p.end());
                open.points.insert(open.points.end(), p.begin(), p.begin() + (long)i + 1);
                straighten(open, out);
                return true;
            }
        }
    Polyline current;
    current.closed = in.closed;
    for (size_t i = 0; i < p.size(); i++) {
        if (current.points.size() >= 2) {
            const Point2F &a = current.points[current.points.size() - 2], &b = current.points.back(), &c = p[i];
            float ux = b.x - a.x, uy = b.y - a.y, vx = c.x - b.x, vy = c.y - b.y;
            float cross = ux * vy - uy * vx, dot = ux * vx + uy * vy;
            if (cross == 0.0f) {
                changed = true;
                if (dot > 0) {  // (straight on: b isn't needed)
                    current.points.back() = c;
                    continue;
                }
                // (straight back: the figure ends at b, and goes on from there as a new one)
                current.closed = false;
                out.push_back(current);
                current.points = {b};
            }
        }
        current.points.push_back(p[i]);
    }
    if (current.points.size() >= 2)
        out.push_back(current);
    // (a closed figure's corner where it ends and begins again)
    if (in.closed && out.size() == 1 && out[0].points.size() >= 3) {
        auto& q = out[0].points;
        auto inLine = [](const Point2F& a, const Point2F& b, const Point2F& c) {
            return (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x) == 0.0f;
        };
        while (q.size() >= 3 && inLine(q[q.size() - 2], q.back(), q[0]))
            q.pop_back(), changed = true;
        while (q.size() >= 3 && inLine(q.back(), q[0], q[1]))
            q.erase(q.begin()), changed = true;
    }
    return changed;
}

void WINAPI drawGeometry(void* dc, void* geometry, void* brush, float width, void* style) {
    LineSink sink;
    sink.vtable = sinkTable;
    Matrix t{};
    method<void(WINAPI*)(void*, Matrix*)>(dc, RT_GET_TRANSFORM)(dc, &t);
    float scale = std::sqrt(std::fabs(t.m11 * t.m22 - t.m12 * t.m21));
    float tolerance = 0.25f / (scale > 1e-6f ? scale : 1.0f);  // (a quarter of a pixel, as Direct2D's default)
    constexpr int SIMPLIFY_LINES = 1;
    if (!geometry ||
        method<LONG(WINAPI*)(void*, int, const Matrix*, float, void*)>(geometry, GEOMETRY_SIMPLIFY)(
            geometry, SIMPLIFY_LINES, nullptr, tolerance, &sink) < 0 ||
        sink.beziers) {
        wineDrawGeometry(dc, geometry, brush, width, style);
        return;
    }
    std::vector<Polyline> figures;
    bool changed = false;
    for (const auto& f : sink.figures)
        changed |= straighten(f, figures);
    if (!changed) {  // (nothing Wine gets wrong: its own path, as it was)
        wineDrawGeometry(dc, geometry, brush, width, style);
        return;
    }
    void *factory = nullptr, *path = nullptr, *pathSink = nullptr;
    method<void(WINAPI*)(void*, void**)>(dc, RESOURCE_GET_FACTORY)(dc, &factory);
    bool ok = factory && method<LONG(WINAPI*)(void*, void**)>(factory, FACTORY_CREATE_PATH)(factory, &path) >= 0;
    ok = ok && method<LONG(WINAPI*)(void*, void**)>(path, PATH_OPEN)(path, &pathSink) >= 0;
    if (ok) {
        constexpr int BEGIN_HOLLOW = 1, END_OPEN = 0, END_CLOSED = 1;
        for (const auto& f : figures) {
            method<void(WINAPI*)(void*, Point2F, int)>(pathSink, SINK_BEGIN)(pathSink, f.points[0], BEGIN_HOLLOW);
            method<void(WINAPI*)(void*, const Point2F*, UINT)>(pathSink, SINK_ADD_LINES)(
                pathSink, f.points.data() + 1, (UINT)(f.points.size() - 1));
            method<void(WINAPI*)(void*, int)>(pathSink, SINK_END)(pathSink, f.closed ? END_CLOSED : END_OPEN);
        }
        ok = method<LONG(WINAPI*)(void*)>(pathSink, SINK_CLOSE)(pathSink) >= 0;
    }
    wineDrawGeometry(dc, ok ? path : geometry, brush, width, style);
    release(pathSink);
    release(path);
    release(factory);
}


// --- paths Wine couldn't fill ---

// When Wine can't triangulate a path for filling ("Triangle not found"), its Close() frees the
// path's figures but keeps their count (d2d_path_geometry_free_figures), and the methods that don't
// look at the path's state first then read through a null pointer: Simplify (the outlines above
// call it for every outline), FillContainsPoint and StrokeContainsPoint. Serum 2's wavetable view
// makes such paths while its WT POS knob turns: its window's thread crashed. They are answered as
// Wine's other methods answer for such a path (GetFigureCount looks at the state): wrong state.
constexpr int GEOMETRY_STROKE_CONTAINS = 6, GEOMETRY_FILL_CONTAINS = 7, PATH_GET_FIGURE_COUNT = 20;
constexpr HRESULT_ D2DERR_WRONG_STATE = (HRESULT_)0x88990001;
using SimplifyFn = HRESULT_(WINAPI*)(void* path, int option, const Matrix* transform, float tolerance, void* sink);
using StrokeContainsFn = HRESULT_(WINAPI*)(void* path, Point2F point, float width, void* style, const Matrix* transform,
                                           float tolerance, BOOL* contains);
using FillContainsFn = HRESULT_(WINAPI*)(void* path, Point2F point, const Matrix* transform, float tolerance,
                                         BOOL* contains);
SimplifyFn wineSimplify = nullptr;
StrokeContainsFn wineStrokeContains = nullptr;
FillContainsFn wineFillContains = nullptr;

bool pathClosed(void* path) {
    UINT count = 0;
    return method<HRESULT_(WINAPI*)(void*, UINT*)>(path, PATH_GET_FIGURE_COUNT)(path, &count) >= 0;
}

HRESULT_ WINAPI simplifyPath(void* path, int option, const Matrix* transform, float tolerance, void* sink) {
    return pathClosed(path) ? wineSimplify(path, option, transform, tolerance, sink) : D2DERR_WRONG_STATE;
}

HRESULT_ WINAPI strokeContains(void* path, Point2F point, float width, void* style, const Matrix* transform,
                               float tolerance, BOOL* contains) {
    if (pathClosed(path))
        return wineStrokeContains(path, point, width, style, transform, tolerance, contains);
    if (contains)
        *contains = FALSE;
    return D2DERR_WRONG_STATE;
}

HRESULT_ WINAPI fillContains(void* path, Point2F point, const Matrix* transform, float tolerance, BOOL* contains) {
    if (pathClosed(path))
        return wineFillContains(path, point, transform, tolerance, contains);
    if (contains)
        *contains = FALSE;
    return D2DERR_WRONG_STATE;
}


// --- finding font families ---

constexpr int COLLECTION_FAMILY_COUNT = 3, COLLECTION_FIND_FAMILY = 5, DWRITE_FACTORY_SYSTEM_COLLECTION = 3;
const Guid IID_IDWriteFactory = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
using FindFamilyFn = LONG(WINAPI*)(void* collection, const char16_t* name, UINT* index, BOOL* exists);
FindFamilyFn wineFindFamily = nullptr;

struct FamilyKey {
    void* collection;
    UINT count;
    std::u16string name;
    bool operator<(const FamilyKey& o) const {
        return collection != o.collection ? collection < o.collection
               : count != o.count         ? count < o.count
                                          : name < o.name;
    }
};
CRITICAL_SECTION_* familiesLock = nullptr;
std::map<FamilyKey, std::pair<UINT, BOOL>>* families = nullptr;
// the collections in `families`' keys, held (else a collection let go, and a new one made where it
// was, would get its answers)
std::vector<void*>* familyCollections = nullptr;

// forgets every answer (the collections they hold let go)
void forgetFamilies() {
    std::vector<void*> going;
    EnterCriticalSection_(familiesLock);
    families->clear();
    going.swap(*familyCollections);
    LeaveCriticalSection_(familiesLock);
    for (void* c : going)
        release(c);
}

LONG WINAPI findFamily(void* collection, const char16_t* name, UINT* index, BOOL* exists) {
    if (!collection || !name || !index || !exists)
        return wineFindFamily(collection, name, index, exists);
    UINT count = method<UINT(WINAPI*)(void*)>(collection, COLLECTION_FAMILY_COUNT)(collection);
    FamilyKey key{collection, count, name};
    EnterCriticalSection_(familiesLock);
    auto found = families->find(key);
    bool known = found != families->end();
    if (known)
        *index = found->second.first, *exists = found->second.second;
    LeaveCriticalSection_(familiesLock);
    if (known)
        return 0;
    LONG hr = wineFindFamily(collection, name, index, exists);
    if (hr >= 0) {
        EnterCriticalSection_(familiesLock);
        bool full = families->size() > 4096;
        LeaveCriticalSection_(familiesLock);
        if (full)
            forgetFamilies();
        EnterCriticalSection_(familiesLock);
        auto& held = *familyCollections;
        if (std::find(held.begin(), held.end(), collection) == held.end()) {
            addRef(collection);
            held.push_back(collection);
        }
        (*families)[key] = {*index, *exists};
        LeaveCriticalSection_(familiesLock);
    }
    return hr;
}


// --- paths Wine recurses on without end ---

// Filling a path, Wine's Direct2D inserts each of its sides into a triangulation; for a side whose
// end it finds in line with an edge ("collinear": d2d_point_ccw() exactly 0, with floats), it goes
// on from that edge's other end, which may lie beyond the end, from where the edge back is in line
// too: d2d_cdt_insert_segment() calls itself back and forth without end, until the thread's stack
// runs out (Serum 2's wavetable view, as its WT POS knob turned: the plugin crashed). Windows' own
// Direct2D doesn't do this. So Close() is done on a thread of its own, its stack bounded: run out
// there, in Direct2D, the thread is turned to an end (as Windows lets an exception handler do: a
// new instruction and stack for it) and the path is answered as one Wine couldn't fill (as Wine
// answers "Triangle not found"); the plugin goes on, and the next Close() has a new thread. (Such a
// path is left half made: Wine has it closed, without its triangles, so it fills as nothing or, with
// curves, as slivers of them; and its triangulation's memory stays taken. Seen? Not yet: on a
// thread of its own, the Close() that recursed without end on Serum 2's thread ended every time.)
constexpr SIZE_T CLOSE_STACK = 16u << 20;
constexpr LONG E_FAIL_ = (LONG)0x80004005;
using CloseFn = LONG(WINAPI*)(void* sink);
CloseFn wineClose = nullptr;
HMODULE d2dModule = nullptr;

std::mutex closeCallers;  // (one Close() at a time on the closing thread)
std::mutex closeLock;
std::condition_variable closeChanged;
void* closeSink = nullptr;
bool closeAsked = false, closeAnswered = false, closerAlive = false;
LONG closeResult = 0;
std::atomic<DWORD> closerThread{0};
HANDLE closerHandle = nullptr;
std::atomic<uintptr_t> closerTop{0};
std::atomic<int> endlessPaths{0};

__attribute__((noreturn)) void WINAPI closerGivesUp() {
    {
        std::lock_guard<std::mutex> hold(closeLock);
        closeResult = E_FAIL_;
        closeAnswered = true;
        closerAlive = false;
        closerThread = 0;
        endlessPaths++;
    }
    closeChanged.notify_all();
    ExitThread(0);
}

DWORD WINAPI closerMain(LPVOID) {
    volatile char top = 0;
    closerTop = (uintptr_t)&top;
    closerThread = GetCurrentThreadId();
    std::unique_lock<std::mutex> hold(closeLock);
    for (;;) {
        closeChanged.wait(hold, [] { return closeAsked; });
        closeAsked = false;
        void* sink = closeSink;
        hold.unlock();
        LONG result = wineClose(sink);
        hold.lock();
        closeResult = result;
        closeAnswered = true;
        closeChanged.notify_all();
    }
}

LONG WINAPI closerRanOut(EXCEPTION_POINTERS* info) {
    constexpr DWORD STACK_OVERFLOW = 0xC00000FD;
    const EXCEPTION_RECORD* r = info ? info->ExceptionRecord : nullptr;
    if (!r || r->ExceptionCode != STACK_OVERFLOW || !info->ContextRecord || GetCurrentThreadId() != closerThread ||
        !d2dModule || !inside(d2dModule, r->ExceptionAddress))
        return EXCEPTION_CONTINUE_SEARCH;
    // (x64 CONTEXT: Rsp at 0x98, Rip at 0xF8; the stack as a call to closerGivesUp from the thread's own
    // frame would leave it, above the frames that recursed)
    char* context = (char*)info->ContextRecord;
    uint64_t rsp = ((uint64_t)closerTop.load() - 4096) & ~(uint64_t)15;
    *(uint64_t*)(context + 0x98) = rsp - 8;
    *(uint64_t*)(context + 0xF8) = (uint64_t)(uintptr_t)closerGivesUp;
    return EXCEPTION_CONTINUE_EXECUTION;
}

LONG WINAPI closeBounded(void* sink) {
    if (GetCurrentThreadId() == closerThread)
        return wineClose(sink);
    std::lock_guard<std::mutex> one(closeCallers);
    std::unique_lock<std::mutex> hold(closeLock);
    if (!closerAlive) {
        HANDLE thread = CreateThread(nullptr, CLOSE_STACK, closerMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!thread) {
            hold.unlock();
            return wineClose(sink);
        }
        if (closerHandle)
            CloseHandle(closerHandle);
        closerHandle = thread;
        closerAlive = true;
    }
    int endless = endlessPaths.load();
    closeSink = sink;
    closeAsked = true;
    closeAnswered = false;
    closeChanged.notify_all();
    // (a closing thread that ended otherwise, as Wine ends one it can't even tell of its stack running
    // out: the path answered as unfillable, not waited for for ever)
    while (!closeChanged.wait_for(hold, std::chrono::milliseconds(100), [] { return closeAnswered; }))
        if (WaitForSingleObject(closerHandle, 0) == 0) {
            closerAlive = false;
            closerThread = 0;
            closeAnswered = true;
            closeResult = E_FAIL_;
            endlessPaths++;
        }
    LONG result = closeResult;
    if (endlessPaths.load() != endless && getenv("TINYDAW_WINE_TRACE"))
        fprintf(stderr, "tinydaw-wine-host: a path Wine's Direct2D recursed on without end: not filled\n");
    return result;
}

}  // namespace

void fixWineDirect2D() {
    static bool tried = false;
    if (tried)
        return;
    HMODULE d2d = GetModuleHandleW(u"d2d1.dll");
    if (!d2d)
        return;  // (no plugin here draws with Direct2D; asked again when one is loaded)
    tried = true;
    bool trace = getenv("TINYDAW_WINE_TRACE") != nullptr;
    if (getenv("TINYDAW_WINE_D2D_AS_IS") || !wineBuiltin(d2d))
        return;
    using CreateFactory = HRESULT_(WINAPI*)(int type, const Guid* iid, const void* options, void** factory);
    using CreateD3D = HRESULT_(WINAPI*)(void* adapter, int driver, HMODULE software, UINT flags, const int* levels,
                                        UINT count, UINT sdk, void** device, int* level, void** context);
    auto createFactory = (CreateFactory)GetProcAddress(d2d, "D2D1CreateFactory");
    HMODULE d3d11 = LoadLibraryW(u"d3d11.dll");
    auto createD3D = d3d11 ? (CreateD3D)GetProcAddress(d3d11, "D3D11CreateDevice") : nullptr;
    if (!createFactory || !createD3D)
        return;
    void *factory = nullptr, *d3d = nullptr, *dxgi = nullptr, *device = nullptr, *context = nullptr;
    constexpr UINT BGRA_SUPPORT = 0x20, SDK_VERSION = 7;
    constexpr int HARDWARE = 1, WARP = 5;
    bool ok = createFactory(0, &IID_ID2D1Factory1, nullptr, &factory) >= 0 && factory;
    ok = ok && (createD3D(nullptr, HARDWARE, nullptr, BGRA_SUPPORT, nullptr, 0, SDK_VERSION, &d3d, nullptr, nullptr) >= 0 ||
                createD3D(nullptr, WARP, nullptr, BGRA_SUPPORT, nullptr, 0, SDK_VERSION, &d3d, nullptr, nullptr) >= 0);
    ok = ok && method<HRESULT_(WINAPI*)(void*, const Guid*, void**)>(d3d, UNKNOWN_QUERY)(d3d, &IID_IDXGIDevice, &dxgi) >= 0;
    ok = ok && method<HRESULT_(WINAPI*)(void*, void*, void**)>(factory, FACTORY1_CREATE_DEVICE)(factory, dxgi, &device) >= 0;
    ok = ok && method<HRESULT_(WINAPI*)(void*, int, void**)>(device, DEVICE_CREATE_CONTEXT)(device, 0, &context) >= 0;
    if (ok && context) {
        void** table = *(void***)context;
        // (both entries must be Wine's d2d1's own code, else the table isn't what it is taken for)
        if (inside(d2d, table[RT_CREATE_GRADIENT_STOPS]) && inside(d2d, table[DC_CREATE_GRADIENT_STOPS]) &&
            table[RT_CREATE_GRADIENT_STOPS] != table[DC_CREATE_GRADIENT_STOPS]) {
            patch(table, RT_CREATE_GRADIENT_STOPS, (void*)createStops10, (void**)&wine10);
            patch(table, DC_CREATE_GRADIENT_STOPS, (void*)createStops11, (void**)&wine11);
            if (trace)
                fprintf(stderr, "tinydaw-wine-host: Wine's Direct2D gradients corrected\n");
            // outlines, through straightened paths
            if (inside(d2d, table[RT_DRAW_GEOMETRY])) {
                patch(table, RT_DRAW_GEOMETRY, (void*)drawGeometry, (void**)&wineDrawGeometry);
                if (trace)
                    fprintf(stderr, "tinydaw-wine-host: Direct2D outlines straightened\n");
            }
            // layers, if Wine's are still stubs (left alone once Wine does them: then they'd be
            // nothing like this little code)
            if (inside(d2d, table[RT_PUSH_LAYER]) && inside(d2d, table[DC_PUSH_LAYER]) && inside(d2d, table[RT_POP_LAYER])) {
                static CRITICAL_SECTION_ lock;
                InitializeCriticalSection_(&lock);
                layersLock = &lock;
                layers = new std::map<void*, std::vector<Layer>>();
                spareBitmaps = new std::vector<LayerBitmap>();
                void* was = nullptr;
                patch(table, RT_PUSH_LAYER, (void*)pushLayerRT, &was);
                patch(table, DC_PUSH_LAYER, (void*)pushLayerDC, &was);
                patch(table, RT_POP_LAYER, (void*)popLayerRT, &was);
                if (trace)
                    fprintf(stderr, "tinydaw-wine-host: Direct2D layers done by the host\n");
            }
        } else if (trace) {
            fprintf(stderr, "tinydaw-wine-host: Wine's Direct2D isn't as expected; gradients left as they are\n");
        }
    } else if (trace) {
        fprintf(stderr, "tinydaw-wine-host: no Direct2D device context to correct gradients with\n");
    }
    // paths Wine couldn't fill: answered, not read through a null pointer (Wine's paths share one table)
    void* path = nullptr;
    if (factory && method<HRESULT_(WINAPI*)(void*, void**)>(factory, FACTORY_CREATE_PATH)(factory, &path) >= 0 && path) {
        void** table = *(void***)path;
        if (inside(d2d, table[GEOMETRY_SIMPLIFY]) && inside(d2d, table[GEOMETRY_STROKE_CONTAINS]) &&
            inside(d2d, table[GEOMETRY_FILL_CONTAINS]) && inside(d2d, table[PATH_GET_FIGURE_COUNT])) {
            patch(table, GEOMETRY_SIMPLIFY, (void*)simplifyPath, (void**)&wineSimplify);
            patch(table, GEOMETRY_STROKE_CONTAINS, (void*)strokeContains, (void**)&wineStrokeContains);
            patch(table, GEOMETRY_FILL_CONTAINS, (void*)fillContains, (void**)&wineFillContains);
            if (trace)
                fprintf(stderr, "tinydaw-wine-host: Direct2D paths Wine couldn't fill guarded\n");
        }
        // (and filling them: Close(), on its sink's table, shared by all paths too)
        void* sink = nullptr;
        if (method<LONG(WINAPI*)(void*, void**)>(path, PATH_OPEN)(path, &sink) >= 0 && sink) {
            void** sinks = *(void***)sink;
            if (inside(d2d, sinks[SINK_CLOSE]) && !getenv("TINYDAW_WINE_PATHS_AS_IS")) {
                d2dModule = d2d;
                AddVectoredExceptionHandler(1, closerRanOut);
                patch(sinks, SINK_CLOSE, (void*)closeBounded, (void**)&wineClose);
                if (trace)
                    fprintf(stderr, "tinydaw-wine-host: Direct2D paths filled with a bounded stack\n");
            }
            release(sink);
        }
    }
    release(path);
    release(context);
    release(device);
    release(dxgi);
    release(d3d);
    release(factory);
}

void forgetLayerBitmaps() {
    if (!layersLock)
        return;
    std::vector<LayerBitmap> going;
    size_t contexts = 0, pushed = 0;
    EnterCriticalSection_(layersLock);
    going.swap(*spareBitmaps);
    contexts = layers->size();
    for (auto& [dc, stack] : *layers)
        pushed += stack.size();
    LeaveCriticalSection_(layersLock);
    for (auto& b : going)
        releaseBitmap(b);
    if (getenv("TINYDAW_WINE_TRACE"))
        fprintf(stderr, "tinydaw-wine-host: %zu layer bitmaps let go; %zu contexts with %zu layers still pushed\n",
                going.size(), contexts, pushed);
}

void forgetFontLookups() {
    if (familiesLock)
        forgetFamilies();
}

void fixWineDxgi() {
    static bool tried = false;
    if (tried)
        return;
    HMODULE dxgi = GetModuleHandleW(u"dxgi.dll");
    if (!dxgi)
        return;
    tried = true;
    bool trace = getenv("TINYDAW_WINE_TRACE") != nullptr;
    if (getenv("TINYDAW_WINE_D2D_AS_IS") || !wineBuiltin(dxgi))
        return;  // (DXVK's dxgi has its own)
    using CreateFactory1 = LONG(WINAPI*)(const Guid* iid, void** factory);
    auto create = (CreateFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1");
    void *factory = nullptr, *adapter = nullptr, *output = nullptr;
    bool ok = create && create(&IID_IDXGIFactory1, &factory) >= 0 && factory;
    ok = ok && method<LONG(WINAPI*)(void*, UINT, void**)>(factory, FACTORY_ENUM_ADAPTERS)(factory, 0, &adapter) >= 0;
    ok = ok && method<LONG(WINAPI*)(void*, UINT, void**)>(adapter, ADAPTER_ENUM_OUTPUTS)(adapter, 0, &output) >= 0;
    if (trace)  // (each output's monitor, which JUCE matches with its window's)
        for (UINT i = 0; adapter; i++) {
            void* o = nullptr;
            if (method<LONG(WINAPI*)(void*, UINT, void**)>(adapter, ADAPTER_ENUM_OUTPUTS)(adapter, i, &o) < 0 || !o)
                break;
            unsigned char desc[96] = {0};
            method<LONG(WINAPI*)(void*, void*)>(o, 7)(o, desc);
            fprintf(stderr, "tinydaw-wine-host: DXGI output %u: attached %d, monitor %p\n", i, *(int*)(desc + 80),
                    *(void**)(desc + 88));
            release(o);
        }
    if (ok && output) {
        void** table = *(void***)output;
        void* wine = table[OUTPUT_WAIT_FOR_VBLANK];
        // (only if it is Wine's stub: it answers at once, E_NOTIMPL)
        if (inside(dxgi, wine) && ((LONG(WINAPI*)(void*))wine)(output) < 0) {
            void* was = nullptr;
            patch(table, OUTPUT_WAIT_FOR_VBLANK, (void*)waitForVBlank, &was);
            if (trace)
                fprintf(stderr, "tinydaw-wine-host: DXGI's WaitForVBlank done by the host\n");
        }
    }
    release(output);
    release(adapter);
    release(factory);
}

void fixWineDirectWrite() {
    static bool tried = false;
    if (tried)
        return;
    HMODULE dwrite = GetModuleHandleW(u"dwrite.dll");
    if (!dwrite)
        return;
    tried = true;
    bool trace = getenv("TINYDAW_WINE_TRACE") != nullptr;
    if (getenv("TINYDAW_WINE_D2D_AS_IS") || !wineBuiltin(dwrite))
        return;
    using CreateFactory = LONG(WINAPI*)(int type, const Guid* iid, void** factory);
    auto create = (CreateFactory)GetProcAddress(dwrite, "DWriteCreateFactory");
    void *factory = nullptr, *collection = nullptr;
    bool ok = create && create(0, &IID_IDWriteFactory, &factory) >= 0 && factory;
    ok = ok && method<LONG(WINAPI*)(void*, void**, BOOL)>(factory, DWRITE_FACTORY_SYSTEM_COLLECTION)(factory, &collection,
                                                                                                    FALSE) >= 0;
    if (ok && collection) {
        void** table = *(void***)collection;
        if (inside(dwrite, table[COLLECTION_FIND_FAMILY])) {
            static CRITICAL_SECTION_ lock;
            InitializeCriticalSection_(&lock);
            familiesLock = &lock;
            families = new std::map<FamilyKey, std::pair<UINT, BOOL>>();
            familyCollections = new std::vector<void*>();
            patch(table, COLLECTION_FIND_FAMILY, (void*)findFamily, (void**)&wineFindFamily);
            if (trace)
                fprintf(stderr, "tinydaw-wine-host: DirectWrite family lookups remembered\n");
        }
    }
    release(collection);
    release(factory);
}

// --- (tests) a filled path Wine must triangulate ---

// tinydaw-wine-host.exe --path-test <points> <shape> [as-is]: one filled figure of so many points,
// closed, as plugins draw a wave filled down to its line (shape 0: a zigzag between that line and
// above it; 1: a flat run along it; 2: a wave, in whole pixels; 3: a zigzag across it, which its
// closing side crosses at every turn); says what Close() answered. With
// "as-is", Wine's Direct2D isn't corrected first
int pathTest(int points, int shape, bool asIs) {
    HMODULE d2d = LoadLibraryW(u"d2d1.dll");
    if (!asIs)
        fixWineDirect2D();
    using CreateFactory = HRESULT_(WINAPI*)(int type, const Guid* iid, const void* options, void** factory);
    auto createFactory = d2d ? (CreateFactory)GetProcAddress(d2d, "D2D1CreateFactory") : nullptr;
    void *factory = nullptr, *path = nullptr, *sink = nullptr;
    if (!createFactory || createFactory(0, &IID_ID2D1Factory1, nullptr, &factory) < 0 || !factory ||
        method<LONG(WINAPI*)(void*, void**)>(factory, FACTORY_CREATE_PATH)(factory, &path) < 0 ||
        method<LONG(WINAPI*)(void*, void**)>(path, PATH_OPEN)(path, &sink) < 0) {
        fprintf(stderr, "path test: no Direct2D\n");
        return 1;
    }
    std::vector<Point2F> line;
    for (int i = 1; i <= points; i++) {
        float y = shape == 0   ? (i % 2 ? 10.f : 0.f)
                  : shape == 1 ? 0.f
                  : shape == 2 ? (float)(int)(20 * std::sin(i * 0.01))
                               : (i % 2 ? 10.f : -10.f);
        line.push_back({(float)i, y});
    }
    if (shape == 1)
        line.push_back({(float)points, 10.f});
    constexpr int BEGIN_FILLED = 0, END_CLOSED = 1;
    method<void(WINAPI*)(void*, Point2F, int)>(sink, SINK_BEGIN)(sink, Point2F{0, 0}, BEGIN_FILLED);
    method<void(WINAPI*)(void*, const Point2F*, UINT)>(sink, SINK_ADD_LINES)(sink, line.data(), (UINT)line.size());
    method<void(WINAPI*)(void*, int)>(sink, SINK_END)(sink, END_CLOSED);
    LONG closed = method<LONG(WINAPI*)(void*)>(sink, SINK_CLOSE)(sink);
    printf("path test: %d points, shape %d: Close() answered %08x\n", points, shape, (unsigned)closed);
    fflush(stdout);
    release(sink);
    release(path);
    release(factory);
    return 0;
}
