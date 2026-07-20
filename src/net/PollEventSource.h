// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The production @c EventSource: multiplexes the user-registered file
/// descriptors and nothing else.
///
/// This is the reactor substrate for headless coroutine work driven by an
/// @c EventLoop: the multiplexer daemon's socket server, or any flow that only
/// needs to `co_await loop.waitReadable(fd)` / `waitWritable(fd)` and timers.

#include <cstdint>
#include <vector>

#include <net/EventSource.h>
#include <net/platform/NativeHandle.h>

namespace net
{

/// An @c EventSource whose wait set is exactly the user-registered fds.
///
/// Cross-platform: POSIX uses `poll(2)`, Windows uses `WaitForMultipleObjects`.
/// When nothing is registered, a positive timeout sleeps and a negative timeout is
/// treated as a benign timeout so the loop cannot block forever with no wakeable
/// source (the loop only calls `wait()` while something is parked, and a flow
/// parked solely on a timer supplies a finite timeout).
class PollEventSource: public EventSource
{
  public:
    PollEventSource() = default;

    [[nodiscard]] WaitOutcome wait(int timeoutMs) override;

    [[nodiscard]] FdToken attach(NativeHandle fd, FdInterest interest) override
    {
        return _registry.attach(fd, interest);
    }

    void detach(FdToken token) override { _registry.detach(token); }

    /// @return The number of fds currently attached.
    [[nodiscard]] std::size_t attachedCount() const noexcept { return _registry.size(); }

  private:
    FdRegistry _registry; ///< Watched fds, in registration order.
};

} // namespace net
