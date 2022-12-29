/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <vtbackend/RenderBuffer.h>

#include <fmt/format.h>

#include <mutex>

namespace terminal
{

bool RenderDoubleBuffer::swapBuffers(std::chrono::steady_clock::time_point now) noexcept
{
    // If the terminal thread (writer) cannot try_lock (w/o wait time)
    // the front buffer, it'll just flush back buffer instead of swapping
    // buffers as the front buffer is apparently still in use by the
    // renderer thread and we want to avoid render-thread imposed
    // wait times in the terminal thread as much as possible.

    if (!readerLock.try_lock())
        return false;

    auto const _ = std::lock_guard<decltype(readerLock)>(readerLock, std::adopt_lock);

    for (;;)
    {
        auto current = currentBackBufferIndex.load();
        if (currentBackBufferIndex.compare_exchange_weak(current, (current + 1) % 2))
            break;
    }

    lastUpdate = now;
    state = RenderBufferState::WaitingForRefresh;
    return true;
}

} // namespace terminal
