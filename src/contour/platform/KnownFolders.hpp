// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The folders the operating system names for the user, behind an interface so that no caller asks
/// the shell directly and a test can say where they are. Qt-free.

#include <filesystem>
#include <memory>
#include <optional>

namespace contour::platform
{

/// The user's known folders.
class IKnownFolders
{
  public:
    virtual ~IKnownFolders() = default;

    /// The user's Documents folder, where Windows PowerShell and PowerShell keep their profiles.
    /// @return Its path, or nullopt where the platform has no such folder or it cannot be found.
    [[nodiscard]] virtual std::optional<std::filesystem::path> documents() const = 0;
};

/// Known folders for a platform that names none, and for tests that must reach no real folder.
class NullKnownFolders final: public IKnownFolders
{
  public:
    /// @return Always nullopt.
    [[nodiscard]] std::optional<std::filesystem::path> documents() const override { return std::nullopt; }
};

/// The platform's known folders.
/// @return On Windows, the shell's known folders (`SHGetKnownFolderPath`); elsewhere NullKnownFolders.
[[nodiscard]] std::unique_ptr<IKnownFolders> makePlatformKnownFolders();

} // namespace contour::platform
