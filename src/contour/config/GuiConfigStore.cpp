// SPDX-License-Identifier: Apache-2.0
#include <contour/config/AtomicFileWrite.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace contour::config
{

namespace
{
    /// Where the side files of one kind live, relative to the configuration directory.
    struct SideFileLocation
    {
        SideFileKind kind;              ///< The kind this row places.
        std::string_view directory;     ///< The directory of a kind kept one file per name; empty otherwise.
        std::string_view singletonName; ///< The file of a kind there is one of; empty otherwise.
    };

    /// One row per SideFileKind, in enumerator order.
    constexpr auto SideFileLocations = std::array {
        SideFileLocation {
            .kind = SideFileKind::GuiSettings, .directory = {}, .singletonName = "settings.yml" },
        SideFileLocation { .kind = SideFileKind::Profile, .directory = "profiles", .singletonName = {} },
        SideFileLocation {
            .kind = SideFileKind::ColorScheme, .directory = "colorschemes", .singletonName = {} },
    };

    static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, SideFileLocations.size()),
                                      [](size_t index) {
                                          return static_cast<size_t>(SideFileLocations.at(index).kind)
                                                 == index;
                                      }),
                  "SideFileLocations is indexed by SideFileKind");

    [[nodiscard]] SideFileLocation const& locationOf(SideFileKind kind)
    {
        return SideFileLocations.at(static_cast<size_t>(kind));
    }
} // namespace

SideFile guiSettingsFile()
{
    return SideFile { .kind = SideFileKind::GuiSettings, .name = {} };
}

SideFile profileFile(std::string name)
{
    return SideFile { .kind = SideFileKind::Profile, .name = std::move(name) };
}

SideFile colorSchemeFile(std::string name)
{
    return SideFile { .kind = SideFileKind::ColorScheme, .name = std::move(name) };
}

std::filesystem::path relativePathOf(SideFile const& file)
{
    auto const& location = locationOf(file.kind);
    if (!location.singletonName.empty())
        return std::filesystem::path { location.singletonName };
    return std::filesystem::path { location.directory } / sideFileName(file.name);
}

std::string displayPathOf(SideFile const& file)
{
    auto const spelled = relativePathOf(file).generic_u8string();
    return std::string { spelled.begin(), spelled.end() };
}

FileGuiConfigStore::FileGuiConfigStore(core::platform::FileSystem const& fileSystem,
                                       std::filesystem::path configDir):
    _fileSystem { fileSystem }, _configDir { std::move(configDir) }
{
}

std::expected<std::optional<std::string>, std::string> FileGuiConfigStore::read(SideFile const& file) const
{
    auto const path = _configDir / relativePathOf(file);
    if (!_fileSystem.exists(path))
        return std::optional<std::string> {};
    return _fileSystem.readFile(path).transform(
        [](std::string bytes) { return std::optional<std::string> { std::move(bytes) }; });
}

std::expected<void, std::string> FileGuiConfigStore::write(SideFile const& file, std::string_view bytes)
{
    return writeFileDurably(_fileSystem, _configDir / relativePathOf(file), bytes);
}

std::expected<void, std::string> FileGuiConfigStore::remove(SideFile const& file)
{
    // An already-absent file answers false: the post-state wanted either way.
    return _fileSystem.remove(_configDir / relativePathOf(file)).transform([](bool) {});
}

std::expected<std::vector<SideFile>, std::string> FileGuiConfigStore::list(SideFileKind kind) const
{
    auto const& location = locationOf(kind);
    auto files = std::vector<SideFile> {};
    if (!location.singletonName.empty())
    {
        if (_fileSystem.isRegularFile(_configDir / location.singletonName))
            files.push_back(SideFile { .kind = kind, .name = {} });
        return files;
    }

    auto const directory = _configDir / location.directory;
    if (!_fileSystem.isDirectory(directory))
        return files;
    auto const entries = _fileSystem.listDirectory(directory);
    if (!entries)
        return std::unexpected(entries.error());
    for (auto const& entry: *entries)
        // Its name, read back as sideFileName() spelled it. A temporary of writeFileDurably ends in .tmp.
        if (entry.isRegularFile && entry.path.extension() == ".yml")
            files.push_back(SideFile { .kind = kind, .name = core::pathToUtf8(entry.path.stem()) });
    std::ranges::sort(files);
    return files;
}

} // namespace contour::config
