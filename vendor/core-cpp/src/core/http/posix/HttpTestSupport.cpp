// SPDX-License-Identifier: Apache-2.0
//
// The POSIX half of HttpTestSupport.hpp's platform questions: no POSIX process runs under wine.
#include <core/http/HttpTestSupport.hpp>

namespace core::http::test
{

bool runsUnderWine() noexcept
{
    return false;
}

LoopbackFlood loopbackFlood() noexcept
{
    return LoopbackFlood::Stalls;
}

} // namespace core::http::test
