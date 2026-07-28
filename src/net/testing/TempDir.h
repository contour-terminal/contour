// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `TempDir` — a unique per-test directory, removed on destruction.
///
/// Shared because AF_UNIX tests need somewhere short-lived to put a socket file, and every test
/// that does was otherwise hand-rolling the same mkdtemp + remove_all pair.

#ifndef _WIN32

    #include <catch2/catch_test_macros.hpp>

    #include <filesystem>
    #include <format>
    #include <string>
    #include <string_view>
    #include <system_error>

    #include <unistd.h>

namespace net::testing
{

/// A unique directory under the system temp dir, removed (recursively) on destruction.
///
/// Not copyable or movable: the destructor deletes the directory, so a second owner would delete it
/// out from under the first.
class TempDir
{
  public:
    /// @param prefix Basename prefix, to tell one test's directory from another's in a stray leftover.
    explicit TempDir(std::string_view prefix = "contour-test")
    {
        auto templ = (std::filesystem::temp_directory_path() / std::format("{}-XXXXXX", prefix)).string();
        REQUIRE(::mkdtemp(templ.data()) != nullptr);
        _path = templ;
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

    /// @return The directory itself.
    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

    /// @param name A basename inside this directory.
    /// @return The full path to @p name. AF_UNIX callers must keep the result under ~100 bytes.
    [[nodiscard]] std::filesystem::path operator/(std::string_view name) const
    {
        return _path / std::filesystem::path { name };
    }

  private:
    std::filesystem::path _path;
};

} // namespace net::testing

#endif // _WIN32
