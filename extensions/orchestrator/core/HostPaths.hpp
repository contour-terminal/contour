// SPDX-License-Identifier: Apache-2.0
//
// The host's paths as the host spells them. The guest is wasm32 on every host, so no platform macro says how
// a path is spelled: the style is read from the spelling of a path the host resolved (the worktree root), and
// every path the guest builds or compares beneath it follows that style. The host remains the authority: it
// judges every path again before it acts on one.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace orchestrator
{

/// How the host spells its paths. The guest is wasm32 everywhere: it reads this from the spelling.
enum class PathStyle : uint8_t
{
    Posix = 0, ///< `/` and then `/`-separated components, compared byte for byte.
    Windows,   ///< `X:\` or `\\server\share` and then `\`-separated components, ASCII letters folded.
};

/// @param absolutePath A path as the host spells it.
/// @return Posix for `/…`, Windows for `X:\…` or `\\…`; std::nullopt for anything else.
[[nodiscard]] constexpr std::optional<PathStyle> pathStyleOf(std::string_view absolutePath) noexcept
{
    auto const isAsciiLetter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    if (absolutePath.starts_with('/'))
        return PathStyle::Posix;
    if (absolutePath.starts_with(R"(\\)")
        || (absolutePath.size() >= 3 && isAsciiLetter(absolutePath[0]) && absolutePath[1] == ':'
            && absolutePath[2] == '\\'))
        return PathStyle::Windows;
    return std::nullopt;
}

/// @param style A path style.
/// @return The separator the host puts between two components: `/` (Posix) or `\` (Windows).
[[nodiscard]] constexpr char separatorOf(PathStyle style) noexcept
{
    return style == PathStyle::Windows ? '\\' : '/';
}

/// @param directory A normalised absolute path of @p style.
/// @param component One component.
/// @param style The style @p directory is spelled in.
/// @return @p component appended to @p directory with the style's separator (none added after a Windows drive
///         root, which ends in one).
[[nodiscard]] std::string joinPath(std::string_view directory, std::string_view component, PathStyle style);

/// @param path A normalised absolute path of @p style.
/// @param style The style it is spelled in.
/// @return The last component of @p path.
[[nodiscard]] std::string_view lastComponent(std::string_view path, PathStyle style) noexcept;

/// @param path A path, as the host or git spells it.
/// @param root A normalised absolute path of @p style.
/// @param style The style @p root is spelled in.
/// @return Whether @p path lies strictly inside @p root, by whole components: byte for byte on Posix; ASCII
///         letters folded and with `/` and `\` equal on Windows (git spells `C:/…`).
[[nodiscard]] bool isStrictlyInside(std::string_view path, std::string_view root, PathStyle style) noexcept;

/// @param a One path.
/// @param b The other path.
/// @param style The style both are spelled in.
/// @return Whether @p a and @p b name one path, by isStrictlyInside()'s rule.
[[nodiscard]] bool sameHostPath(std::string_view a, std::string_view b, PathStyle style) noexcept;

/// @param text A text that may start with a path, such as git's output.
/// @param prefix A path.
/// @param style The style @p prefix is spelled in.
/// @return What follows @p prefix in @p text when @p text starts with it by sameHostPath()'s rule;
///         std::nullopt when it does not.
[[nodiscard]] std::optional<std::string_view> afterHostPath(std::string_view text,
                                                            std::string_view prefix,
                                                            PathStyle style) noexcept;

/// @param root A normalised absolute path of @p style.
/// @param path A path.
/// @param style The style both are spelled in.
/// @return @p path beneath @p root as a `/`-separated relative path, what `file-read` takes; std::nullopt
///         when @p path is not strictly inside @p root.
[[nodiscard]] std::optional<std::string> slashRelative(std::string_view root,
                                                       std::string_view path,
                                                       PathStyle style);

} // namespace orchestrator
