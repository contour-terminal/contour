// SPDX-License-Identifier: Apache-2.0
#include <vtextension/RootPointer.hpp>

#include <vtpty/PathStyle.hpp>
#include <vtpty/WindowsPathRules.hpp>

#include <core/Sha256.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <ranges>
#include <utility>

#include <glaze/json.hpp>
#include <vtextension/RootPathSyntax.hpp>
#include <vtextension/RootResolution.hpp>

namespace vtextension
{

namespace
{
    using Json = glz::generic;

    /// @return The nodes @p component leads to from each of @p nodes; or why it leads nowhere.
    [[nodiscard]] std::expected<std::vector<Json const*>, RootPointerError> step(
        std::span<Json const* const> nodes, std::string_view component)
    {
        auto next = std::vector<Json const*> {};
        for (auto const* node: nodes)
        {
            if (component == "*")
            {
                auto const* list = node->get_if<Json::array_t>();
                if (list == nullptr)
                    return std::unexpected(RootPointerError::StarOverNonList);
                for (auto const& element: *list)
                    next.push_back(&element);
                continue;
            }
            auto const* object = node->get_if<Json::object_t>();
            if (object == nullptr)
                return std::unexpected(RootPointerError::MissingKey);
            auto const member = object->find(component);
            if (member == object->end())
                return std::unexpected(RootPointerError::MissingKey);
            next.push_back(&member->second);
        }
        return next;
    }

    /// The prefixes of a Win32 device or verbatim path, once its `/` are `\`.
    constexpr auto DevicePrefixes = std::to_array<std::string_view>({ R"(\\?\)", R"(\\.\)", R"(\??\)" });

    /// The prefix of a UNC path.
    constexpr auto UncPrefix = std::string_view { R"(\\)" };

    /// @return Whether @p c is an ASCII letter: the drive letters Win32 knows.
    [[nodiscard]] constexpr bool isAsciiLetter(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    /// @return @p c, upper-cased if it is an ASCII lower-case letter.
    [[nodiscard]] constexpr char asciiUpper(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    }

    /// @return @p text without the trailing @p separator characters.
    [[nodiscard]] constexpr std::string_view withoutTrailing(std::string_view text,
                                                             std::string_view separators) noexcept
    {
        auto const last = text.find_last_not_of(separators);
        return last == std::string_view::npos ? std::string_view {} : text.substr(0, last + 1);
    }

    /// @return Whether every `\`-separated component of @p components is a plain Windows name.
    [[nodiscard]] bool arePlainWindowsNames(std::string_view components)
    {
        return std::ranges::all_of(components | std::views::split('\\'), [](auto const& component) {
            return vtpty::isPlainWindowsName(std::string_view { component });
        });
    }

    /// rootSpellingOf() for PathStyle::Posix: today's expansion, nothing normalised.
    [[nodiscard]] std::expected<std::string, RootPointerError> posixSpellingOf(std::string_view raw,
                                                                               std::string_view home)
    {
        // Without a home, `~/x` would become `/x`: a directory nobody configured.
        if ((raw == "~" || raw.starts_with("~/")) && home.empty())
            return std::unexpected(RootPointerError::NoHome);
        auto expanded = std::string(raw);
        if (raw == "~")
            expanded = std::string(home);
        else if (raw.starts_with("~/"))
            expanded = std::format("{}/{}", home, raw.substr(2));
        if (!expanded.starts_with('/'))
            return std::unexpected(RootPointerError::NotAbsolute);
        return expanded;
    }

    /// @return @p raw with `~`, `~\…` and `~/…` expanded to @p home; anything else as it is.
    [[nodiscard]] std::expected<std::string, RootPointerError> windowsExpansionOf(std::string_view raw,
                                                                                  std::string_view home)
    {
        auto const homeRelative = raw.starts_with("~\\") || raw.starts_with("~/");
        // Without a home, `~\x` would become `\x`: a directory nobody configured.
        if ((raw == "~" || homeRelative) && home.empty())
            return std::unexpected(RootPointerError::NoHome);
        if (raw == "~")
            return std::string(home);
        if (homeRelative)
            return std::format("{}\\{}", withoutTrailing(home, "\\/"), raw.substr(2));
        return std::string(raw);
    }

    /// @return @p spelled, if the comparison rule takes it for a normalised absolute Windows path;
    ///         `NotAbsolute` otherwise. The backstop of windowsSpellingOf(): its grammar cannot drift from
    ///         the rule every root is later judged by (RootPathSyntax), and a spelling longer than
    ///         MaxPathArgumentBytes, which no path could be judged beneath, is refused here rather than
    ///         admitted as an unusable root.
    [[nodiscard]] std::expected<std::string, RootPointerError> comparableWindowsSpelling(std::string spelled)
    {
        if (!isNormalisedAbsolutePath(spelled, vtpty::PathStyle::Windows))
            return std::unexpected(RootPointerError::NotAbsolute);
        return spelled;
    }

    /// rootSpellingOf() for PathStyle::Windows: `X:\…` or `\\server\share…`, `\`-separated, every component
    /// plain, the drive letter upper-cased, no trailing separator but a drive root's own.
    [[nodiscard]] std::expected<std::string, RootPointerError> windowsSpellingOf(std::string_view raw,
                                                                                 std::string_view home)
    {
        auto expanded = windowsExpansionOf(raw, home);
        if (!expanded)
            return expanded;
        auto& spelled = *expanded;
        std::ranges::replace(spelled, '/', '\\');
        if (std::ranges::any_of(DevicePrefixes,
                                [&spelled](std::string_view prefix) { return spelled.starts_with(prefix); }))
            return std::unexpected(RootPointerError::DevicePath);

        if (spelled.starts_with(UncPrefix))
        {
            auto const server = std::string_view { spelled }.substr(UncPrefix.size());
            auto const serverEnd = server.find('\\');
            auto const share =
                serverEnd == std::string_view::npos ? std::string_view {} : server.substr(serverEnd + 1);
            auto const shareName = share.substr(0, share.find('\\'));
            if (serverEnd == 0 || shareName.empty())
                return std::unexpected(RootPointerError::NotAbsolute);
            auto const path = withoutTrailing(spelled, "\\");
            if (!arePlainWindowsNames(path.substr(UncPrefix.size())))
                return std::unexpected(RootPointerError::NotPlainName);
            return comparableWindowsSpelling(std::string(path));
        }

        if (spelled.size() < 3 || !isAsciiLetter(spelled[0]) || spelled[1] != ':' || spelled[2] != '\\')
            return std::unexpected(RootPointerError::NotAbsolute);
        auto const below = withoutTrailing(std::string_view { spelled }.substr(3), "\\");
        if (!below.empty() && !arePlainWindowsNames(below))
            return std::unexpected(RootPointerError::NotPlainName);
        return comparableWindowsSpelling(std::format("{}:\\{}", asciiUpper(spelled[0]), below));
    }
} // namespace

std::expected<std::vector<std::string>, RootPointerError> rootValuesOf(std::string_view configJson,
                                                                       std::string_view pointer)
{
    auto const components = std::ranges::to<std::vector<std::string_view>>(
        pointer | std::views::split('/')
        | std::views::transform([](auto const& part) { return std::string_view { part }; }));
    if (pointer.empty() || std::ranges::contains(components, std::string_view {}))
        return std::unexpected(RootPointerError::BadPointer);

    auto document = Json {};
    if (glz::read_json(document, configJson))
        return std::unexpected(RootPointerError::NotJson);

    auto nodes = std::vector<Json const*> { &document };
    for (auto const component: components)
    {
        auto next = step(nodes, component);
        if (!next)
            return std::unexpected(next.error());
        nodes = std::move(*next);
    }

    auto values = std::vector<std::string> {};
    values.reserve(nodes.size());
    for (auto const* node: nodes)
    {
        auto const* text = node->get_if<std::string>();
        if (text == nullptr)
            return std::unexpected(RootPointerError::NotAString);
        values.push_back(*text);
    }
    return values;
}

std::expected<std::string, RootPointerError> rootSpellingOf(std::string_view raw,
                                                            std::string_view home,
                                                            vtpty::PathStyle style)
{
    switch (style)
    {
        case vtpty::PathStyle::Posix: return posixSpellingOf(raw, home);
        case vtpty::PathStyle::Windows: return windowsSpellingOf(raw, home);
    }
    std::unreachable();
}

std::expected<std::string, RootPointerError> resolveRootPath(std::string_view raw,
                                                             std::string_view home,
                                                             core::platform::FileSystem const& fileSystem,
                                                             vtpty::PathStyle style)
{
    auto const spelling = rootSpellingOf(raw, home, style);
    if (!spelling)
        return std::unexpected(spelling.error());
    auto const path = core::pathFromUtf8(*spelling);
    if (!path)
        return std::unexpected(RootPointerError::NotAbsolute);
    auto resolved = core::pathToUtf8(fileSystem.weaklyCanonical(*path));
    if (style == vtpty::PathStyle::Windows)
    {
        // The canonical form comes back verbatim (`\\?\C:\…`) and is spelled once more, so that a root is
        // only ever a path the grammar admits: one that is not (`\\?\Volume{…}\`) is refused outright.
        return rootSpellingOf(vtpty::dosPathOf(resolved), "", style).transform_error([](RootPointerError) {
            return RootPointerError::NotAbsolute;
        });
    }
    // Without a trailing `/`, which RootResolution reads as an empty component, and which would keep two
    // spellings of one directory apart.
    while (resolved.size() > 1 && resolved.ends_with('/'))
        resolved.pop_back();
    return resolved;
}

std::expected<std::vector<ResolvedRoot>, RootPointerError> resolveRoots(
    Manifest const& manifest,
    std::string_view configJson,
    std::string_view home,
    core::platform::FileSystem const& fileSystem,
    vtpty::PathStyle style)
{
    auto roots = std::vector<ResolvedRoot> {};
    roots.reserve(manifest.roots.size());
    for (auto const& declared: manifest.roots)
    {
        auto values = rootValuesOf(configJson, declared.pointer);
        if (!values)
            return std::unexpected(values.error());
        auto root = ResolvedRoot { .label = declared.label, .paths = {} };
        for (auto const& value: *values)
        {
            auto path = resolveRootPath(value, home, fileSystem, style);
            if (!path)
                return std::unexpected(path.error());
            root.paths.push_back(std::move(*path));
        }
        roots.push_back(std::move(root));
    }
    if (!checkRootsDistinct(roots, style))
        return std::unexpected(RootPointerError::EqualRoots);
    return roots;
}

std::string grantDigestOf(std::string_view manifestDigest, std::span<ResolvedRoot const> roots)
{
    auto hasher = core::Sha256 {};
    hasher.update(manifestDigest);
    hasher.update("\n");
    for (auto const& root: roots)
    {
        for (auto const& path: root.paths)
        {
            hasher.update(root.label);
            hasher.update("\t");
            hasher.update(path);
            hasher.update("\n");
        }
    }
    return core::toLowerHexString(hasher.finish());
}

} // namespace vtextension
