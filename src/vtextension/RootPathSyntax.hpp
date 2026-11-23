// SPDX-License-Identifier: Apache-2.0
//
// The string rules of a root's paths, for either grammar: what makes a path absolute and normalised, how it
// splits into components, and when two paths are the same or one lies beneath the other. Pure and UTF-8 in
// and out, with no filesystem, so that both grammars are judged, and tested, on every platform.
#pragma once

#include <vtpty/PathStyle.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtextension
{

/// The longest path a Path argument, a working directory or a root may have, in bytes.
inline constexpr std::size_t MaxPathArgumentBytes = 4096;

/// @param path The path judged, UTF-8.
/// @param style The grammar it is spelled in.
/// @return Whether @p path starts with the style's absolute prefix: `/` (Posix); `X:\` or `\\` (Windows).
[[nodiscard]] bool hasAbsoluteRoot(std::string_view path, vtpty::PathStyle style) noexcept;

/// @param path The path judged, UTF-8.
/// @param style The grammar it is spelled in.
/// @return Whether @p path is absolute and normalised, and at most MaxPathArgumentBytes long, without NUL.
///         Posix: `/` and then non-empty components other than `.` and `..`. Windows: `X:\` or
///         `\\server\share`, and then `\`-separated components that vtpty::isPlainWindowsName() accepts
///         (the server and the share among them), with no trailing separator except a drive root's own.
[[nodiscard]] bool isNormalisedAbsolutePath(std::string_view path, vtpty::PathStyle style) noexcept;

/// @param component One path component, UTF-8.
/// @param style The grammar it is spelled in.
/// @return Whether @p component is one name of @p style: Posix non-empty, not `.`/`..`, no `/`, no NUL;
///         Windows vtpty::isPlainWindowsName().
[[nodiscard]] bool isPlainComponent(std::string_view component, vtpty::PathStyle style) noexcept;

/// @param path The path judged, UTF-8.
/// @param style The grammar it is spelled in.
/// @return Whether @p path is a volume root: `/` (Posix); `X:\`, `\\server\share` (Windows).
[[nodiscard]] bool isVolumeRoot(std::string_view path, vtpty::PathStyle style) noexcept;

/// @param path A normalised absolute path, UTF-8.
/// @param style The grammar it is spelled in.
/// @return @p path's parent and last component, both viewing @p path; std::nullopt for a volume root, or a
///         path that is not absolute.
[[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>> parentAndLeaf(
    std::string_view path, vtpty::PathStyle style) noexcept;

/// @param directory A normalised absolute path, UTF-8.
/// @param component One plain component of @p style.
/// @param style The grammar both are spelled in.
/// @return @p component appended to @p directory with the style's separator (none added after a volume root
///         that ends in one).
[[nodiscard]] std::string childPath(std::string_view directory,
                                    std::string_view component,
                                    vtpty::PathStyle style);

/// @param a One path, UTF-8.
/// @param b The other path, UTF-8.
/// @param style The grammar both are spelled in.
/// @return Whether @p a and @p b name the same path: byte-equal (Posix); ASCII letters folded (Windows), so a
///         non-ASCII case difference does not match.
[[nodiscard]] bool samePath(std::string_view a, std::string_view b, vtpty::PathStyle style) noexcept;

/// @param path A normalised absolute path, UTF-8.
/// @param directory A normalised absolute path, UTF-8.
/// @param style The grammar both are spelled in.
/// @return Whether @p path is @p directory or lies beneath it, by whole components, by samePath()'s rule.
///         (`/wt/ab` is not beneath `/wt/a`; `D:\w` not beneath `C:\w`; `\\s\b` not beneath `\\s\a`.) Fails
///         closed: a component beneath @p directory that isPlainComponent() refuses makes it false.
[[nodiscard]] bool isAtOrBeneath(std::string_view path,
                                 std::string_view directory,
                                 vtpty::PathStyle style) noexcept;

/// @param directory A normalised absolute path, UTF-8.
/// @param path A normalised absolute path, UTF-8.
/// @param style The grammar both are spelled in.
/// @return The components of @p path strictly beneath @p directory, top-down, viewing @p path;
///         std::nullopt when it is not strictly beneath it, or a component isPlainComponent() refuses.
[[nodiscard]] std::optional<std::vector<std::string_view>> componentsBeneath(std::string_view directory,
                                                                             std::string_view path,
                                                                             vtpty::PathStyle style);

/// @param path A path, UTF-8.
/// @param style The grammar it is spelled in.
/// @return A key equal for exactly the paths samePath() calls equal (Posix: @p path; Windows: ASCII letters
///         lowered).
[[nodiscard]] std::string comparisonKeyOf(std::string_view path, vtpty::PathStyle style);

} // namespace vtextension
