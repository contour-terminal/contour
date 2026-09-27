// SPDX-License-Identifier: Apache-2.0
#include <core/net/emscripten/EmscriptenHostScheduler.hpp>

#include <algorithm>
#include <limits>

#include <emscripten/emscripten.h>

namespace core::net
{

void EmscriptenHostScheduler::callAfter(std::chrono::milliseconds delay, HostCallback fn, void* state)
{
    // emscripten_async_call takes an int of milliseconds. A delay that does not fit is
    // a loop with no timer within twenty-four days, which is not a case worth a second
    // timer chain: clamp, and the pump arrives early. Early is a spare turn; late is a
    // deadline that never fires.
    auto const millis =
        std::clamp<std::chrono::milliseconds::rep>(delay.count(), 0, std::numeric_limits<int>::max());
    // The callback is `void (*)(void*) noexcept`, which converts to the
    // `void (*)(void*)` this takes; the other direction would not, and is why
    // HostCallback is noexcept — a host's loop cannot catch.
    emscripten_async_call(fn, state, static_cast<int>(millis));
}

} // namespace core::net
