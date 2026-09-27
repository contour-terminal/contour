// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file ScopedTempDir.hpp
/// @brief A uniquely-named temporary directory that removes itself.

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
    #include <atomic>
    #include <format>
    #include <ranges>

    #include <process.h>
#else

    #include <cstdlib> // mkdtemp

    #include <unistd.h>
#endif

namespace core::testing
{

/// @brief A temporary directory unique to this instance, removed on destruction.
///
/// Tests that need a real filesystem must not build fixtures at a fixed path such as
/// `std::filesystem::temp_directory_path() / "my_cp_test"`: that is one shared location
/// for every process on the machine, so two runs of the same binary -- `ctest -j`, a
/// developer alongside CI, a sanitizer build alongside a normal one -- overwrite and delete
/// each other's fixtures. Several tests also wipe the directory *before* creating it, which
/// makes a collision destructive rather than merely flaky.
///
/// Uniqueness comes from the OS (`mkdtemp`), not from the caller choosing a distinctive
/// name, because a distinctive name is exactly what everyone believed they already had.
///
/// Prefer an injected `core::platform::testing::InMemoryFileSystem` where the code under test
/// accepts one; use this only where a real filesystem is unavoidable -- a forked child, or a real
/// file descriptor.
class ScopedTempDir
{
  public:
    /// @brief Creates a uniquely-named directory under the system temp directory.
    /// @param prefix Human-readable prefix, to make stray directories identifiable.
    /// @throws std::runtime_error if the directory cannot be created.
    ///
    /// Throwing rather than degrading: an empty path would make operator/ return a relative
    /// path, so the test would quietly build its fixtures in the current working directory
    /// -- which, with ScopedWorkingDirectory in play, may be another test's temp directory.
    /// Catch2 reports a constructor throw as a failing test, which is what should happen.
    explicit ScopedTempDir(std::string_view prefix)
    {
#ifdef _WIN32
        // No mkdtemp on Windows; the pid plus a per-process counter is unique, and
        // create_directory fails rather than silently reusing an existing directory.
        static std::atomic<unsigned> counter { 0 };
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, 128))
        {
            auto candidate = std::filesystem::temp_directory_path()
                             / std::format("{}_{}_{}", prefix, _getpid(), counter.fetch_add(1));
            auto ec = std::error_code {};
            if (std::filesystem::create_directory(candidate, ec))
            {
                _path = std::move(candidate);
                return;
            }
        }
#else
        auto pattern = (std::filesystem::temp_directory_path() / std::string(prefix)).string() + "_XXXXXX";
        if (char const* const created = ::mkdtemp(pattern.data()))
        {
            _path = created;
            return;
        }
#endif
        throw std::runtime_error("ScopedTempDir: could not create a temporary directory");
    }

    ~ScopedTempDir()
    {
        if (_path.empty())
            return;
        auto ec = std::error_code {};
        std::filesystem::remove_all(_path, ec);
    }

    ScopedTempDir(ScopedTempDir const&) = delete;
    ScopedTempDir& operator=(ScopedTempDir const&) = delete;
    ScopedTempDir(ScopedTempDir&&) = delete;
    ScopedTempDir& operator=(ScopedTempDir&&) = delete;

    /// @brief The directory's path. Never empty: construction throws instead.
    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

    /// @brief Builds a path inside the directory.
    [[nodiscard]] std::filesystem::path operator/(std::string_view name) const
    {
        return _path / std::filesystem::path(name);
    }

    /// @brief The directory's path as a string, for interpolating into a command line.
    ///
    /// Generic (forward-slash) form on purpose: a command language that treats a backslash as
    /// an escape would mangle a native Windows path -- and inside a glob pattern it would
    /// silently escape the character that follows it instead of separating a directory.
    /// Forward slashes are accepted by the Windows file APIs as well.
    [[nodiscard]] std::string string() const { return _path.generic_string(); }

  private:
    std::filesystem::path _path;
};

} // namespace core::testing
