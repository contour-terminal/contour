// SPDX-License-Identifier: Apache-2.0
//
// An extension's store as a directory of one file per key, each replaced atomically and synced before a write
// returns.
#pragma once

#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/ExtensionStore.hpp>
#include <vtextension/FileLocker.hpp>

namespace vtextension
{

/// An extension's store in a directory of its own: one file per key, named the key with every `/` spelled
/// `%`, and a lock file `#lock`, held while the store lives.
///
/// set() writes the value to a new temporary `~<n>` (`~` is outside the key alphabet, so it never names a
/// key's file), syncs it, renames it over the key's file and syncs the directory; a step failing after the
/// temporary was created removes it, and every failure reports `Io`. A crash leaves the old value or the new
/// one, and at most a torn temporary, which get() and keys() ignore and open() removes.
///
/// Only regular files are values: an entry that is a symbolic link, a directory or anything else is not
/// listed by keys(), and get() answers nothing for it. Not thread-safe: one extension's calls are serialized
/// by its lane.
class FileExtensionStore final: public IExtensionStore
{
  public:
    /// Opens the store in @p directory, its single owner. Creates the directory (and its parents) if missing,
    /// and sets its mode to 0700 whether or not it was created. Takes the directory's exclusive lock, and
    /// removes the temporaries a crash left behind.
    /// @param fileSystem What the store is read and written through; must outlive the store.
    /// @param locker What takes the directory's lock; must outlive the call.
    /// @param directory The store's directory. A symbolic link is refused, never followed.
    /// @param limits How much the store may hold.
    /// @return The store; `Busy` if another store holds the directory; `Io` if it is a symbolic link, or
    ///         cannot be created, made private, locked or listed.
    [[nodiscard]] static std::expected<FileExtensionStore, StoreError> open(
        core::platform::FileSystem const& fileSystem,
        IFileLocker const& locker,
        std::filesystem::path directory,
        StoreLimits limits);

    FileExtensionStore(FileExtensionStore const&) = delete;
    FileExtensionStore(FileExtensionStore&&) = default;
    FileExtensionStore& operator=(FileExtensionStore const&) = delete;
    FileExtensionStore& operator=(FileExtensionStore&&) = delete;
    ~FileExtensionStore() override = default;

    [[nodiscard]] std::expected<std::optional<std::string>, StoreError> get(
        std::string_view key) const override;
    [[nodiscard]] std::expected<void, StoreError> set(std::string_view key, std::string_view value) override;
    [[nodiscard]] std::expected<void, StoreError> remove(std::string_view key) override;
    [[nodiscard]] std::expected<std::vector<std::string>, StoreError> keys(
        std::string_view prefix) const override;

  private:
    FileExtensionStore(core::platform::FileSystem const& fileSystem,
                       std::unique_ptr<FileLock> lock,
                       std::filesystem::path directory,
                       StoreLimits limits);

    /// @return The file that holds @p key's value; or `BadKey`.
    [[nodiscard]] std::expected<std::filesystem::path, StoreError> fileOf(std::string_view key) const;

    /// @return Every key the directory holds, unsorted; or `Io`.
    [[nodiscard]] std::expected<std::vector<std::string>, StoreError> allKeys() const;

    /// @return Whether @p file is a value: a regular file, not a symbolic link to one.
    [[nodiscard]] bool isValue(std::filesystem::path const& file) const;

    /// @return A temporary's path that names nothing yet.
    [[nodiscard]] std::filesystem::path nextTemporary();

    core::platform::FileSystem const& _fileSystem;
    std::unique_ptr<FileLock> _lock; ///< The directory's lock: this store is its single owner.
    std::filesystem::path _directory;
    StoreLimits _limits;
    uint64_t _nextTemporary = 0; ///< The `<n>` of the next temporary's name.
};

} // namespace vtextension
