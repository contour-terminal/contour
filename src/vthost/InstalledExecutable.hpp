// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where an installation's executable is: the folder its installer recorded, not wherever a process happens
/// to run from.

#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace vthost
{

/// Where an installer records the folder it installed into: per machine, or per user.
enum class InstallRootHive : uint8_t
{
    LocalMachine = 0, ///< The per-machine installer's record.
    CurrentUser,      ///< The per-user installer's record.
};

/// What the installers recorded of where they installed. An interface because it reaches the registry.
class InstallRootSource
{
  public:
    InstallRootSource() = default;
    InstallRootSource(InstallRootSource const&) = delete;
    InstallRootSource& operator=(InstallRootSource const&) = delete;
    InstallRootSource(InstallRootSource&&) = delete;
    InstallRootSource& operator=(InstallRootSource&&) = delete;
    virtual ~InstallRootSource() = default;

    /// @param hive Whose record.
    /// @return The install folder that record names (`Software\Contour\Contour\InstallRoot`); nullopt for
    /// none.
    [[nodiscard]] virtual std::optional<std::string> recordedRoot(InstallRootHive hive) const = 0;
};

/// @return The source of this platform: the registry on Windows, where the installers record their folder;
///         one that knows none elsewhere.
[[nodiscard]] std::unique_ptr<InstallRootSource> makeNativeInstallRootSource();

/// @param roots What the installers recorded.
/// @param fileSystem Where the executable is looked for.
/// @return `<root>\bin\contour.exe` of the first record (the machine's, then the user's) whose executable
/// exists;
///         empty when there is none. A process that runs from elsewhere (a portable copy, a build tree) is
///         therefore never an installation.
[[nodiscard]] std::filesystem::path installedExecutableOf(InstallRootSource const& roots,
                                                          core::platform::FileSystem const& fileSystem);

} // namespace vthost
