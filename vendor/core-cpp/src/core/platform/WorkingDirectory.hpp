// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/PlatformError.hpp>

#include <expected>
#include <filesystem>
#include <memory>

namespace core::platform
{

/// @brief The process's working directory, behind a seam a test can replace.
///
/// Its own interface rather than two members of the environment one: a working directory is not
/// an environment variable, and code that reads `HOME` has no business changing directory.
class WorkingDirectory
{
  public:
    WorkingDirectory() = default;
    virtual ~WorkingDirectory() = default;

    WorkingDirectory(WorkingDirectory const&) = delete;
    WorkingDirectory& operator=(WorkingDirectory const&) = delete;
    WorkingDirectory(WorkingDirectory&&) = delete;
    WorkingDirectory& operator=(WorkingDirectory&&) = delete;

    /// Changes the working directory.
    ///
    /// @param path The new working directory, absolute or relative to the current one.
    /// @return Nothing, or why it could not be changed.
    [[nodiscard]] virtual std::expected<void, PlatformError> changeDirectory(
        std::filesystem::path const& path) = 0;

    /// @return The working directory, with forward slashes; on Windows in the capitalization the
    ///         filesystem stores, whatever case it was changed to. A path rather than a string, so
    ///         it round-trips through @c changeDirectory whatever it spells: a string converted to
    ///         a path reads through the ANSI code page on Windows. @c normalizePath gives its
    ///         UTF-8 spelling.
    [[nodiscard]] virtual std::filesystem::path currentDirectory() const = 0;
};

/// @brief Creates this operating system's own WorkingDirectory, for a composition root: the
/// process's working directory, which every instance shares.
///
/// @return The working directory, owned by the caller.
[[nodiscard]] std::unique_ptr<WorkingDirectory> nativeWorkingDirectory();

} // namespace core::platform
