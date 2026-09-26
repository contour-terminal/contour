// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The self-pipe a blocking @c IoBackend is woken through from another thread.
///
/// @c IoBackend::wake() is the one thread-safe member of the interface, and every
/// backend that actually blocks needs the same mechanism behind it: a channel whose
/// read end sits in the wait set, so a byte written from any thread ends the wait
/// like any other readiness. Shared rather than written four times because the
/// failure mode of getting it wrong is silent — a lost wakeup is a loop that never
/// returns from `wait()`, which reads as a hang at shutdown and nowhere else.

#include <core/net/IoBackend.hpp>
#include <core/platform/SystemPipe.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

namespace core::net::detail
{

/// A wakeup channel and the @c ReadinessHandler that drains it.
///
/// The handler is registered with its backend like any other, so the wakeup takes
/// the same attach/interest/dispatch path every user registration does rather than a
/// private one that only shutdown exercises.
class WakeupChannel
{
  public:
    /// Creates the channel.
    /// @throws std::runtime_error under descriptor or handle exhaustion. Deliberately
    ///         not a @c std::expected: a backend whose wakeup silently failed would
    ///         deadlock the first time another thread tried to stop it, and there is
    ///         no useful thing for a caller to do about it here
    ///         (`.agent/rules/cpp-guidelines.md`, on unrecoverable conditions).
    WakeupChannel()
    {
        auto pipe = platform::createSystemPipe();
        if (!pipe)
            throw std::runtime_error("core::net: cannot create the backend wakeup channel "
                                     "(descriptor exhaustion?)");
        _pipe = std::move(*pipe);
        _handler = ReadinessHandler { .handle = _pipe->waitHandle(),
                                      .kind = DefaultHandleKind,
                                      .owner = this,
                                      .onReadable = &WakeupChannel::drain,
                                      .onWritable = nullptr,
                                      .onError = nullptr };
    }

    WakeupChannel(WakeupChannel const&) = delete;
    WakeupChannel& operator=(WakeupChannel const&) = delete;
    WakeupChannel(WakeupChannel&&) = delete;
    WakeupChannel& operator=(WakeupChannel&&) = delete;
    ~WakeupChannel() = default;

    /// @return The handler to register with the backend. Its address is stable for
    ///         this object's lifetime, which is what a registration requires.
    [[nodiscard]] ReadinessHandler& handler() noexcept { return _handler; }

    /// Writes one wakeup byte. Safe from any thread; it is a socket send.
    ///
    /// A wakeup with no wait in flight is not lost: the channel stays readable, so
    /// the next wait returns at once and drains it.
    void signal() noexcept
    {
        auto const one = char { 1 };
        std::ignore = _pipe->write(&one, 1);
    }

  private:
    /// Reads the wakeup bytes away. One bounded read: if more remain, the channel is
    /// still readable and the next wait reports it again (every backend here is
    /// level-triggered), so nothing is lost and nothing spins.
    /// @param handler The wakeup channel's own handler.
    static void drain(ReadinessHandler& handler) noexcept
    {
        auto* const self = static_cast<WakeupChannel*>(handler.owner);
        auto buffer = std::array<char, 256> {};
        std::ignore = self->_pipe->read(buffer.data(), buffer.size());
    }

    std::unique_ptr<platform::SystemPipe> _pipe;
    ReadinessHandler _handler;
};

} // namespace core::net::detail
