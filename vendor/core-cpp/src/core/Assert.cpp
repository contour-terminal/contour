// SPDX-License-Identifier: Apache-2.0

#include <core/Assert.hpp>

namespace core::detail
{

FailHandler& failHandler() noexcept
{
    static FailHandler storage {};
    return storage;
}

} // namespace core::detail
