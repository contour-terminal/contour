// SPDX-License-Identifier: Apache-2.0
//
// How many files the process may hold open through the C runtime.
//
// On Windows the C runtime caps its own streams at 512 unless told otherwise, far below what the operating
// system allows. FreeType opens every font face it loads through that runtime and holds the file for the
// face's lifetime, and a font shaper can hold well over a hundred fallback faces of C:\Windows\Fonts. A few
// shapers alive at once -- a few tabs -- used the whole budget, after which every other open failed:
// configuration loads, settings saves, log files.
//
// POSIX has no such runtime cap; its limit is the process's RLIMIT_NOFILE, which every child inherits. A
// terminal raising it would raise it for every shell it starts, so it is deliberately left alone there.

#pragma once

#include <expected>
#include <format>
#include <string>

#if defined(_WIN32)
    #include <cstdio>
#endif

namespace contour::platform
{

/// The most files the process asks to hold open through the C runtime: the largest the Windows runtime
/// accepts.
inline constexpr int OpenFileLimit = 8192;

/// Raises the C runtime's open-file limit to OpenFileLimit, where the runtime imposes one of its own.
/// Called once, first thing in main().
/// @return Nothing, or why the runtime refused; a refusal leaves the previous limit in place.
[[nodiscard]] inline std::expected<void, std::string> raiseOpenFileLimit()
{
#if defined(_WIN32)
    if (_setmaxstdio(OpenFileLimit) == -1)
        return std::unexpected(
            std::format("The C runtime kept its limit of {} open files; {} were asked for.",
                        _getmaxstdio(),
                        OpenFileLimit));
#endif
    return {};
}

} // namespace contour::platform
