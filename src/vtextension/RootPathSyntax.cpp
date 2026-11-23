// SPDX-License-Identifier: Apache-2.0
#include <vtextension/RootPathSyntax.hpp>

#include <vtpty/WindowsPathRules.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <ranges>
#include <utility>

namespace vtextension
{

namespace
{
    /// Folds an ASCII upper-case letter to lower case; every other byte, the UTF-8 ones included, as it is.
    [[nodiscard]] constexpr char asciiLower(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    [[nodiscard]] constexpr char unchanged(char c) noexcept
    {
        return c;
    }

    [[nodiscard]] constexpr bool isAsciiLetter(char c) noexcept
    {
        return asciiLower(c) >= 'a' && asciiLower(c) <= 'z';
    }

    [[nodiscard]] bool isPlainPosixName(std::string_view name) noexcept
    {
        return !name.empty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\0');
    }

    /// Whether @p path starts with `/`.
    [[nodiscard]] bool hasPosixRoot(std::string_view path) noexcept
    {
        return path.starts_with('/');
    }

    /// Whether @p path starts with `X:\` or `\\`.
    [[nodiscard]] bool hasWindowsRoot(std::string_view path) noexcept
    {
        return (path.size() >= 3 && isAsciiLetter(path[0]) && path[1] == ':' && path[2] == '\\')
               || path.starts_with(R"(\\)");
    }

    /// @return The length of @p path's volume root `/`.
    [[nodiscard]] std::optional<std::size_t> posixVolumeRootLength(std::string_view path) noexcept
    {
        return hasPosixRoot(path) ? std::optional<std::size_t> { 1 } : std::nullopt;
    }

    /// @return The length of @p path's volume root: `X:\`, or `\\server\share` with both a plain name.
    [[nodiscard]] std::optional<std::size_t> windowsVolumeRootLength(std::string_view path) noexcept
    {
        constexpr auto UncPrefix = std::string_view { R"(\\)" };
        if (!path.starts_with(UncPrefix))
            return hasWindowsRoot(path) ? std::optional<std::size_t> { 3 } : std::nullopt;
        auto const serverEnd = path.find('\\', UncPrefix.size());
        if (serverEnd == std::string_view::npos)
            return std::nullopt;
        auto const shareEnd = std::min(path.find('\\', serverEnd + 1), path.size());
        auto const server = path.substr(UncPrefix.size(), serverEnd - UncPrefix.size());
        auto const share = path.substr(serverEnd + 1, shareEnd - serverEnd - 1);
        if (!vtpty::isPlainWindowsName(server) || !vtpty::isPlainWindowsName(share))
            return std::nullopt;
        return shareEnd;
    }

    /// What tells one path grammar from the other: one row per vtpty::PathStyle, in enumerator order.
    struct Grammar
    {
        char separator;                             ///< Between two components.
        bool (*hasRoot)(std::string_view) noexcept; ///< The absolute prefix.
        std::optional<std::size_t> (*volumeRootLength)(std::string_view) noexcept; ///< Its volume root.
        bool (*isPlain)(std::string_view) noexcept;                                ///< One component.
        char (*fold)(char) noexcept; ///< What comparison ignores.
    };

    constexpr auto Grammars = std::array {
        Grammar { .separator = '/',
                  .hasRoot = hasPosixRoot,
                  .volumeRootLength = posixVolumeRootLength,
                  .isPlain = isPlainPosixName,
                  .fold = unchanged },
        Grammar { .separator = '\\',
                  .hasRoot = hasWindowsRoot,
                  .volumeRootLength = windowsVolumeRootLength,
                  .isPlain = vtpty::isPlainWindowsName,
                  .fold = asciiLower },
    };

    [[nodiscard]] constexpr Grammar const& grammarOf(vtpty::PathStyle style) noexcept
    {
        return Grammars.at(std::to_underlying(style));
    }

    /// @return The part of @p path after its volume root and the separator that follows it, which is
    ///         empty for a volume root; std::nullopt when @p path has no volume root, or ends in a
    ///         separator after one that does not end in one itself (`\\s\sh\`).
    [[nodiscard]] std::optional<std::string_view> componentsPart(std::string_view path,
                                                                 Grammar const& grammar) noexcept
    {
        auto const rootLength = grammar.volumeRootLength(path);
        if (!rootLength)
            return std::nullopt;
        if (*rootLength == path.size() || path[*rootLength - 1] == grammar.separator)
            return path.substr(*rootLength);
        if (path[*rootLength] != grammar.separator || *rootLength + 1 == path.size())
            return std::nullopt;
        return path.substr(*rootLength + 1);
    }

    /// @return Whether @p part, separated by the grammar's separator, is a run of plain components; an empty
    ///         @p part is none.
    [[nodiscard]] bool arePlainComponents(std::string_view part, Grammar const& grammar) noexcept
    {
        return part.empty()
               || std::ranges::all_of(std::views::split(part, grammar.separator), [&](auto const& c) {
                      return grammar.isPlain(std::string_view(c.begin(), c.end()));
                  });
    }

    /// @return The part of @p path strictly beneath @p directory, without the separator between them;
    ///         std::nullopt when it is not strictly beneath it by whole components.
    [[nodiscard]] std::optional<std::string_view> partBeneath(std::string_view directory,
                                                              std::string_view path,
                                                              vtpty::PathStyle style) noexcept
    {
        auto const& grammar = grammarOf(style);
        if (!grammar.volumeRootLength(directory) || path.size() <= directory.size()
            || !samePath(path.substr(0, directory.size()), directory, style))
            return std::nullopt;
        if (directory.ends_with(grammar.separator))
            return path.substr(directory.size());
        if (path[directory.size()] != grammar.separator)
            return std::nullopt;
        return path.substr(directory.size() + 1);
    }
} // namespace

bool hasAbsoluteRoot(std::string_view path, vtpty::PathStyle style) noexcept
{
    return grammarOf(style).hasRoot(path);
}

bool isNormalisedAbsolutePath(std::string_view path, vtpty::PathStyle style) noexcept
{
    if (path.size() > MaxPathArgumentBytes || path.contains('\0'))
        return false;
    auto const& grammar = grammarOf(style);
    auto const part = componentsPart(path, grammar);
    return part && arePlainComponents(*part, grammar);
}

bool isPlainComponent(std::string_view component, vtpty::PathStyle style) noexcept
{
    return grammarOf(style).isPlain(component);
}

bool isVolumeRoot(std::string_view path, vtpty::PathStyle style) noexcept
{
    return grammarOf(style).volumeRootLength(path) == path.size();
}

std::optional<std::pair<std::string_view, std::string_view>> parentAndLeaf(std::string_view path,
                                                                           vtpty::PathStyle style) noexcept
{
    auto const& grammar = grammarOf(style);
    auto const rootLength = grammar.volumeRootLength(path);
    if (!rootLength || *rootLength >= path.size())
        return std::nullopt;
    auto const lastSeparator = path.rfind(grammar.separator);
    return std::pair { path.substr(0, std::max(lastSeparator, *rootLength)), path.substr(lastSeparator + 1) };
}

std::string childPath(std::string_view directory, std::string_view component, vtpty::PathStyle style)
{
    auto const separator = grammarOf(style).separator;
    auto result = std::string(directory);
    if (!(isVolumeRoot(directory, style) && directory.ends_with(separator)))
        result += separator;
    result += component;
    return result;
}

bool samePath(std::string_view a, std::string_view b, vtpty::PathStyle style) noexcept
{
    auto const fold = grammarOf(style).fold;
    return std::ranges::equal(a, b, std::ranges::equal_to {}, fold, fold);
}

bool isAtOrBeneath(std::string_view path, std::string_view directory, vtpty::PathStyle style) noexcept
{
    if (!grammarOf(style).volumeRootLength(directory))
        return false;
    if (samePath(path, directory, style))
        return true;
    auto const part = partBeneath(directory, path, style);
    return part && !part->empty() && arePlainComponents(*part, grammarOf(style));
}

std::optional<std::vector<std::string_view>> componentsBeneath(std::string_view directory,
                                                               std::string_view path,
                                                               vtpty::PathStyle style)
{
    auto const& grammar = grammarOf(style);
    auto const part = partBeneath(directory, path, style);
    if (!part || part->empty() || !arePlainComponents(*part, grammar))
        return std::nullopt;
    return std::views::split(*part, grammar.separator)
           | std::views::transform([](auto const& c) { return std::string_view(c.begin(), c.end()); })
           | std::ranges::to<std::vector>();
}

std::string comparisonKeyOf(std::string_view path, vtpty::PathStyle style)
{
    return path | std::views::transform(grammarOf(style).fold) | std::ranges::to<std::string>();
}

} // namespace vtextension
