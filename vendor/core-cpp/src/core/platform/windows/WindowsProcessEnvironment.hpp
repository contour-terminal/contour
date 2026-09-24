// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/ProcessEnvironment.hpp>

#include <map>
#include <string>

namespace core::platform
{

/// @brief Windows implementation of ProcessEnvironment.
///
/// Reads through @c core::LiveEnvironment and exports through
/// @c core::setProcessEnvironmentVariable(), both over the wide API in UTF-8, so a value outside
/// the ANSI code page survives. Environment variable names are treated case-insensitively,
/// matching Windows behavior.
class WindowsProcessEnvironment final: public ProcessEnvironment
{
  public:
    [[nodiscard]] std::expected<void, PlatformError> set(std::string_view name,
                                                         std::string_view value) override;
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const override;
    [[nodiscard]] std::expected<void, PlatformError> unset(std::string_view name) override;
    [[nodiscard]] std::expected<void, PlatformError> exportVariable(std::string_view name) override;
    [[nodiscard]] std::vector<std::string> keys() const override;

  private:
    /// @brief Internal storage for variables set but not exported, ordered as Windows compares
    /// names: case-insensitively in all of Unicode (`CompareStringOrdinal` with case ignored).
    struct CaseInsensitiveLess
    {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const;
    };

    std::map<std::string, std::string, CaseInsensitiveLess> _values;
};

} // namespace core::platform
