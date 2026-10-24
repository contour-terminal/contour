// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Making a file or directory the current user's alone. The snapshot holds the scrollback of every pane,
/// which can hold anything a user ever typed or printed, so it is never readable by anyone else.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace vthost
{

/// What kind of entry is being restricted; a directory also governs what is created beneath it.
enum class PrivateEntry : std::uint8_t
{
    File = 0,
    Directory = 1,
};

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
};

} // namespace vthost
