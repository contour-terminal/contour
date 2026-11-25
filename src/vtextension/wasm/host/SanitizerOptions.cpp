// SPDX-License-Identifier: Apache-2.0
//
// What a sanitizer build's sandboxed process defaults its sanitizer to, unless the environment it was started
// with says otherwise: no leak check at exit. LeakSanitizer stops the world through a cloned tracer (clone,
// ptrace, wait4), which the extension host's system call filter kills, so every exit would end in SIGSYS.
// Leaks are the in-process HostServer tests' to find. The runtime asks for these at start; the environment's
// ASAN_OPTIONS/LSAN_OPTIONS still override them.
//
// Linked as objects into each executable that locks itself down, as a static library member defining them
// would never be pulled in over the runtime's own weak defaults. Compiled into Linux builds only: it serves
// the Linux filter, and its assembler labels are ELF names (Mach-O's would carry a leading underscore).
#include <vtextension/wasm/BuildInstrumentation.hpp>

#if CONTOUR_BUILD_INSTRUMENTATION == 2
// The runtime looks them up by their C names, which an assembler label gives these, so that the C++ names
// follow the project's.
extern "C"
{
    /// @return AddressSanitizer's default options for this process.
    [[gnu::used]] char const* asanDefaultOptions() __asm__("__asan_default_options");
    /// @return LeakSanitizer's default options for this process.
    [[gnu::used]] char const* lsanDefaultOptions() __asm__("__lsan_default_options");
}

char const* asanDefaultOptions()
{
    return "detect_leaks=0";
}

char const* lsanDefaultOptions()
{
    return "detect_leaks=0";
}
#endif
