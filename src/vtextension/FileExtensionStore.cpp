// SPDX-License-Identifier: Apache-2.0
#include <vtextension/FileExtensionStore.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <tuple>
#include <utility>

namespace vtextension
{

namespace
{
    using namespace std::string_view_literals;

    /// What a key may contain besides lowercase letters and digits.
    constexpr auto KeyPunctuation = "/._-"sv;

    /// Separates a key's components.
    constexpr auto KeySeparator = '/';

    /// What a key's separator is spelled as in its file's name: outside the key alphabet, so the mapping is
    /// one to one.
    constexpr auto FileSeparator = '%';

    /// The lock file's name: outside the key alphabet, and not a temporary's.
    constexpr auto LockFileName = "#lock"sv;

    /// Starts a temporary's name: outside the key alphabet, so no key's file is ever taken for one.
    constexpr auto TemporaryMark = '~';

    /// What no component may end in: Windows drops a trailing `.` from a name, so `a.` would be `a` there,
    /// and `.` and `..` would name directories.
    constexpr auto ForbiddenLastCharacter = '.';

    /// What no component may contain: the temporaries' spelling of other stores.
    constexpr auto ForbiddenInComponent = ".tmp-"sv;

    /// The names Windows reserves for devices, alone or before any extension. Keys are lowercase, so this
    /// spelling covers every case. Refused everywhere, so that a guest's keys are portable.
    constexpr auto ReservedDeviceNames = std::array {
        "con"sv,  "prn"sv,  "aux"sv,  "nul"sv,  "com1"sv, "com2"sv, "com3"sv, "com4"sv,
        "com5"sv, "com6"sv, "com7"sv, "com8"sv, "com9"sv, "lpt1"sv, "lpt2"sv, "lpt3"sv,
        "lpt4"sv, "lpt5"sv, "lpt6"sv, "lpt7"sv, "lpt8"sv, "lpt9"sv,
    };

    [[nodiscard]] constexpr bool isKeyCharacter(char ch) noexcept
    {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || KeyPunctuation.contains(ch);
    }

    /// @return Whether @p component may be one of a key's `/`-separated components.
    [[nodiscard]] constexpr bool isValidComponent(std::string_view component) noexcept
    {
        auto const deviceName = component.substr(0, component.find('.'));
        return !component.empty() && !component.ends_with(ForbiddenLastCharacter)
               && !component.contains(ForbiddenInComponent)
               && !std::ranges::contains(ReservedDeviceNames, deviceName);
    }

    /// @return Whether @p key is of the store's key grammar and at most @p maxBytes long.
    [[nodiscard]] bool isValidKey(std::string_view key, std::size_t maxBytes)
    {
        if (key.empty() || key.size() > maxBytes || !std::ranges::all_of(key, isKeyCharacter))
            return false;
        return std::ranges::all_of(key | std::views::split(KeySeparator), [](auto const& part) {
            return isValidComponent(std::string_view { part });
        });
    }

    [[nodiscard]] bool isTemporary(std::filesystem::path const& file)
    {
        return file.filename().string().starts_with(TemporaryMark);
    }

    /// Turns a filesystem failure into the store's.
    template <typename T>
    [[nodiscard]] std::expected<T, StoreError> orIo(std::expected<T, std::string> result)
    {
        return std::move(result).transform_error([](auto const&) { return StoreError::Io; });
    }
} // namespace

FileExtensionStore::FileExtensionStore(core::platform::FileSystem const& fileSystem,
                                       std::unique_ptr<FileLock> lock,
                                       std::filesystem::path directory,
                                       StoreLimits limits):
    _fileSystem { fileSystem },
    _lock { std::move(lock) },
    _directory { std::move(directory) },
    _limits { limits }
{
}

std::expected<FileExtensionStore, StoreError> FileExtensionStore::open(
    core::platform::FileSystem const& fileSystem,
    IFileLocker const& locker,
    std::filesystem::path directory,
    StoreLimits limits)
{
    // A link would have the store, and the mode below, land wherever it points.
    if (fileSystem.isSymlink(directory))
        return std::unexpected(StoreError::Io);
    auto const made = fileSystem.createDirectories(directory).and_then(
        [&] { return fileSystem.setPermissions(directory, std::filesystem::perms::owner_all); });
    if (!made || fileSystem.isSymlink(directory) || !fileSystem.isDirectory(directory))
        return std::unexpected(StoreError::Io);

    auto lock = locker.tryLockExclusive(directory / LockFileName);
    if (!lock)
        return std::unexpected(lock.error() == LockError::Held ? StoreError::Busy : StoreError::Io);

    auto const entries = fileSystem.listDirectory(directory);
    if (!entries)
        return std::unexpected(StoreError::Io);
    // What a crash left mid-write: never a value, and set() picks a name no file has, so one that cannot be
    // removed now is merely left behind. Only now, under the lock, since another store's are not torn.
    for (auto const& entry: *entries)
        if (isTemporary(entry.path))
            std::ignore = fileSystem.remove(entry.path);

    return FileExtensionStore { fileSystem, std::move(*lock), std::move(directory), limits };
}

bool FileExtensionStore::isValue(std::filesystem::path const& file) const
{
    return !_fileSystem.isSymlink(file) && _fileSystem.isRegularFile(file);
}

std::expected<std::filesystem::path, StoreError> FileExtensionStore::fileOf(std::string_view key) const
{
    if (!isValidKey(key, _limits.maxKeyBytes))
        return std::unexpected(StoreError::BadKey);
    auto name = std::string { key };
    std::ranges::replace(name, KeySeparator, FileSeparator);
    return _directory / name;
}

std::expected<std::vector<std::string>, StoreError> FileExtensionStore::allKeys() const
{
    return orIo(_fileSystem.listDirectory(_directory)).transform([&](auto const& entries) {
        auto keys = std::vector<std::string> {};
        for (auto const& entry: entries)
        {
            auto key = entry.path.filename().string();
            std::ranges::replace(key, FileSeparator, KeySeparator);
            // A temporary is not of the key grammar, nor is the lock file or anything else not put there
            // by set(); and only a regular file is a value.
            if (entry.isRegularFile && !entry.isSymlink && isValidKey(key, _limits.maxKeyBytes))
                keys.push_back(std::move(key));
        }
        return keys;
    });
}

std::filesystem::path FileExtensionStore::nextTemporary()
{
    auto const nameAt = [&](uint64_t number) {
        return _directory / std::format("{}{}", TemporaryMark, number);
    };
    auto temporary = nameAt(_nextTemporary++);
    while (_fileSystem.exists(temporary) || _fileSystem.isSymlink(temporary))
        temporary = nameAt(_nextTemporary++);
    return temporary;
}

std::expected<std::optional<std::string>, StoreError> FileExtensionStore::get(std::string_view key) const
{
    return fileOf(key).and_then(
        [&](auto const& file) -> std::expected<std::optional<std::string>, StoreError> {
            if (!isValue(file))
                return std::nullopt;
            return orIo(_fileSystem.readFile(file)).transform([](std::string value) {
                return std::optional { std::move(value) };
            });
        });
}

std::expected<void, StoreError> FileExtensionStore::set(std::string_view key, std::string_view value)
{
    auto const file = fileOf(key);
    if (!file)
        return std::unexpected(file.error());
    if (value.size() > _limits.maxValueBytes)
        return std::unexpected(StoreError::TooLarge);
    if (!isValue(*file))
    {
        auto const held = allKeys();
        if (!held)
            return std::unexpected(held.error());
        if (held->size() >= _limits.maxKeys)
            return std::unexpected(StoreError::TooManyKeys);
    }

    auto const temporary = nextTemporary();
    if (!_fileSystem.createNewFile(temporary))
        return std::unexpected(StoreError::Io); // Nothing of ours to remove: what is there is not ours.
    auto const replaced = _fileSystem.writeFile(temporary, value)
                              .and_then([&] { return _fileSystem.syncFile(temporary); })
                              .and_then([&] { return _fileSystem.replaceFile(temporary, *file); });
    if (!replaced)
    {
        // The old value stands. A temporary that cannot be removed now is removed by the next open().
        std::ignore = _fileSystem.remove(temporary);
        return std::unexpected(StoreError::Io);
    }
    return orIo(_fileSystem.syncDirectory(_directory));
}

std::expected<void, StoreError> FileExtensionStore::remove(std::string_view key)
{
    return fileOf(key).and_then([&](auto const& file) {
        return orIo(_fileSystem.remove(file)).and_then([&](bool) {
            return orIo(_fileSystem.syncDirectory(_directory));
        });
    });
}

std::expected<std::vector<std::string>, StoreError> FileExtensionStore::keys(std::string_view prefix) const
{
    return allKeys().transform([&](std::vector<std::string> keys) {
        std::erase_if(keys, [&](std::string const& key) { return !key.starts_with(prefix); });
        std::ranges::sort(keys);
        return keys;
    });
}

} // namespace vtextension
