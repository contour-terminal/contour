// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

namespace contour::test
{

/// A scratch directory that deletes itself, with no Qt in it.
///
/// QTemporaryDir would do the same job, but the tests that need a scratch directory are the ones
/// covering the configuration model and the CLI -- the layers that are deliberately Qt-free, and
/// the only ones a CONTOUR_FRONTEND_GUI=OFF build has. Depending on QtCore to test them would mean
/// that build could not run its own tests, which is how it came to have none.
class TempDir
{
  public:
    TempDir()
    {
        // Claimed by creating it: create_directory() reports false for a name that already exists,
        // so the loop settles on a free one whatever else is running. A counter alone would collide
        // between two concurrent test processes, and getpid() is not portable to MSVC.
        static std::atomic<unsigned> counter { 0 };
        auto const base =
            std::filesystem::temp_directory_path() / ("contour-test-" + std::to_string(counter.fetch_add(1)));
        for (auto attempt = 0u; attempt < 4096; ++attempt)
        {
            auto candidate = base;
            candidate += "-" + std::to_string(attempt);
            auto ec = std::error_code {};
            if (std::filesystem::create_directory(candidate, ec) && !ec)
            {
                _path = candidate;
                _valid = true;
                return;
            }
        }
    }

    ~TempDir()
    {
        auto ec = std::error_code {};
        std::filesystem::remove_all(_path, ec);
    }

    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    /// @return The directory, which exists for as long as this object does.
    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

    /// @return Whether the directory could be created.
    [[nodiscard]] bool isValid() const noexcept { return _valid; }

  private:
    std::filesystem::path _path;
    bool _valid = false;
};

} // namespace contour::test
