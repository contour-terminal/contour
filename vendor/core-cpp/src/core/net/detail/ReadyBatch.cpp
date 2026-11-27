// SPDX-License-Identifier: Apache-2.0

#include <core/net/detail/ReadyBatch.hpp>

namespace core::net::detail
{

std::size_t& readinessDispatchDepth() noexcept
{
    static thread_local auto depth = std::size_t { 0 };
    return depth;
}

} // namespace core::net::detail
