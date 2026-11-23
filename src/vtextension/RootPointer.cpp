// SPDX-License-Identifier: Apache-2.0
#include <vtextension/RootPointer.hpp>

#include <vtpty/PathStyle.hpp>

#include <core/Sha256.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>
#include <utility>

#include <glaze/json.hpp>
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

std::expected<std::string, RootPointerError> resolveRootPath(std::string_view raw,
                                                             std::string_view home,
                                                             core::platform::FileSystem const& fileSystem)
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
    auto const path = core::pathFromUtf8(expanded);
    if (!path)
        return std::unexpected(RootPointerError::NotAbsolute);
    // Without a trailing `/`, which RootResolution reads as an empty component, and which would keep two
    // spellings of one directory apart.
    auto resolved = core::pathToUtf8(fileSystem.weaklyCanonical(*path));
    while (resolved.size() > 1 && resolved.ends_with('/'))
        resolved.pop_back();
    return resolved;
}

std::expected<std::vector<ResolvedRoot>, RootPointerError> resolveRoots(
    Manifest const& manifest,
    std::string_view configJson,
    std::string_view home,
    core::platform::FileSystem const& fileSystem)
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
            auto path = resolveRootPath(value, home, fileSystem);
            if (!path)
                return std::unexpected(path.error());
            root.paths.push_back(std::move(*path));
        }
        roots.push_back(std::move(root));
    }
    if (!checkRootsDistinct(roots, vtpty::NativePathStyle))
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
