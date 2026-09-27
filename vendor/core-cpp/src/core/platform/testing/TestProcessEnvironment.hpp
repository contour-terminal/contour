// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/ProcessEnvironment.hpp>

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::platform::testing
{

/// @brief A process environment holding exactly what a test put into it.
///
/// It never reads nor writes the process environment. It is the one double for both seams: a
/// @c ProcessEnvironment is a @c core::Environment, so a case hands this same object to code that
/// only reads and to code that writes. Its refusals are the native one's, so a case can assert
/// them here.
class TestProcessEnvironment final: public ProcessEnvironment
{
  public:
    /// Constructs an environment in which every name is unset.
    TestProcessEnvironment() = default;

    /// @param entries The variables this environment starts with; every other name reads as unset.
    explicit TestProcessEnvironment(std::map<std::string, std::string, std::less<>> entries):
        _values { std::move(entries) }
    {
    }

    [[nodiscard]] std::expected<void, PlatformError> set(std::string_view name,
                                                         std::string_view value) override
    {
        if (!isValidEnvironmentName(name) || value.contains('\0'))
            return std::unexpected(PlatformError::InvalidArgument);
        _values.insert_or_assign(std::string { name }, std::string { value });
        return {};
    }

    [[nodiscard]] std::optional<std::string> get(std::string_view name) const override
    {
        if (auto const i = _values.find(name); i != _values.end())
            return i->second;
        return std::nullopt;
    }

    [[nodiscard]] std::expected<void, PlatformError> unset(std::string_view name) override
    {
        if (!isValidEnvironmentName(name))
            return std::unexpected(PlatformError::InvalidArgument);
        if (auto const i = _values.find(name); i != _values.end())
            _values.erase(i);
        return {};
    }

    /// No process to export to: a variable reads the same exported or not.
    [[nodiscard]] std::expected<void, PlatformError> exportVariable(std::string_view name) override
    {
        if (!isValidEnvironmentName(name))
            return std::unexpected(PlatformError::InvalidArgument);
        return {};
    }

    [[nodiscard]] std::vector<std::string> keys() const override
    {
        auto result = std::vector<std::string> {};
        result.reserve(_values.size());
        for (auto const& [key, _]: _values)
            result.push_back(key);
        return result;
    }

  private:
    std::map<std::string, std::string, std::less<>> _values;
};

} // namespace core::platform::testing
