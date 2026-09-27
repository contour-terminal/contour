// SPDX-License-Identifier: Apache-2.0
#include <core/log/LogSink.hpp>

#include <unistd.h>

namespace core::log
{

bool isStdOutTerminal() noexcept
{
    return ::isatty(STDOUT_FILENO) != 0;
}

bool isStdErrTerminal() noexcept
{
    return ::isatty(STDERR_FILENO) != 0;
}

} // namespace core::log
