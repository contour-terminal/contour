// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file ScopedWorkingDirectory.hpp
/// @brief Changes the process working directory and restores it on destruction.

#include <filesystem>
#include <system_error>

namespace core::testing
{

/// @brief Sets the process working directory for a scope, restoring the previous one.
///
/// The working directory is process-global, so a test that changes it and restores it at
/// the end of the test body leaks that change whenever an assertion fails first -- and if
/// the directory it moved into is then deleted, every later test resolving a relative path
/// runs from a deleted inode. Restoring from a destructor makes that impossible.
class ScopedWorkingDirectory
{
  public:
    /// @brief Changes the working directory to @p path.
    /// @param path Directory to switch to.
    /// @throws std::filesystem::filesystem_error if the directory cannot be changed.
    ///
    /// Deliberately not error-swallowing: a test that believes it moved but did not would
    /// run its body against the wrong directory and report a confusing failure elsewhere.
    explicit ScopedWorkingDirectory(std::filesystem::path const& path):
        _previous { std::filesystem::current_path() }
    {
        std::filesystem::current_path(path);
    }

    ~ScopedWorkingDirectory()
    {
        if (_previous.empty())
            return;
        auto ec = std::error_code {};
        std::filesystem::current_path(_previous, ec);
    }

    ScopedWorkingDirectory(ScopedWorkingDirectory const&) = delete;
    ScopedWorkingDirectory& operator=(ScopedWorkingDirectory const&) = delete;
    ScopedWorkingDirectory(ScopedWorkingDirectory&&) = delete;
    ScopedWorkingDirectory& operator=(ScopedWorkingDirectory&&) = delete;

  private:
    std::filesystem::path _previous;
};

} // namespace core::testing
