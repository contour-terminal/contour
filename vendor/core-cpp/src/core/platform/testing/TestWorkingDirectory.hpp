// SPDX-License-Identifier: Apache-2.0
#pragma once

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
class TestWorkingDirectory final: public WorkingDirectory
{
  public:
    /// @param initial The working directory to start in.
    explicit TestWorkingDirectory(std::string initial = "/home/testuser"): _current(std::move(initial)) {}

    [[nodiscard]] std::expected<void, PlatformError> changeDirectory(
        std::filesystem::path const& path) override
    {
        auto const resolved = path.is_absolute() ? path : std::filesystem::path(_current) / path;
        auto normalized = resolved.lexically_normal().generic_string();
        if (!_validPaths.empty() && !_validPaths.contains(normalized))
            return std::unexpected(PlatformError::FileNotFound);
        _current = std::move(normalized);
        return {};
    }

    [[nodiscard]] std::filesystem::path currentDirectory() const override { return _current; }

    /// Adds a path that @c changeDirectory accepts. While none has been added, every path is.
    void addValidPath(std::string const& path)
    {
        _validPaths.insert(std::filesystem::path(path).lexically_normal().generic_string());
    }

  private:
    std::string _current;
    std::set<std::string, std::less<>> _validPaths;
};

} // namespace core::platform::testing
