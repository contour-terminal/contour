// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Generator.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace core::platform
{

/// @brief What an opened-for-writing file starts out holding.
///
/// Named rather than a `bool`, so `openWrite(path, WriteMode::Append)` says at the call site what
/// `openWrite(path, true)` only said in the header (`.agent/rules/design-principles.md`).
enum class WriteMode : std::uint8_t
{
    Truncate, ///< Discard whatever the file held.
    Append,   ///< Write after whatever the file held.
};

/// @brief What a copy does when the destination already exists.
///
/// Named rather than a `bool`, for the reason @ref WriteMode gives.
enum class OverwritePolicy : std::uint8_t
{
    Refuse,  ///< Fail, leaving the destination as it was.
    Replace, ///< Replace the destination's content.
};

/// Abstract interface for filesystem operations.
///
/// This interface abstracts all filesystem I/O, so code that takes it can be unit-tested
/// in isolation over testing::InMemoryFileSystem.
class FileSystem
{
  public:
    virtual ~FileSystem() = default;

    // Path queries
    [[nodiscard]] virtual bool exists(std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual bool isDirectory(std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual bool isRegularFile(std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual bool isSymlink(std::filesystem::path const& path) const = 0;

    /// Tests whether @p path names a file a process can execute.
    ///
    /// Returns true for regular files and symlinks to them, and — on Windows — also for
    /// App Execution Alias reparse points (e.g. `winget`, Microsoft Store `python`). These
    /// are zero-byte `IO_REPARSE_TAG_APPEXECLINK` reparse points that are neither regular
    /// files nor symlinks and cannot be opened or followed through normal filesystem APIs,
    /// yet they must stay discoverable on `PATH` so that `CreateProcessW` can launch them —
    /// matching how `cmd.exe` and PowerShell resolve such commands. On POSIX the file must
    /// additionally carry an execute permission bit. Directories always return false.
    ///
    /// @param path The path to test.
    /// @return True if @p path is a runnable executable file, false otherwise.
    [[nodiscard]] virtual bool isExecutableFile(std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual std::filesystem::path weaklyCanonical(std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual std::filesystem::path currentPath() const = 0;

    // File I/O
    [[nodiscard]] virtual std::expected<std::string, std::string> readFile(
        std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual std::expected<void, std::string> writeFile(std::filesystem::path const& path,
                                                                     std::string_view content) const = 0;
    [[nodiscard]] virtual std::expected<void, std::string> appendFile(std::filesystem::path const& path,
                                                                      std::string_view content) const = 0;
    /// @brief Opens a file for reading.
    ///
    /// The error is the reason alone -- "No such file or directory", "Permission denied",
    /// "Is a directory" -- so a caller prefixes it with its own command name and the path
    /// rather than reformatting it. Without this a caller can only observe *that* the open
    /// failed, and reporting an unreadable file as a missing one is a real diagnosis error.
    ///
    /// A directory is rejected here rather than left to the first read: opening one succeeds
    /// on POSIX and would otherwise surface as an empty file.
    ///
    /// @param path The file to open.
    /// @return The stream, or why it could not be opened.
    [[nodiscard]] virtual std::expected<std::unique_ptr<std::istream>, std::string> openRead(
        std::filesystem::path const& path) const = 0;

    /// @brief Opens a file for writing, creating it if absent.
    /// @param path The file to open.
    /// @param mode Whether to keep the existing content and write after it, or discard it.
    /// @return The stream, or why it could not be opened. See openRead() on the error's form.
    [[nodiscard]] virtual std::expected<std::unique_ptr<std::ostream>, std::string> openWrite(
        std::filesystem::path const& path, WriteMode mode = WriteMode::Truncate) const = 0;

    /// @brief Opens a file for reading and writing, keeping what it holds.
    ///
    /// The stream starts at the beginning of the file and carries one position for reading and
    /// writing, as `std::fstream` does: a write overwrites from wherever the stream stands and
    /// only extends the file past its end. Seek before switching between the two directions.
    ///
    /// @param path The file to open.
    /// @return The stream, or why it could not be opened. See openRead() on the error's form.
    [[nodiscard]] virtual std::expected<std::unique_ptr<std::iostream>, std::string> openReadWrite(
        std::filesystem::path const& path) const = 0;

    // Directory ops
    /// Creates a single directory. Fails if the parent does not exist.
    [[nodiscard]] virtual std::expected<void, std::string> createDirectory(
        std::filesystem::path const& path) const = 0;
    /// Creates a directory and all missing parent directories.
    [[nodiscard]] virtual std::expected<void, std::string> createDirectories(
        std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual std::expected<bool, std::string> remove(
        std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual std::expected<std::uintmax_t, std::string> removeAll(
        std::filesystem::path const& path) const = 0;
    /// Copies @p from onto @p to, creating it.
    /// @param from The file to copy.
    /// @param to Where to copy it.
    /// @param policy What to do when @p to already exists.
    /// @return Nothing, or why the copy failed.
    [[nodiscard]] virtual std::expected<void, std::string> copyFile(
        std::filesystem::path const& from,
        std::filesystem::path const& to,
        OverwritePolicy policy = OverwritePolicy::Refuse) const = 0;
    [[nodiscard]] virtual std::expected<void, std::string> rename(std::filesystem::path const& from,
                                                                  std::filesystem::path const& to) const = 0;

    // Directory listing
    struct DirectoryEntry
    {
        std::filesystem::path path;
        bool isDirectory = false;
        bool isRegularFile = false;
        bool isSymlink = false;
        /// Depth below the walk root: 1 for a direct child, 2 for a grandchild, etc.
        /// 0 for entries that are not the product of a recursive walk (e.g. listDirectory).
        int depth = 0;
    };

    [[nodiscard]] virtual std::expected<std::vector<DirectoryEntry>, std::string> listDirectory(
        std::filesystem::path const& path) const = 0;

    /// Materializes @ref walkDirectoryRecursive into a vector (parents before their contents).
    ///
    /// Implemented once here in terms of the lazy walk, so backends only provide the coroutine.
    /// Prefer @ref walkDirectoryRecursive directly for large trees or interruptible consumers;
    /// this convenience eagerly buffers the whole tree.
    ///
    /// @param path Root directory to list recursively.
    /// @return Every entry under @p path, or an error string if enumeration failed.
    [[nodiscard]] std::expected<std::vector<DirectoryEntry>, std::string> listDirectoryRecursive(
        std::filesystem::path const& path) const
    {
        std::error_code ec;
        auto entries = std::vector<DirectoryEntry> {};
        for (auto const& entry: walkDirectoryRecursive(path, &ec))
            entries.push_back(entry);
        if (ec)
            return std::unexpected(
                std::format("Cannot recursively list directory '{}': {}", path.string(), ec.message()));
        return entries;
    }

    /// Lazily walks @p path recursively, yielding each entry as it is discovered
    /// (parents before their contents) without materializing the whole tree.
    /// Permission-denied subtrees are skipped.
    ///
    /// Iterating the returned generator drives the walk one entry at a time;
    /// abandoning it (e.g. `break`-ing out of the loop) destroys the suspended
    /// coroutine and stops the walk. That single-step laziness is what lets
    /// long-running consumers (find, cp, rm, grep) poll for Ctrl+C between
    /// entries and abort promptly.
    ///
    /// @param path Root directory to walk. Taken BY VALUE: the implementation is
    ///             a coroutine, so a by-reference parameter would dangle in the
    ///             coroutine frame once the caller's argument expires.
    /// @param outError Optional out-parameter (taken by pointer, not reference,
    ///             to keep the coroutine free of reference parameters; it must
    ///             outlive the returned generator). When non-null, it is set to
    ///             the error that aborted the walk — a non-existent/non-directory
    ///             root, or an iteration error (other than skipped
    ///             permission-denied subtrees). Destructive callers (cp/mv/rm)
    ///             pass it so they can avoid acting on a partial enumeration;
    ///             read-only callers (find/grep) leave it null and tolerate a
    ///             partial walk. It is left untouched on a fully successful walk.
    /// @return A generator of entries. A non-existent or non-directory root, or
    ///         an unreadable root, simply yields nothing. The owning FileSystem
    ///         must outlive the returned generator.
    [[nodiscard]] virtual Generator<DirectoryEntry> walkDirectoryRecursive(
        std::filesystem::path path, std::error_code* outError = nullptr) const = 0;

    // Metadata
    [[nodiscard]] virtual std::expected<std::uintmax_t, std::string> fileSize(
        std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual std::expected<std::filesystem::file_time_type, std::string> lastWriteTime(
        std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual std::expected<std::filesystem::perms, std::string> permissions(
        std::filesystem::path const& path) const = 0;
    [[nodiscard]] virtual std::expected<void, std::string> setPermissions(
        std::filesystem::path const& path, std::filesystem::perms perms) const = 0;

    // Temp files
    [[nodiscard]] virtual std::expected<std::filesystem::path, std::string> createTempFile(
        std::string_view prefix) const = 0;
};

} // namespace core::platform
