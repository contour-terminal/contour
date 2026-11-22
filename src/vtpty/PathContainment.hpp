// SPDX-License-Identifier: Apache-2.0
//
// Whether one path lies at or beneath another: the one containment rule every host-side check shares, so
// that a directory a root admits and a directory a transcript binds to are judged alike.
#pragma once

#include <cstdint>
#include <filesystem>

namespace vtpty
{

/// How two spellings of a path component are compared.
enum class PathCase : uint8_t
{
    Sensitive = 0, ///< Byte for byte, as on Linux and the other POSIX systems.
    Insensitive,   ///< ASCII letters fold, as Windows does by default; a non-ASCII case difference does not
                   ///< match, so a path is refused rather than admitted by a folding the platform may not
                   ///< share.
};

/// How the platform compares path components: insensitively on Windows, sensitively elsewhere.
#ifdef _WIN32
inline constexpr auto NativePathCase = PathCase::Insensitive;
#else
inline constexpr auto NativePathCase = PathCase::Sensitive;
#endif

/// @param path The path judged.
/// @param directory The directory it must lie in.
/// @param rule How components are compared; NativePathCase for the platform's own rule.
/// @return Whether @p path is @p directory or lies beneath it, compared by whole path component after
///         lexical normalisation (so `/wt/ab` is not beneath `/wt/a`, nor `C:\wt\a` beneath `D:\wt\a`, nor
///         one share beneath another). Both must be absolute, which on Windows means drive-rooted or UNC.
[[nodiscard]] bool isAtOrBeneath(std::filesystem::path const& path,
                                 std::filesystem::path const& directory,
                                 PathCase rule);

} // namespace vtpty
