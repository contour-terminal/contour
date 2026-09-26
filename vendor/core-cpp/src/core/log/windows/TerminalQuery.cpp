// SPDX-License-Identifier: Apache-2.0
#include <core/log/LogSink.hpp>

#include <cstdio>

#include <io.h>

namespace core::log
{

bool isStdOutTerminal() noexcept
{
    return ::_isatty(::_fileno(stdout)) != 0;
}

bool isStdErrTerminal() noexcept
{
    // This answered `true` unconditionally while it was an #ifdef inside ScopedOutput, so a
    // redirected standard error received SGR escapes -- against LogSink.hpp's own contract.
    return ::_isatty(::_fileno(stderr)) != 0;
}

} // namespace core::log
