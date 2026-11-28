// SPDX-License-Identifier: Apache-2.0
//
// The POSIX half of DescriptorTestSupport.hpp's platform questions: no POSIX process runs under wine.
#include <core/fs/DescriptorTestSupport.hpp>

namespace core::fs::test
{

bool runsUnderWine() noexcept
{
    return false;
}

} // namespace core::fs::test
