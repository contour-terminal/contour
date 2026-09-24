// SPDX-License-Identifier: Apache-2.0
#include <core/log/detail/ProcessId.hpp>

#include <process.h>

namespace core::log::detail
{

int processId() noexcept
{
    return ::_getpid();
}

} // namespace core::log::detail
