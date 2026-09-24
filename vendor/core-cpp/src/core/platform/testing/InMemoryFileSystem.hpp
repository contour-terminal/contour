// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace core::platform::testing
{

/// Describes a single filesystem entry for InMemoryFileSystem initialization.
struct FileEntry
{
    std::filesystem::path path {};
    std::string content {};
    std::filesystem::perms perms = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    bool isExecutable = false; ///< Shorthand for adding owner_exec to perms.
    bool isDirectory = false;  ///< If true, creates a directory instead of a file.
    bool isSymlink = false;    ///< If true, content is treated as symlink target.
};

/// In-memory filesystem implementation for testing.
///
/// Stores files as string contents in a map, supports basic directory operations.
/// Files are keyed by their lexically-normal path string for consistent lookup.
///
/// Answers as `NativeFileSystem` does wherever it models the behaviour; what it deliberately does
/// not model -- `exists()` on a dangling symlink, the OS's symlink resolution and directory
/// semantics, two stream put-back corner cases -- is tabled in `docs/modules/platform.md`, so a
/// test that depends on one of those belongs against the real filesystem.
///
/// Usage:
/// @code
/// InMemoryFileSystem fs({
///     { "/home/user/.config/app/init.conf", "echo hello" },
///     { "/usr/bin/grep", "", { .isExecutable = true } },
///     { "/tmp/data", "", { .isDirectory = true } },
/// });
/// @endcode
class InMemoryFileSystem final: public FileSystem
{
  public:
    InMemoryFileSystem() = default;

    /// Construct with a list of file entries for convenient test setup.
    InMemoryFileSystem(std::initializer_list<FileEntry> entries);

    // Path queries
    [[nodiscard]] bool exists(std::filesystem::path const& path) const override;
    [[nodiscard]] bool isDirectory(std::filesystem::path const& path) const override;
    [[nodiscard]] bool isRegularFile(std::filesystem::path const& path) const override;
    [[nodiscard]] bool isSymlink(std::filesystem::path const& path) const override;
    [[nodiscard]] bool isExecutableFile(std::filesystem::path const& path) const override;
    [[nodiscard]] std::filesystem::path weaklyCanonical(std::filesystem::path const& path) const override;
    [[nodiscard]] std::filesystem::path currentPath() const override;

    // File I/O
    [[nodiscard]] std::expected<std::string, std::string> readFile(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<void, std::string> writeFile(std::filesystem::path const& path,
                                                             std::string_view content) const override;
    [[nodiscard]] std::expected<void, std::string> appendFile(std::filesystem::path const& path,
                                                              std::string_view content) const override;
    [[nodiscard]] std::expected<std::unique_ptr<std::istream>, std::string> openRead(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<std::unique_ptr<std::ostream>, std::string> openWrite(
        std::filesystem::path const& path, WriteMode mode = WriteMode::Truncate) const override;
    [[nodiscard]] std::expected<std::unique_ptr<std::iostream>, std::string> openReadWrite(
        std::filesystem::path const& path) const override;

    // Directory ops
    [[nodiscard]] std::expected<void, std::string> createDirectory(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<void, std::string> createDirectories(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<bool, std::string> remove(std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<std::uintmax_t, std::string> removeAll(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<void, std::string> copyFile(
        std::filesystem::path const& from,
        std::filesystem::path const& to,
        OverwritePolicy policy = OverwritePolicy::Refuse) const override;
    [[nodiscard]] std::expected<void, std::string> rename(std::filesystem::path const& from,
                                                          std::filesystem::path const& to) const override;

    // Directory listing
    [[nodiscard]] std::expected<std::vector<DirectoryEntry>, std::string> listDirectory(
        std::filesystem::path const& path) const override;
    [[nodiscard]] Generator<DirectoryEntry> walkDirectoryRecursive(
        std::filesystem::path path, std::error_code* outError = nullptr) const override;

    // Metadata
    [[nodiscard]] std::expected<std::uintmax_t, std::string> fileSize(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<std::filesystem::file_time_type, std::string> lastWriteTime(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<std::filesystem::perms, std::string> permissions(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<void, std::string> setPermissions(
        std::filesystem::path const& path, std::filesystem::perms perms) const override;

    // Temp files
    [[nodiscard]] std::expected<std::filesystem::path, std::string> createTempFile(
        std::string_view prefix) const override;

    // --- Test helpers ---

    /// Sets the current working directory for the virtual filesystem.
    void setCurrentPath(std::filesystem::path const& path);

    /// Adds a file with the given content and optional permissions.
    void addFile(std::filesystem::path const& path,
                 std::string content,
                 std::filesystem::perms perms = std::filesystem::perms::owner_read
                                                | std::filesystem::perms::owner_write);

    /// Adds an executable file (owner_read | owner_write | owner_exec).
    void addExecutable(std::filesystem::path const& path, std::string content = {});

    /// Adds an empty directory.
    void addDirectory(std::filesystem::path const& path);

    /// Adds a symlink entry (points to target, stored as metadata).
    void addSymlink(std::filesystem::path const& path, std::filesystem::path const& target);

    /// Deny all access to a path (simulate EACCES / EPERM).
    void denyAccess(std::filesystem::path const& path);

  private:
    /// @brief Follows a symlink chain to the key it ultimately names.
    ///
    /// std::filesystem's is_regular_file(), is_directory() and the stream opens all follow
    /// symlinks; only is_symlink() and symlink_status() do not. This model has to agree, or a
    /// program reading through it would classify a symlink as neither file nor directory and
    /// take a path meant for FIFOs and devices.
    ///
    /// @param key A normalized path.
    /// @return The normalized key the chain ends at, or @p key when it is not a symlink. A
    ///         cycle resolves to the last key reached rather than looping.
    [[nodiscard]] std::string resolveSymlinks(std::string key) const;

    [[nodiscard]] std::string normalize(std::filesystem::path const& path) const;
    void ensureParentDirectories(std::filesystem::path const& path) const;

    /// @brief The string holding @p key's content, created empty if the file is not there yet.
    ///
    /// The one place a file's storage is made, so that a key always keeps the same string: a
    /// writer that replaced it would detach any stream already open on that file.
    ///
    /// @param key A normalized path.
    /// @return The shared content, never null.
    [[nodiscard]] std::shared_ptr<std::string>& fileAt(std::string const& key) const;

    /// @brief Refuses an open the model can answer without looking at content.
    /// @param key A normalized path.
    /// @return The reason to refuse, or nullopt to proceed.
    [[nodiscard]] std::optional<std::string> refuseOpen(std::string const& key) const;

    /// path -> content. The content is shared, not held by value: a stream handed out by
    /// openWrite() or openReadWrite() keeps its own reference, so remove() and rename() cannot
    /// leave it pointing at a string that is gone -- and a write through one is seen by the
    /// other, as two handles on one file see each other on a real filesystem.
    mutable std::map<std::string, std::shared_ptr<std::string>> _files;
    mutable std::set<std::string> _directories;                         ///< known directories
    mutable std::map<std::string, std::string> _symlinks;               ///< path -> target
    mutable std::map<std::string, std::filesystem::perms> _permissions; ///< path -> permissions
    std::set<std::string> _deniedPaths;                                 ///< paths that simulate EACCES
    std::filesystem::path _currentPath = "/";
    mutable int _tempCounter = 0;
};

} // namespace core::platform::testing
