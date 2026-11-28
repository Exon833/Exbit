// Denormal numbers off on an audio thread: the SSE control register's FTZ (results flushed to zero, 0x8000) and
// DAZ (inputs taken as zero, 0x0040) bits. A filter or reverb fading into silence otherwise makes numbers so small
// that each operation on them costs a hundred times more, and a quiet song's CPU use explodes. The register is the
// thread's own, so each audio thread sets it as it starts (Exbit's sound device and worker threads, the plugin
// hosts' audio threads). Under Wine it is set in the host's Windows code: Wine keeps its Unix side's register apart.
// Header-only: the plugin hosts (src/winehost) and the benchmark's reference program use it too.
#pragma once
#include <cstdint>

namespace audio {

constexpr uint32_t FLUSH_TO_ZERO_BIT = 0x8000;
constexpr uint32_t DENORMALS_ARE_ZERO_BIT = 0x0040;

// the calling thread's SSE control register (MXCSR); 0 where there is none (not x86)
inline uint32_t readFloatingPointControl() {
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    uint32_t control = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(control));
    return control;
#else
    return 0;
#endif
}

// turns FTZ and DAZ on for the calling thread; false where the processor has no such bits (compiled out off x86)
inline bool flushDenormalsOnThisThread() {
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    uint32_t control = readFloatingPointControl() | FLUSH_TO_ZERO_BIT | DENORMALS_ARE_ZERO_BIT;
    __asm__ volatile("ldmxcsr %0" : : "m"(control));
    return true;
#else
    return false;  // shortcut: ARM's FPCR.FZ (bit 24) would do the same, when Exbit is built for ARM
#endif
}

}  // namespace audio
