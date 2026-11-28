// SPDX-License-Identifier: Apache-2.0

#include <core/Clock.hpp>

namespace core
{

IClock& defaultSteadyClock() noexcept
{
    static SteadyClock instance;
    return instance;
}

} // namespace core
