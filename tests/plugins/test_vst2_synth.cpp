// Tiny Test Synth: a VST2 instrument for Tiny DAW's tests (make plugins → build/test-plugins/TinyTestSynth.so),
// written on plugins/vst2.h the way a VST2 plugin is written.
//
// A sine for every note held (no envelope: a note-off silences it at once), 16 voices.
// Parameters: 0 Volume (amplitude 0…1, default 0.5), 1 Octave (−2…+2 in fifths of the range, default
// 0.5 = none), 2 Tempo seen (the host's tempo / 1000, as audioMasterGetTime told it while processing;
// read only). Its settings are a chunk ("TTS1", volume, octave). It reports 16 frames of latency.
// Its window (Xlib; on Windows a Win32 child window, for the Wine tests: make win-plugins): a bar
// showing the volume; click to set it (sent to the host as automation), right-click asks the host
// for a bigger window.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <X11/Xlib.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>

#include "plugins/vst2.h"

namespace {

constexpr int VOICES = 16;
constexpr float PI2 = 6.283185307179586f;

struct Voice {
    int pitch = -1, channel = 0;
    double phase = 0;
};

struct Pending {
    int frame;
    unsigned char status, data1, data2;
};

struct Synth {
    vst2::Effect effect;
    vst2::Callback host;
    float volume = 0.5f, octave = 0.5f, tempoSeen = 0;
    double rate = 44100;
    Voice voices[VOICES];
    Pending pending[4096];
    int pendingCount = 0;
    char chunk[12];
    // the window
#ifdef _WIN32
    HWND window = nullptr;
#else
    Display* display = nullptr;
    ::Window window = 0;
#endif
    int w = 320, h = 160;
};

Synth* of(vst2::Effect* e) { return (Synth*)e->object; }

int octaveShift(float v) { return (int)std::lround(v * 4) - 2; }

void noteOn(Synth* s, int pitch, int channel) {
    for (auto& v : s->voices)
        if (v.pitch < 0) {
            v.pitch = pitch;
            v.channel = channel;
            v.phase = 0;
            return;
        }
}

void noteOff(Synth* s, int pitch, int channel) {
    for (auto& v : s->voices)
        if (v.pitch == pitch && v.channel == channel)
            v.pitch = -1;
}

void apply(Synth* s, const Pending& p) {
    int kind = p.status & 0xF0, channel = p.status & 0x0F;
    if (kind == 0x90 && p.data2 > 0)
        noteOn(s, p.data1, channel);
    else if (kind == 0x80 || kind == 0x90)
        noteOff(s, p.data1, channel);
    else if (kind == 0xB0 && (p.data1 == 120 || p.data1 == 123))
        for (auto& v : s->voices)
            v.pitch = -1;
}

void processReplacing(vst2::Effect* e, float** /*in*/, float** out, int32_t frames) {
    Synth* s = of(e);
    if (auto* t = (vst2::TimeInfo*)s->host(e, vst2::host::GET_TIME, 0, vst2::time_flag::TEMPO_VALID, nullptr, 0))
        if (t->flags & vst2::time_flag::TEMPO_VALID)
            s->tempoSeen = (float)(t->tempo / 1000.0);
    int next = 0;
    int shift = octaveShift(s->octave);
    for (int32_t i = 0; i < frames; i++) {
        while (next < s->pendingCount && s->pending[next].frame <= i)
            apply(s, s->pending[next++]);
        float sum = 0;
        for (auto& v : s->voices) {
            if (v.pitch < 0)
                continue;
            double f = 440.0 * std::pow(2.0, (v.pitch + 12 * shift - 69) / 12.0);
            sum += (float)std::sin(v.phase);
            v.phase += PI2 * f / s->rate;
            if (v.phase > PI2)
                v.phase -= PI2;
        }
        out[0][i] = out[1][i] = sum * s->volume;
    }
    while (next < s->pendingCount)
        apply(s, s->pending[next++]);
    s->pendingCount = 0;
}

void processAccumulating(vst2::Effect* e, float** in, float** out, int32_t frames) {
    static float a[8192], b[8192];
    float* tmp[2] = {a, b};
    if (frames > 8192)
        frames = 8192;
    processReplacing(e, in, tmp, frames);
    for (int32_t i = 0; i < frames; i++) {
        out[0][i] += a[i];
        out[1][i] += b[i];
    }
}

void setParameter(vst2::Effect* e, int32_t index, float value) {
    Synth* s = of(e);
    if (index == 0)
        s->volume = value;
    else if (index == 1)
        s->octave = value;
}

float getParameter(vst2::Effect* e, int32_t index) {
    Synth* s = of(e);
    return index == 0 ? s->volume : index == 1 ? s->octave : index == 2 ? s->tempoSeen : 0.0f;
}

// a click at x in the window: the volume there, told to the host as an edit
void userSetsVolume(Synth* s, int x) {
    float v = (float)(x - 10) / (float)(s->w - 20);
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    s->volume = v;
    s->host(&s->effect, vst2::host::BEGIN_EDIT, 0, 0, nullptr, 0);
    s->host(&s->effect, vst2::host::AUTOMATE, 0, 0, nullptr, v);
    s->host(&s->effect, vst2::host::END_EDIT, 0, 0, nullptr, 0);
}

#ifdef _WIN32

COLORREF rgb(unsigned c) { return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

void fill(HDC dc, int x, int y, int w, int h, unsigned color) {
    RECT r = {x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(rgb(color));
    FillRect(dc, &r, b);
    DeleteObject(b);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Synth* s = (Synth*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (s) {
                fill(dc, 0, 0, s->w, s->h, 0x202830);
                fill(dc, 10, s->h / 2, (int)((s->w - 20) * s->volume), 20, 0x40C080);
                static const unsigned swatches[] = {0xE04040, 0x4080E0, 0xE0E040, 0xA040E0, 0x40E0C0};
                for (int i = 0; i < 5; i++) fill(dc, 10 + i * 24, s->h / 2 + 30, 18, 18, swatches[i]);  // (colours to see)
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, rgb(0xFFFFFF));
                const char* title = "Tiny Test Synth (VST2, Windows): click to set the volume";
                TextOutA(dc, 10, 8, title, (int)std::strlen(title));
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (s) {
                userSetsVolume(s, (short)LOWORD(lp));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_RBUTTONDOWN:
            if (s) {
                s->w += 40;
                s->h += 20;
                SetWindowPos(hwnd, nullptr, 0, 0, s->w, s->h, SWP_NOMOVE | SWP_NOZORDER);
                s->host(&s->effect, vst2::host::SIZE_WINDOW, s->w, s->h, nullptr, 0);
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void openEditor(Synth* s, void* parent) {
    static ATOM cls = 0;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void*)&windowProc, &self);
    if (!cls) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = self;
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"TinyTestSynthWindow";
        cls = RegisterClassW(&wc);
    }
    s->window = CreateWindowExW(0, L"TinyTestSynthWindow", L"Tiny Test Synth", WS_CHILD | WS_VISIBLE, 0, 0, s->w, s->h,
                                (HWND)parent, nullptr, self, nullptr);
    if (s->window) SetWindowLongPtrW(s->window, GWLP_USERDATA, (LONG_PTR)s);
}

void editIdle(Synth*) {}  // (Windows sends the window its messages itself)

void closeEditor(Synth* s) {
    if (s->window) DestroyWindow(s->window);
    s->window = nullptr;
}

#else

void draw(Synth* s) {
    if (!s->display)
        return;
    GC gc = DefaultGC(s->display, DefaultScreen(s->display));
    XSetForeground(s->display, gc, 0x202830);
    XFillRectangle(s->display, s->window, gc, 0, 0, (unsigned)s->w, (unsigned)s->h);
    XSetForeground(s->display, gc, 0x40C080);
    XFillRectangle(s->display, s->window, gc, 10, s->h / 2, (unsigned)((s->w - 20) * s->volume), 20);
    XSetForeground(s->display, gc, 0xFFFFFF);
    const char* title = "Tiny Test Synth (VST2): click to set the volume";
    XDrawString(s->display, s->window, gc, 10, 20, title, (int)std::strlen(title));
    XFlush(s->display);
}

void editIdle(Synth* s) {
    if (!s->display)
        return;
    while (XPending(s->display)) {
        XEvent ev;
        XNextEvent(s->display, &ev);
        if (ev.type == Expose) {
            draw(s);
        } else if (ev.type == ButtonPress && ev.xbutton.button == 1) {
            userSetsVolume(s, ev.xbutton.x);
            draw(s);
        } else if (ev.type == ButtonPress && ev.xbutton.button == 3) {
            s->w += 40;
            s->h += 20;
            XResizeWindow(s->display, s->window, (unsigned)s->w, (unsigned)s->h);
            s->host(&s->effect, vst2::host::SIZE_WINDOW, s->w, s->h, nullptr, 0);
        }
    }
}

void closeEditor(Synth* s) {
    if (s->display) {
        XDestroyWindow(s->display, s->window);
        XCloseDisplay(s->display);
        s->display = nullptr;
        s->window = 0;
    }
}

void openEditor(Synth* s, void* parent) {
    s->display = XOpenDisplay(nullptr);
    if (!s->display)
        return;
    s->window = XCreateSimpleWindow(s->display, (::Window)(uintptr_t)parent, 0, 0, (unsigned)s->w, (unsigned)s->h, 0,
                                    0, 0x202830);
    XSelectInput(s->display, s->window, ExposureMask | ButtonPressMask);
    XMapWindow(s->display, s->window);
    XFlush(s->display);
}

#endif

vst2::intptr dispatcher(vst2::Effect* e, int32_t opcode, int32_t index, vst2::intptr value, void* ptr, float opt) {
    Synth* s = of(e);
    auto copy = [&](const char* text) {
        if (ptr)
            std::strcpy((char*)ptr, text);
        return (vst2::intptr)1;
    };
    static const char* names[] = {"Volume", "Octave", "Tempo seen"};
    static const char* labels[] = {"%", "oct", "bpm"};
    switch (opcode) {
        case vst2::effect::OPEN:
            return 0;
        case vst2::effect::CLOSE:
            closeEditor(s);
            delete s;
            return 1;
        case vst2::effect::GET_PARAM_NAME:
            return index >= 0 && index < 3 ? copy(names[index]) : 0;
        case vst2::effect::GET_PARAM_LABEL:
            return index >= 0 && index < 3 ? copy(labels[index]) : 0;
        case vst2::effect::GET_PARAM_DISPLAY: {
            char text[32];
            if (index == 0)
                std::snprintf(text, sizeof(text), "%.1f", s->volume * 100.0);
            else if (index == 1)
                std::snprintf(text, sizeof(text), "%+d", octaveShift(s->octave));
            else
                std::snprintf(text, sizeof(text), "%.0f", s->tempoSeen * 1000.0);
            return copy(text);
        }
        case vst2::effect::SET_SAMPLE_RATE:
            s->rate = opt > 0 ? opt : 44100;
            return 0;
        case vst2::effect::SET_BLOCK_SIZE:
        case vst2::effect::MAINS_CHANGED:
        case vst2::effect::START_PROCESS:
        case vst2::effect::STOP_PROCESS:
            return 0;
        case vst2::effect::EDIT_GET_RECT: {
            static vst2::Rect rect;
            rect = {0, 0, (int16_t)s->h, (int16_t)s->w};
            if (ptr)
                *(vst2::Rect**)ptr = &rect;
            return 1;
        }
        case vst2::effect::EDIT_OPEN:
            closeEditor(s);
            openEditor(s, ptr);
            return s->window ? 1 : 0;
        case vst2::effect::EDIT_CLOSE:
            closeEditor(s);
            return 0;
        case vst2::effect::EDIT_IDLE:
            editIdle(s);
            return 0;
        case vst2::effect::GET_CHUNK: {
            std::memcpy(s->chunk, "TTS1", 4);
            std::memcpy(s->chunk + 4, &s->volume, 4);
            std::memcpy(s->chunk + 8, &s->octave, 4);
            if (ptr)
                *(void**)ptr = s->chunk;
            return sizeof(s->chunk);
        }
        case vst2::effect::SET_CHUNK:
            if (ptr && value >= 12 && std::memcmp(ptr, "TTS1", 4) == 0) {
                std::memcpy(&s->volume, (char*)ptr + 4, 4);
                std::memcpy(&s->octave, (char*)ptr + 8, 4);
                return 1;
            }
            return 0;
        case vst2::effect::PROCESS_EVENTS: {
            auto* list = (vst2::Events*)ptr;
            for (int32_t i = 0; list && i < list->count; i++) {
                auto* ev = (vst2::MidiEvent*)list->events[i];
                if (!ev || ev->type != vst2::MIDI_EVENT || s->pendingCount >= 4096)
                    continue;
                s->pending[s->pendingCount++] = {ev->delta_frames, (unsigned char)ev->midi[0],
                                                 (unsigned char)ev->midi[1], (unsigned char)ev->midi[2]};
            }
            return 1;
        }
        case vst2::effect::GET_PLUG_CATEGORY:
            return vst2::CATEGORY_SYNTH;
        case vst2::effect::GET_EFFECT_NAME:
            return copy("Tiny Test Synth");
        case vst2::effect::GET_PRODUCT_STRING:
            return copy("Tiny Test Synth");
        case vst2::effect::GET_VENDOR_STRING:
            return copy("Tiny DAW Tests");
        case vst2::effect::CAN_DO:
            return ptr && (std::strcmp((const char*)ptr, "receiveVstMidiEvent") == 0 ||
                           std::strcmp((const char*)ptr, "receiveVstEvents") == 0)
                       ? 1
                       : 0;
        case vst2::effect::GET_VST_VERSION:
            return 2400;
        default:
            return 0;
    }
}

}  // namespace

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

EXPORT vst2::Effect* VSTPluginMain(vst2::Callback host) {
    if (!host || host(nullptr, vst2::host::VERSION, 0, 0, nullptr, 0) == 0)
        return nullptr;
    Synth* s = new Synth();
    std::memset(&s->effect, 0, sizeof(s->effect));
    vst2::Effect& e = s->effect;
    e.magic = vst2::MAGIC;
    e.dispatcher = dispatcher;
    e.process_accumulating = processAccumulating;
    e.set_parameter = setParameter;
    e.get_parameter = getParameter;
    e.programs = 1;
    e.parameters = 3;
    e.inputs = 0;
    e.outputs = 2;
    e.flags = vst2::flag::HAS_EDITOR | vst2::flag::CAN_REPLACE | vst2::flag::PROGRAM_CHUNKS | vst2::flag::IS_SYNTH;
    e.initial_delay = 16;
    e.io_ratio = 1;
    e.object = s;
    e.unique_id = 0x54547953;  // 'TTyS'
    e.version = 100;
    e.process_replacing = processReplacing;
    s->host = host;
    return &e;
}
