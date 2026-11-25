// How much memory this program holds in RAM (its resident set): /proc/self/statm on Linux,
// GetProcessMemoryInfo on Windows.
#pragma once
#include <cstdint>

namespace process_memory {

// the bytes of RAM this process uses now; 0 when the system doesn't say
uint64_t residentBytes();

}  // namespace process_memory
