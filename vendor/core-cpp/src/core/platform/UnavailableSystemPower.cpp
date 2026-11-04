// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SystemPower.hpp>

#include <expected>
#include <memory>

namespace core::platform
{

std::expected<std::unique_ptr<ISystemPowerMonitor>, PlatformError> makeNativeSystemPowerMonitor()
{
    return std::unexpected(PlatformError::NotImplemented);
}

} // namespace core::platform
