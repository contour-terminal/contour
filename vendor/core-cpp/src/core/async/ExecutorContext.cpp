// SPDX-License-Identifier: Apache-2.0

#include <core/async/ExecutorContext.hpp>

namespace core::async::detail
{

ExecutorScope*& innermostExecutorScope() noexcept
{
    constinit thread_local ExecutorScope* innermost = nullptr;
    return innermost;
}

} // namespace core::async::detail
