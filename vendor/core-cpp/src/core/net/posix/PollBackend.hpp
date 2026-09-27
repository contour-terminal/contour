// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The portable POSIX @c IoBackend: multiplexes the registered descriptors with
/// poll(2).
///
/// Always available, and the control every other backend is held against: it needs
/// no kernel object of its own, so it cannot fail to construct, and it keeps no
/// descriptor of its own per registration, so it can never hold a peer's connection
/// open. What it costs is the wait: poll(2) hands the kernel the whole watch set on
/// every call and scans it again on return, so a wait is O(registered) where epoll
/// and kqueue are O(ready). That is irrelevant for a terminal watching a handful of
/// descriptors and decisive for a server holding thousands of idle ones.
///
/// Interest is level-triggered, as it is on every backend: a descriptor that stays
/// ready is reported again on the next wait, so a partial read need not drain to
/// `EAGAIN` to stay live.

#include <core/net/IoBackend.hpp>
#include <core/net/detail/ReadyBatch.hpp>
#include <core/net/detail/WakeupChannel.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <vector>

namespace core::net
{

/// An @c IoBackend whose wait set is exactly the registered handlers, plus the
/// wakeup channel.
class PollBackend final: public IoBackend
{
  public:
    /// Creates the backend and its wakeup channel.
    /// @throws std::runtime_error under descriptor exhaustion (@c detail::WakeupChannel).
    PollBackend();

    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::Poll; }

    [[nodiscard]] std::expected<void, NetError> attach(ReadinessHandler& handler) override;

    [[nodiscard]] std::expected<void, NetError> setInterest(ReadinessHandler& handler,
                                                            Interest interest) override;

    void detach(ReadinessHandler& handler) noexcept override;

    [[nodiscard]] WaitResult wait(std::optional<platform::SteadyDuration> timeout) override;

    void wake() noexcept override { _wakeup.signal(); }

    /// @return The number of handlers currently attached, the wakeup channel's own
    ///         registration excluded. Not part of @c IoBackend: the loop never asks,
    ///         and the one caller is a test proving that nothing leaked a registration.
    [[nodiscard]] std::size_t attachedCount() const noexcept { return _registrations.size() - 1; }

  private:
    /// One registered handler and what it is currently watched for.
    struct Registration
    {
        ReadinessHandler* handler = nullptr; ///< The caller's handler (not owned).
        Interest interest = Interest::None;  ///< What it is watched for; None mutes it.
    };

    /// @param handler The handler to look for.
    /// @return Its registration, or nullptr if it is not attached.
    [[nodiscard]] Registration* find(ReadinessHandler const& handler) noexcept;

    /// The registrations, in registration order. The wakeup channel's is element 0,
    /// placed there by the constructor and never removed, so the set is never empty
    /// and a wait can always be broken.
    std::vector<Registration> _registrations;
    detail::WakeupChannel _wakeup; ///< How another thread breaks a wait in flight.
    detail::ReadyBatch _batch;     ///< What this wait found ready, and what `detach` withdraws from.
};

} // namespace core::net
