// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Utf8Path.hpp>
#include <core/platform/PathUtils.hpp>
#include <core/platform/WorkingDirectory.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <utility>

namespace core::platform::testing
{

/// @brief A working directory that exists only in the test: changing it touches no process state.
///
/// It holds its directories as UTF-8 text, generic (forward-slash) form, as every core-cpp path
/// string is: read through core::pathFromTrustedUtf8() and spelled through normalizePath(), never
/// through the narrow constructor or generic_string(), which use the ANSI code page on Windows.
/// The trusted form, because every such string is either the test's own or spelled from a path:
/// one that is not UTF-8 throws on Windows, failing the test that wrote it.
class TestWorkingDirectory final: public WorkingDirectory
{
  public:
    /// @param initial The working directory to start in, in UTF-8.
    explicit TestWorkingDirectory(std::string initial = "/home/testuser"): _current(std::move(initial)) {}

    [[nodiscard]] std::expected<void, PlatformError> changeDirectory(
        std::filesystem::path const& path) override
    {
        auto const resolved = path.is_absolute() ? path : core::pathFromTrustedUtf8(_current) / path;
        auto normalized = normalizePath(resolved.lexically_normal());
        if (!_validPaths.empty() && !_validPaths.contains(normalized))
            return std::unexpected(PlatformError::FileNotFound);
        _current = std::move(normalized);
        return {};
    }

    [[nodiscard]] std::filesystem::path currentDirectory() const override
    {
        return core::pathFromTrustedUtf8(_current);
    }

    /// Adds a path that @c changeDirectory accepts. While none has been added, every path is.
    /// @param path The path, in UTF-8.
    void addValidPath(std::string const& path)
    {
        _validPaths.insert(normalizePath(core::pathFromTrustedUtf8(path).lexically_normal()));
    }

  private:
    std::string _current;
    std::set<std::string, std::less<>> _validPaths;
};

} // namespace core::platform::testing
