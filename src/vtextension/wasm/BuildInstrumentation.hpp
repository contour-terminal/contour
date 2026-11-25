// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How this build is instrumented, as the one switch CMake sets says (CONTOUR_BUILD_INSTRUMENTATION, a public
/// definition of vtextension_remote_wire): what the extension host caps, where its stderr goes, and how its
/// engine traps all follow from it.

#include <cstdint>

#ifndef CONTOUR_BUILD_INSTRUMENTATION
    #error "CONTOUR_BUILD_INSTRUMENTATION is set by linking vtextension_remote_wire."
#endif

namespace vtextension::wasm
{

/// How a build is instrumented. Each kind owns more of the process than the one before it.
enum class BuildInstrumentation : uint8_t
{
    None,       ///< Not at all.
    Coverage,   ///< For coverage: its counters are written out, and its reports go to stderr.
    Sanitizers, ///< By a sanitizer, whose shadow memory and threads exceed any cap, and which owns the
                ///< signal handlers.
};

/// How this build is instrumented.
inline constexpr auto ThisBuildInstrumentation = BuildInstrumentation { CONTOUR_BUILD_INSTRUMENTATION };

} // namespace vtextension::wasm
