// alsa_api.h's pointers, and loading them
#include "audio/alsa_api.h"

#include <dlfcn.h>

#include "core/util.h"

namespace alsa {
#define TINYDAW_ALSA_DEFINE(f) decltype(&::f) f = nullptr;
TINYDAW_ALSA(TINYDAW_ALSA_DEFINE)
#undef TINYDAW_ALSA_DEFINE

bool load(std::string& why) {
    static const std::string error = [] {
        void* lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_GLOBAL);
        if (!lib) return std::string("ALSA's library (libasound.so.2) isn't installed");
        std::string missing;
#define TINYDAW_ALSA_FIND(f) \
    if (!(f = reinterpret_cast<decltype(f)>(dlsym(lib, #f)))) missing += std::string(missing.empty() ? "" : ", ") + #f;
        TINYDAW_ALSA(TINYDAW_ALSA_FIND)
#undef TINYDAW_ALSA_FIND
        return missing.empty() ? std::string() : "ALSA's library lacks " + missing;
    }();
    static const bool told = [] {
        util::debugf("audio: ALSA %s\n", error.empty() ? "loaded" : ("unavailable: " + error).c_str());
        return true;
    }();
    (void)told;
    why = error;
    return error.empty();
}
}  // namespace alsa
