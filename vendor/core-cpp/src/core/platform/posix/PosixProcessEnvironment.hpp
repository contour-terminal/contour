// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/ProcessEnvironment.hpp>

#include <functional>
#include <map>
#include <string>

namespace core::platform
{

/// @brief POSIX implementation of ProcessEnvironment using real OS calls.
///
/// Keeps variables set but not yet exported in memory. It reads the process environment through
/// @c core::LiveEnvironment, and exports to it through @c core::setProcessEnvironmentVariable() and
/// @c core::unsetProcessEnvironmentVariable() (never `setenv()`, see there).
class PosixProcessEnvironment final: public ProcessEnvironment
{
  public:
    [[nodiscard]] std::expected<void, PlatformError> set(std::string_view name,
                                                         std::string_view value) override;
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const override;
    [[nodiscard]] std::expected<void, PlatformError> unset(std::string_view name) override;
    [[nodiscard]] std::expected<void, PlatformError> exportVariable(std::string_view name) override;
    [[nodiscard]] std::vector<std::string> keys() const override;

  private:
    std::map<std::string, std::string, std::less<>> _values;
};

} // namespace core::platform
