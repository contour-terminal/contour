// SPDX-License-Identifier: Apache-2.0
//
// The POSIX half of PlatformTestSupport.hpp's platform questions: no POSIX process runs under wine.
#include <core/platform/PlatformTestSupport.hpp>

namespace core::platform::test
{

bool runsUnderWine() noexcept
{
    return false;
}

} // namespace core::platform::test
