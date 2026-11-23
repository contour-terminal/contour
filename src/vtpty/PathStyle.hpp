// SPDX-License-Identifier: Apache-2.0
//
// The grammar a path is spelled in, as a value: so that the rules judging a path can be run for either
// platform's grammar on any platform, rather than chosen by the preprocessor.
#pragma once

#include <cstdint>

namespace vtpty
{

/// The grammar a path is spelled in: its separator, what makes it absolute, and how components compare.
enum class PathStyle : uint8_t
{
    Posix = 0, ///< `/`-separated, absolute from `/`, compared byte for byte.
    Windows,   ///< `\`-separated, absolute from `X:\` or `\\server\share`, ASCII letters folded.
};

/// The platform's own grammar.
#ifdef _WIN32
inline constexpr auto NativePathStyle = PathStyle::Windows;
#else
inline constexpr auto NativePathStyle = PathStyle::Posix;
#endif

} // namespace vtpty
