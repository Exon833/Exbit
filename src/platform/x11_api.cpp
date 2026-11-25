// x11_api.h's pointers, and loading them
#include "platform/x11_api.h"

#include <dlfcn.h>

namespace xlib {
#define EXBIT_X11_DEFINE(lib, f) decltype(&::f) f = nullptr;
EXBIT_X11(EXBIT_X11_DEFINE)
#undef EXBIT_X11_DEFINE

bool load(std::string& why) {
    static std::string error = [] {
        struct Lib {
            const char* name;
            const char* file;
            void* handle;
        } libs[] = {{"X11", "libX11.so.6", nullptr}, {"Xext", "libXext.so.6", nullptr},
                    {"Xrandr", "libXrandr.so.2", nullptr}, {"Xcursor", "libXcursor.so.1", nullptr}};
        for (Lib& l : libs)
            if (!(l.handle = dlopen(l.file, RTLD_NOW | RTLD_GLOBAL))) return std::string(l.file) + " isn't installed";
        auto handle = [&](const char* name) {
            for (Lib& l : libs)
                if (std::string(l.name) == name) return l.handle;
            return (void*)nullptr;
        };
        std::string missing;
#define EXBIT_X11_FIND(lib, f) \
    if (!(f = reinterpret_cast<decltype(f)>(dlsym(handle(#lib), #f)))) missing += std::string(missing.empty() ? "" : ", ") + #f;
        EXBIT_X11(EXBIT_X11_FIND)
#undef EXBIT_X11_FIND
        return missing.empty() ? std::string() : "X11's libraries lack " + missing;
    }();
    why = error;
    return error.empty();
}
}  // namespace xlib
