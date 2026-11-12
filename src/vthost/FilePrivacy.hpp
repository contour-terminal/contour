// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Making a file or directory the current user's alone. The snapshot holds the scrollback of every pane,
/// which can hold anything a user ever typed or printed, so it is never readable by anyone else.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <string>

namespace vthost
{

/// What kind of entry is being restricted; a directory also governs what is created beneath it.
enum class PrivateEntry : std::uint8_t
{
    File = 0,
    Directory = 1,
};

/// Who owns a file and its POSIX permission bits, as `fstat` reports them.
struct FileOwnership
{
    std::uint32_t ownerId = 0; ///< The owner's user id.
    std::uint32_t mode = 0;    ///< The permission bits (`st_mode & 07777`).
};

/// Decides whether a file whose identity is a secret may be used: it must be the user's own, and nobody
/// else's to read or write. Anyone who can plant or read it could pass for the one it identifies.
/// @param file Who owns it and its mode.
/// @param userId The effective user id of this process.
/// @param path The file, for the message.
/// @return Nothing, or why it is refused.
[[nodiscard]] inline std::expected<void, std::string> checkPrivateFile(FileOwnership file,
                                                                       std::uint32_t userId,
                                                                       std::filesystem::path const& path)
{
    constexpr auto GroupAndOthers = std::uint32_t { 0b000'111'111 };
    if (file.ownerId != userId)
        return std::unexpected(std::format("'{}' belongs to user {}, not to this one ({}); it is not used",
                                           path.string(),
                                           file.ownerId,
                                           userId));
    if ((file.mode & GroupAndOthers) != 0)
        return std::unexpected(
            std::format("'{}' can be read or written by others (mode {:04o}); run `chmod 600 "
                        "'{}'` if it is yours, else remove it",
                        path.string(),
                        file.mode,
                        path.string()));
    return {};
}

/// The seam to the OS's permission model, so the store's call order is testable without one.
class IFilePrivacy
{
  public:
    IFilePrivacy() = default;
    IFilePrivacy(IFilePrivacy const&) = delete;
    IFilePrivacy(IFilePrivacy&&) = delete;
    IFilePrivacy& operator=(IFilePrivacy const&) = delete;
    IFilePrivacy& operator=(IFilePrivacy&&) = delete;
    virtual ~IFilePrivacy() = default;

    /// Makes @p path readable and writable by the current user alone.
    /// @param path The existing file or directory.
    /// @param kind Which of the two it is.
    /// @return Nothing, or why the OS refused.
    [[nodiscard]] virtual std::expected<void, std::string> restrictToOwner(std::filesystem::path const& path,
                                                                           PrivateEntry kind) const = 0;

    /// Reads a file that must be the current user's alone (checkPrivateFile()). On POSIX the check is made
    /// on the open descriptor, so that what was checked is what is read; on Windows the file is read as is.
    /// @param path The file.
    /// @return Its content, or why it cannot be read or is refused.
    [[nodiscard]] virtual std::expected<std::string, std::string> readPrivateFile(
        std::filesystem::path const& path) const = 0;
};

/// The OS's own: mode 0600/0700 on POSIX, a protected DACL granting only the current user on Windows.
class NativeFilePrivacy final: public IFilePrivacy
{
  public:
    /// @return The one stateless instance.
    [[nodiscard]] static NativeFilePrivacy const& instance()
    {
        static auto const value = NativeFilePrivacy {};
        return value;
    }

    [[nodiscard]] std::expected<void, std::string> restrictToOwner(std::filesystem::path const& path,
                                                                   PrivateEntry kind) const override;

    [[nodiscard]] std::expected<std::string, std::string> readPrivateFile(
        std::filesystem::path const& path) const override;
};

} // namespace vthost
