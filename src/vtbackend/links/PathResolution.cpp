// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/PathResolution.hpp>

#include <core/Assert.hpp>
#include <core/Utf8Path.hpp>
#include <core/platform/FileSystem.hpp>

#include <ranges>
#include <utility>

namespace vtbackend
{

PathCandidates pathCandidates(std::span<std::string const> bases,
                              std::string_view home,
                              std::string_view printed)
{
    auto const joined = [](std::string_view directory,
                           std::string_view relative) -> std::optional<std::filesystem::path> {
        auto const base = core::pathFromUtf8(directory);
        auto const rest = core::pathFromUtf8(relative);
        if (directory.empty() || !base || !rest)
            return std::nullopt;
        return (*base / *rest).lexically_normal();
    };

    auto result = PathCandidates {};
    auto const place = [&result](std::filesystem::path path, std::string_view directory) {
        result.paths.push_back(std::move(path));
        result.directories.emplace_back(directory);
    };
    if (printed.starts_with("~/"))
    {
        if (auto path = joined(home, printed.substr(2)))
            place(std::move(*path), {});
        return result;
    }
    if (auto const path = core::pathFromUtf8(printed); path && path->is_absolute())
    {
        place(path->lexically_normal(), {});
        return result;
    }
    for (auto const& base: bases)
        if (auto path = joined(base, printed))
            place(std::move(*path), base);
    return result;
}

std::optional<ResolvedPath> resolveLocalPath(core::platform::FileSystem const& fileSystem,
                                             std::span<std::filesystem::path const> candidates)
{
    for (auto const index: std::views::iota(std::size_t { 0 }, candidates.size()))
    {
        auto const& candidate = candidates[index];
        if (fileSystem.isDirectory(candidate))
            return ResolvedPath { .path = candidate, .kind = PathKind::Directory, .candidateIndex = index };
        if (fileSystem.isRegularFile(candidate))
            return ResolvedPath { .path = candidate, .kind = PathKind::File, .candidateIndex = index };
    }
    return std::nullopt;
}

LinkTarget resolvedPathTarget(LinkTarget printed,
                              ResolvedPath const& answer,
                              std::span<std::string const> directories)
{
    Require(answer.candidateIndex < directories.size());
    printed.target = core::pathToUtf8(answer.path);
    printed.pathKind = answer.kind;
    printed.directory = directories[answer.candidateIndex];
    return printed;
}

} // namespace vtbackend
