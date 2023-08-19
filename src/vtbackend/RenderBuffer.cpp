// SPDX-License-Identifier: Apache-2.0
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
