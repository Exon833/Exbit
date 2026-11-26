// The stand-ins' exports (see wine_own_dll.h): each is a jump through its place in
// exbitRealFunctions, filled with the address of Wine's own export when the DLL loads, so a call
// reaches Wine's with its arguments and stack as they were (any calling convention, any arguments).
// A stand-in sets its own function in an export's place to correct it.
//
//   #define EXPORT(index, ordinal, name) EXPORT_THUNK(index)
//   #include "winefix/msi_exports.h"
//
// makes thunk0, thunk1…; the .def file (Makefile, from the same list) names them as the exports.
#pragma once

extern void* exbitRealFunctions[];

#if defined(__x86_64__)
#define EXPORT_THUNK(index) \
    __asm__(".text\n.globl thunk" #index "\nthunk" #index ":\n\tjmp *(exbitRealFunctions+8*" #index ")(%rip)\n");
#else
#define EXPORT_THUNK(index) \
    __asm__(".text\n.globl _thunk" #index "\n_thunk" #index ":\n\tjmp *(_exbitRealFunctions+4*" #index ")\n");
#endif
