// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `FailingListener` — an @c IListener whose first accepts fail as scripted, then accepts from the
/// listener it decorates.
///
/// Origin: fastcached `src/tests/FailingAcceptsListener.hpp`.

#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>
#include <core/net/NetError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

namespace core::net::testing
{

/// An @c IListener whose first accepts answer scripted errors, then accepts for real.
///
/// **The listener is fine and the accepts are not**, which is the shape that ends an accept loop
/// that should have gone on: an `AcceptEx` completing with `WSAECONNRESET` because a client reset
/// its queued connection, or an `accept` answering `EMFILE` while descriptors are short. Neither can
/// be asked of a real kernel on demand. Each scripted failure is answered without touching the
/// decorated listener, so a connection a case opened beforehand is still queued behind it -- served
/// only by a loop that went on accepting.
///
/// It is the seam a consumer tests its own accept loop through: `posix/AcceptLoop.hpp`'s
/// `IAcceptCall` scripts the system call inside ONE accept, and is private to core-cpp.
///
/// **What it does not model:** a scripted failure resolves without suspending, where a real accept
/// that fails may have parked first. A loop over it that never yields spins its event loop, which is
/// what @c AcceptErrorPolicy::FailuresBeforeYield bounds; and a loop's behaviour WHILE an accept is
/// parked is not exercised by the scripted answers, only by the decorated listener's.
class FailingListener final: public IListener
{
  public:
    /// Decorates a listener this does not own.
    /// @param inner The listener accepted from once the failures are spent; must outlive this.
    /// @param failures What the first accepts answer, in order. Required: a decorator that fails
    ///        nothing tests nothing.
    FailingListener(IListener& inner, std::vector<NetError> failures):
        _inner { &inner }, _failures { std::move(failures) }
    {
    }

    /// Decorates a listener this owns, for a factory that hands one listener over.
    /// @param inner The listener accepted from once the failures are spent.
    /// @param failures What the first accepts answer, in order.
    FailingListener(std::unique_ptr<IListener> inner, std::vector<NetError> failures):
        _owned { std::move(inner) }, _inner { _owned.get() }, _failures { std::move(failures) }
    {
    }

    FailingListener(FailingListener const&) = delete;
    FailingListener& operator=(FailingListener const&) = delete;
    FailingListener(FailingListener&&) = delete;
    FailingListener& operator=(FailingListener&&) = delete;
    ~FailingListener() override = default;

    /// @return The next scripted failure, or once they are spent -- or once this listener is
    ///         closed -- the decorated listener's accept.
    [[nodiscard]] async::Task<AcceptResult> accept() override
    {
        if (_answered < _failures.size() && !closeToken().stop_requested())
        {
            auto failure = _failures[_answered];
            ++_answered;
            co_return std::unexpected(std::move(failure));
        }
        co_return co_await _inner->accept();
    }

    /// @return The decorated listener's port.
    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return _inner->boundPort(); }

    /// @return How many of the scripted failures have been answered.
    [[nodiscard]] std::size_t failuresAnswered() const noexcept { return _answered; }

  protected:
    /// Closes the decorated listener. Failures not yet answered are then never answered: a closed
    /// listener answers `Cancelled`, as a real one does. Closing the DECORATED listener directly
    /// fires its token, not this one's -- close the decorator, which is the listener the loop holds.
    void doClose() noexcept override { _inner->close(); }

  private:
    std::unique_ptr<IListener> _owned; ///< The decorated listener when this owns it; null otherwise.
    IListener* _inner;                 ///< The decorated listener; never null.
    std::vector<NetError> _failures;   ///< What the first accepts answer.
    std::size_t _answered {};          ///< Only the accepting flow touches it.
};

/// @param code What each failure answers.
/// @param count How many.
/// @return @p count failures with @p code, for a @c FailingListener's script.
[[nodiscard]] inline std::vector<NetError> repeatedFailures(NetErrorCode code, std::size_t count)
{
    return std::vector<NetError>(count, makeNetError(code, 0, "accept"));
}

} // namespace core::net::testing
