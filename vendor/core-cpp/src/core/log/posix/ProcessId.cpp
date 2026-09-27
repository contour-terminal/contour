// SPDX-License-Identifier: Apache-2.0
#include <core/log/detail/ProcessId.hpp>

#include <unistd.h>

namespace core::log::detail
{

int processId() noexcept
{
    return static_cast<int>(::getpid());
}

} // namespace core::log::detail
