// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <functional>

namespace core::platform
{

/// @brief Renames a directory entry, reporting failure through @p ec rather than by throwing.
///
/// The signature of `std::filesystem::rename`'s error_code overload.
using RenameFunction = std::function<void(
    std::filesystem::path const& from, std::filesystem::path const& to, std::error_code& ec)>;

/// @return The rename primitive @ref NativeFileSystem uses unless it is given another:
///         `std::filesystem::rename`.
[[nodiscard]] RenameFunction nativeRename();

/// Native filesystem implementation that delegates to std::filesystem and POSIX/Win32.
class NativeFileSystem final: public FileSystem
{
  public:
    /// @brief Constructs the backend over a rename primitive.
    ///
    /// The one filesystem call this class takes rather than makes, and it is injected for a
    /// reason no mock filesystem would serve: rename() retries a lettercase-only rename in two
    /// hops through a temporary name, and that retry only runs on a volume that refuses a
    /// case-only rename outright. Every volume this project builds and tests on -- ext4, APFS,
    /// NTFS, UFS -- performs one natively, so the retry cannot be reached from outside. It moves
    /// a consumer's entry through a temporary name and can leave it there when both the second
    /// hop and the rollback fail, which is not behaviour that may ship untested.
    ///
    /// @param rename The primitive to rename through. The default is @ref nativeRename(), so
    ///               every existing caller and @ref instance() are unaffected.
    explicit NativeFileSystem(RenameFunction rename = nativeRename());

    [[nodiscard]] static NativeFileSystem& instance();

    [[nodiscard]] bool exists(std::filesystem::path const& path) const override;
    [[nodiscard]] bool isDirectory(std::filesystem::path const& path) const override;
    [[nodiscard]] bool isRegularFile(std::filesystem::path const& path) const override;
    [[nodiscard]] bool isSymlink(std::filesystem::path const& path) const override;
    [[nodiscard]] bool isExecutableFile(std::filesystem::path const& path) const override;
    [[nodiscard]] std::filesystem::path weaklyCanonical(std::filesystem::path const& path) const override;
    [[nodiscard]] std::filesystem::path currentPath() const override;

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

    [[nodiscard]] std::expected<std::vector<DirectoryEntry>, std::string> listDirectory(
        std::filesystem::path const& path) const override;
    [[nodiscard]] Generator<DirectoryEntry> walkDirectoryRecursive(
        std::filesystem::path path, std::error_code* outError = nullptr) const override;

    [[nodiscard]] std::expected<std::uintmax_t, std::string> fileSize(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<std::filesystem::file_time_type, std::string> lastWriteTime(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<std::filesystem::perms, std::string> permissions(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<void, std::string> setPermissions(
        std::filesystem::path const& path, std::filesystem::perms perms) const override;

    [[nodiscard]] std::expected<std::filesystem::path, std::string> createTempFile(
        std::string_view prefix) const override;

  private:
    RenameFunction _rename;
};

} // namespace core::platform
