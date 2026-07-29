// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A scripted @c EventSource for deterministic @c EventLoop unit tests.

#include <cstdint>
#include <deque>
#include <stdexcept>
#include <vector>

#include <net/EventSource.h>
#include <net/platform/NativeHandle.h>

namespace net::testing
{

/// An @c EventSource that returns a pre-scripted sequence of wait outcomes.
///
/// Each `wait()` pops and returns the next scripted outcome and records the
/// timeout it was called with. When the script is exhausted it THROWS: upstream
/// Endo returned an interrupt outcome as a backstop, but this port's WaitOutcome
/// has no interrupt channel, and an empty outcome would let a parked flow spin
/// the pump forever — a thrown error surfaces the test bug immediately instead.
///
/// The fd registry is modelled too: @c attach hands back synthetic, monotonically
/// increasing tokens (no real fds), so tests can script readiness on a given
/// token via @c pushReadable / @c pushWritable and drive the readiness awaitables
/// deterministically.
class ScriptedEventSource: public EventSource
{
  public:
    /// Appends a bare timeout outcome (nothing happened).
    void pushTimeout() { _scripted.push_back(WaitOutcome {}); }

    /// Appends an outcome marking @p token readable.
    /// @param token A token previously handed out by @c attach.
    void pushReadable(FdToken token)
    {
        _scripted.push_back(WaitOutcome { .readyRead = { token }, .readyWrite = {} });
    }

    /// Appends an outcome marking @p token writable.
    /// @param token A token previously handed out by @c attach.
    void pushWritable(FdToken token)
    {
        _scripted.push_back(WaitOutcome { .readyRead = {}, .readyWrite = { token } });
    }

    /// @return The timeouts passed to each `wait()` call, in order.
    [[nodiscard]] std::vector<int> const& recordedTimeouts() const noexcept { return _timeouts; }

    /// @return How many times `wait()` was invoked.
    [[nodiscard]] std::size_t waitCount() const noexcept { return _timeouts.size(); }

    /// @return The number of fds currently attached (after attach/detach).
    [[nodiscard]] std::size_t attachedCount() const noexcept { return _attached; }

    /// @return The token most recently handed out by @c attach (invalid if none).
    [[nodiscard]] FdToken lastToken() const noexcept { return FdToken { _nextToken }; }

    WaitOutcome wait(int timeoutMs) override
    {
        _timeouts.push_back(timeoutMs);
        if (_scripted.empty())
            throw std::runtime_error("ScriptedEventSource: script exhausted while a flow is still parked");
        auto outcome = std::move(_scripted.front());
        _scripted.pop_front();
        return outcome;
    }

    FdToken attach(NativeHandle /*fd*/, FdInterest /*interest*/) override
    {
        ++_attached;
        return FdToken { ++_nextToken };
    }

    void detach(FdToken token) override
    {
        if (token && _attached > 0)
            --_attached;
    }

  private:
    std::deque<WaitOutcome> _scripted;
    std::vector<int> _timeouts;
    std::uint64_t _nextToken = 0; ///< Source of synthetic, never-zero tokens.
    std::size_t _attached = 0;    ///< Live attach()-minus-detach() count.
};

} // namespace net::testing
