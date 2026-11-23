// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/HostPaths.hpp>

#include <algorithm>
#include <functional>

namespace orchestrator
{

namespace
{
    /// What a Windows comparison ignores: ASCII letter case, and which of `/` and `\` separates. A
    /// non-ASCII byte is kept, so a non-ASCII case difference does not match.
    [[nodiscard]] constexpr char windowsFolded(char c) noexcept
    {
        if (c == '/')
            return '\\';
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    [[nodiscard]] constexpr bool isSeparator(char c, PathStyle style) noexcept
    {
        return c == separatorOf(style) || (style == PathStyle::Windows && c == '/');
    }

    /// @return Whether @p path ends in its style's separator, as only a Windows drive root (`C:\`) does.
    [[nodiscard]] bool endsInSeparator(std::string_view path, PathStyle style) noexcept
    {
        return style == PathStyle::Windows && !path.empty() && isSeparator(path.back(), style);
    }

    /// @return The part of @p path strictly inside @p root, without the separator between them; std::nullopt
    ///         when it is not strictly inside it.
    [[nodiscard]] std::optional<std::string_view> partInside(std::string_view path,
                                                             std::string_view root,
                                                             PathStyle style) noexcept
    {
        if (path.size() <= root.size() || !sameHostPath(path.substr(0, root.size()), root, style))
            return std::nullopt;
        if (endsInSeparator(root, style))
            return path.substr(root.size());
        if (!isSeparator(path[root.size()], style))
            return std::nullopt;
        return path.substr(root.size() + 1);
    }
} // namespace

std::string joinPath(std::string_view directory, std::string_view component, PathStyle style)
{
    auto result = std::string(directory);
    if (!endsInSeparator(directory, style))
        result += separatorOf(style);
    result += component;
    return result;
}

std::string_view lastComponent(std::string_view path, PathStyle style) noexcept
{
    return path.substr(path.rfind(separatorOf(style)) + 1);
}

bool isStrictlyInside(std::string_view path, std::string_view root, PathStyle style) noexcept
{
    return partInside(path, root, style).has_value();
}

bool sameHostPath(std::string_view a, std::string_view b, PathStyle style) noexcept
{
    if (style == PathStyle::Posix)
        return a == b;
    return std::ranges::equal(a, b, std::ranges::equal_to {}, windowsFolded, windowsFolded);
}

std::optional<std::string_view> afterHostPath(std::string_view text,
                                              std::string_view prefix,
                                              PathStyle style) noexcept
{
    if (text.size() < prefix.size() || !sameHostPath(text.substr(0, prefix.size()), prefix, style))
        return std::nullopt;
    return text.substr(prefix.size());
}

std::optional<std::string> slashRelative(std::string_view root, std::string_view path, PathStyle style)
{
    return partInside(path, root, style).transform([style](std::string_view part) {
        auto relative = std::string(part);
        std::ranges::replace(relative, separatorOf(style), '/');
        return relative;
    });
}

} // namespace orchestrator
