// SPDX-License-Identifier: Apache-2.0
#include <contour/config/AtomicFileWrite.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <cctype>
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

namespace
{
    /// The names Windows keeps for devices; a file of that name, with any extension, opens the device.
    constexpr auto WindowsDeviceNames = std::array<std::string_view, 22> {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };

    /// Whether @p ch is a control character: below a space, or DEL.
    [[nodiscard]] constexpr bool isControl(char ch) noexcept
    {
        constexpr auto Delete = 0x7F;
        auto const code = static_cast<unsigned char>(ch);
        return code < static_cast<unsigned char>(' ') || code == Delete;
    }

    /// Whether @p name, up to its first dot, is a Windows device name, in any case.
    [[nodiscard]] bool namesWindowsDevice(std::string_view name)
    {
        auto const stem = name.substr(0, name.find('.'));
        return std::ranges::any_of(WindowsDeviceNames, [stem](std::string_view device) {
            return std::ranges::equal(stem, device, [](char a, char b) {
                return std::toupper(static_cast<unsigned char>(a))
                       == std::toupper(static_cast<unsigned char>(b));
            });
        });
    }

    /// One reason a name is refused, and the test that finds it.
    struct NameRule
    {
        bool (*refuses)(std::string_view name); ///< Whether the name breaks the rule.
        std::string_view reason;                ///< What the page says.
    };

    /// The rules checkSideFileName() applies, in order.
    constexpr auto NameRules = std::array {
        NameRule { .refuses = [](std::string_view name) { return name.empty(); }, .reason = "Enter a name." },
        NameRule { .refuses =
                       [](std::string_view name) {
                           return name.find_first_of(std::string_view { "/\\:" }) != std::string_view::npos;
                       },
                   .reason = "A name cannot contain /, \\ or :." },
        NameRule { .refuses = [](std::string_view name) { return std::ranges::any_of(name, isControl); },
                   .reason = "A name cannot contain a control character." },
        NameRule { .refuses = [](std::string_view name) { return name.contains(".."); },
                   .reason = "A name cannot contain \"..\"." },
        NameRule { .refuses = [](std::string_view name) { return name.starts_with('.'); },
                   .reason = "A name cannot start with a dot." },
        NameRule {
            .refuses = [](std::string_view name) { return name.ends_with('.') || name.ends_with(' '); },
            .reason = "A name cannot end with a dot or a space." },
        NameRule { .refuses = namesWindowsDevice,
                   .reason = "A name cannot be one Windows keeps for a device, such as CON or NUL." },
    };
} // namespace

std::expected<void, std::string> checkSideFileName(std::string_view name)
{
    for (auto const& rule: NameRules)
        if (rule.refuses(name))
            return std::unexpected(std::string { rule.reason });
    return {};
}

bool sideFilePathStaysIn(std::filesystem::path const& relative, std::filesystem::path const& directory)
{
    return !relative.has_root_name() && !relative.has_root_directory() && relative.parent_path() == directory;
}

namespace
{
    /// Whether @p file's path stays inside the configuration directory: the store's own guard, beside the
    /// page's, so that no caller can reach outside it.
    [[nodiscard]] std::expected<void, std::string> checkSideFile(SideFile const& file)
    {
        if (file.kind == SideFileKind::GuiSettings)
            return {};
        if (auto const valid = checkSideFileName(file.name); !valid)
            return valid;
        if (!sideFilePathStaysIn(relativePathOf(file),
                                 std::filesystem::path { locationOf(file.kind).directory }))
            return std::unexpected(std::string { "A name cannot leave its directory." });
        return {};
    }
} // namespace

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
    if (auto const valid = checkSideFile(file); !valid)
        return std::unexpected(valid.error());
    auto const path = _configDir / relativePathOf(file);
    if (!_fileSystem.exists(path))
        return std::optional<std::string> {};
    return _fileSystem.readFile(path).transform(
        [](std::string bytes) { return std::optional<std::string> { std::move(bytes) }; });
}

std::expected<void, std::string> FileGuiConfigStore::write(SideFile const& file, std::string_view bytes)
{
    if (auto const valid = checkSideFile(file); !valid)
        return valid;
    return writeFileDurably(_fileSystem, _configDir / relativePathOf(file), bytes);
}

std::expected<void, std::string> FileGuiConfigStore::remove(SideFile const& file)
{
    if (auto const valid = checkSideFile(file); !valid)
        return valid;
    // An already-absent file answers false: the post-state wanted either way, and nothing to sync. A removal
    // reaches the disk, as a replace does, with its directory synced.
    auto const path = _configDir / relativePathOf(file);
    return _fileSystem.remove(path).and_then([&](bool removed) -> std::expected<void, std::string> {
        if (!removed)
            return {};
        return _fileSystem.syncDirectory(path.parent_path());
    });
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
