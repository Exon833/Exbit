// How much memory this program holds in RAM (core/process_memory.h)
#include "core/process_memory.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace process_memory {

uint64_t residentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof counters;
    // (kernel32's own name for it, so no psapi.dll is needed)
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) return 0;
    return (uint64_t)counters.WorkingSetSize;
#else
    // statm: the program's size, then its resident pages
    FILE* statm = fopen("/proc/self/statm", "re");
    if (!statm) return 0;
    unsigned long long totalPages = 0, residentPages = 0;
    int read = fscanf(statm, "%llu %llu", &totalPages, &residentPages);
    fclose(statm);
    long pageSize = sysconf(_SC_PAGESIZE);
    return read == 2 && pageSize > 0 ? (uint64_t)residentPages * (uint64_t)pageSize : 0;
#endif
}

}  // namespace process_memory
