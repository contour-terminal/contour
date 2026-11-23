// SPDX-License-Identifier: Apache-2.0
#include <vtextension/FileReadService.hpp>

#include <vtpty/BeneathRead.hpp>
#include <vtpty/DirectoryHandle.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/RootResolution.hpp>

namespace vtextension
{

namespace
{
    /// What a read beneath a root's directory answers, as the world's `file-error`.
    struct ReadErrorRow
    {
        vtpty::BeneathReadError read;
        FileError file;
    };

    // clang-format off
    constexpr auto ReadErrorRows = std::array {
        ReadErrorRow { vtpty::BeneathReadError::DirectoryMissing, FileError::DirectoryGone },
        ReadErrorRow { vtpty::BeneathReadError::NotRegular,       FileError::NotRegular },
        ReadErrorRow { vtpty::BeneathReadError::Unreadable,       FileError::Unreadable },
        ReadErrorRow { vtpty::BeneathReadError::TooLarge,         FileError::TooLarge },
        ReadErrorRow { vtpty::BeneathReadError::Escapes,          FileError::Escapes },
        ReadErrorRow { vtpty::BeneathReadError::Unsupported,      FileError::NotSupportedYet },
        ReadErrorRow { vtpty::BeneathReadError::Fenced,           FileError::NotGranted },
    };
    // clang-format on

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ReadErrorRows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(ReadErrorRows.at(index).read) == index;
                                      }),
                  "ReadErrorRows is indexed by BeneathReadError, one row each");

    /// What opening a root's directory answers, as the world's `file-error`.
    struct DirectoryErrorRow
    {
        vtpty::DirectoryError directory;
        FileError file;
    };

    // clang-format off
    constexpr auto DirectoryErrorRows = std::array {
        DirectoryErrorRow { vtpty::DirectoryError::Missing,       FileError::DirectoryGone },
        DirectoryErrorRow { vtpty::DirectoryError::NotADirectory, FileError::DirectoryGone },
        DirectoryErrorRow { vtpty::DirectoryError::AccessDenied,  FileError::Unreadable },
        DirectoryErrorRow { vtpty::DirectoryError::Unsupported,   FileError::NotSupportedYet },
    };
    // clang-format on

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, DirectoryErrorRows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(DirectoryErrorRows.at(index).directory)
                                                 == index;
                                      }),
                  "DirectoryErrorRows is indexed by DirectoryError, one row each");

    /// @return @p path's components; std::nullopt when it is empty, absolute, or holds an empty, `.` or `..`
    ///         component or a NUL.
    [[nodiscard]] std::optional<std::vector<std::string_view>> componentsOf(std::string_view path)
    {
        if (path.empty() || path.starts_with('/') || path.contains('\0'))
            return std::nullopt;
        auto components = std::vector<std::string_view> {};
        for (auto const component: path | std::views::split('/'))
        {
            auto const name = std::string_view { component.begin(), component.end() };
            if (name.empty() || name == "." || name == "..")
                return std::nullopt;
            components.push_back(name);
        }
        return components;
    }

    /// @return @p components joined beneath @p directory.
    [[nodiscard]] std::string pathBeneath(std::string_view directory,
                                          std::span<std::string_view const> components)
    {
        auto path = std::string { directory == "/" ? "" : directory };
        for (auto const component: components)
            path.append("/").append(component);
        return path;
    }
} // namespace

FileReadService::FileReadService(Manifest const& manifest,
                                 std::vector<ResolvedRoot> roots,
                                 vtpty::IDirectoryOpener& directories,
                                 vtpty::IBeneathReader& reader):
    _readableRoots { manifest.readableRoots },
    _roots { std::move(roots) },
    _directories { directories },
    _reader { reader }
{
}

std::expected<FileContent, FileError> FileReadService::read(std::string_view root, std::string_view path)
{
    if (!std::ranges::contains(_readableRoots, root))
        return std::unexpected(FileError::NotGranted);
    auto const components = componentsOf(path);
    if (!components)
        return std::unexpected(FileError::BadArgument);
    auto const declared = std::ranges::find(_roots, root, &ResolvedRoot::label);
    if (declared == _roots.end())
        return std::unexpected(FileError::NotGranted);

    auto const lock = std::scoped_lock { _mutex };
    auto answer = std::expected<FileContent, FileError> { std::unexpect, FileError::DirectoryGone };
    for (auto const& directory: declared->paths)
    {
        // A root nested inside this one keeps its files to itself, as it keeps the commands of this one out:
        // by its spelling here, and by its identity in the walk (a case-insensitive name spells it
        // otherwise).
        if (mostSpecificRoot(_roots, pathBeneath(directory, *components), _directories.pathStyle()) != root)
            return std::unexpected(FileError::NotGranted);
        answer = readIn(root, directory, *components);
        if (answer || answer.error() != FileError::DirectoryGone)
            return answer;
    }
    return answer;
}

FileReadService::OpenedDirectory FileReadService::openDirectory(std::string_view root,
                                                                std::string_view directory) const
{
    auto opened = OpenedDirectory {};
    auto handle = _directories.open(directory);
    if (!handle)
    {
        opened.handle = std::unexpected(DirectoryErrorRows.at(std::to_underlying(handle.error())).file);
        return opened;
    }
    // The opener follows links: a root's directory swapped for one is caught by where it now leads.
    if (handle->resolvedPath() != directory)
    {
        opened.handle = std::unexpected(FileError::Escapes);
        return opened;
    }
    opened.handle = std::move(*handle);
    // Only another root's directory beneath this one can be walked into. Each is found from the open handle
    // as a read walks, following no link and (on Linux) crossing no mount, so a mount over it is never asked.
    for (auto const& other: _roots)
    {
        if (other.label == root)
            continue;
        for (auto const& nested: other.paths)
        {
            auto const prefix = directory == "/" ? std::string_view {} : directory;
            auto const below = std::string_view { nested }.substr(std::min(prefix.size(), nested.size()));
            if (!nested.starts_with(prefix) || !below.starts_with('/'))
                continue;
            if (auto const components = componentsOf(below.substr(1)))
                opened.nested.push_back(NestedRoot {
                    .components = std::vector<std::string>(components->begin(), components->end()),
                    .identity = std::nullopt });
        }
    }
    return opened;
}

std::vector<vtpty::FileIdentity> FileReadService::fencesOf(OpenedDirectory& opened) const
{
    auto fenced = std::vector<vtpty::FileIdentity> {};
    for (auto& nested: opened.nested)
    {
        // Found once, kept: renaming the directory away and back cannot unfence it. One not reachable yet
        // (not there, or behind a mount) needs no fence, as the read cannot enter it either; it is looked
        // for again until it is found.
        if (!nested.identity)
        {
            auto const components =
                std::vector<std::string_view>(nested.components.begin(), nested.components.end());
            if (auto const identity = _reader.identityOf(*opened.handle, components))
                nested.identity = *identity;
        }
        if (nested.identity)
            fenced.push_back(*nested.identity);
    }
    return fenced;
}

std::expected<FileContent, FileError> FileReadService::readIn(std::string_view root,
                                                              std::string_view directory,
                                                              std::span<std::string_view const> components)
{
    auto cached = _opened.find(directory);
    if (cached == _opened.end())
        cached = _opened.emplace(std::string(directory), openDirectory(root, directory)).first;
    // A root that did not exist when it was last opened may exist now; any other failure stays.
    else if (!cached->second.handle && cached->second.handle.error() == FileError::DirectoryGone)
        cached->second = openDirectory(root, directory);
    auto answer = readBeneath(cached->second, components);
    // The held root itself was removed (and may be there again): opened anew, once.
    if (!answer && answer.error() == FileError::DirectoryGone && cached->second.handle
        && _reader.identityOf(*cached->second.handle, {})
               == std::unexpected(vtpty::BeneathReadError::DirectoryMissing))
    {
        cached->second = openDirectory(root, directory);
        answer = readBeneath(cached->second, components);
    }
    return answer;
}

std::expected<FileContent, FileError> FileReadService::readBeneath(
    OpenedDirectory& opened, std::span<std::string_view const> components) const
{
    if (!opened.handle)
        return std::unexpected(opened.handle.error());
    auto const fenced = fencesOf(opened);
    return _reader.read(*opened.handle, components, MaxFileReadBytes, fenced)
        .transform([](vtpty::BeneathFile file) {
            return FileContent { .bytes = ByteList(file.bytes.begin(), file.bytes.end()),
                                 .modifiedUnixMs = file.modifiedUnixMs };
        })
        .transform_error(
            [](vtpty::BeneathReadError error) { return ReadErrorRows.at(std::to_underlying(error)).file; });
}

} // namespace vtextension
