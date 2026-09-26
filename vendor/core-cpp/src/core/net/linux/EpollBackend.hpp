// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Linux @c IoBackend: multiplexes the registered descriptors with epoll(7)
/// instead of poll(2).
///
/// Same contract and the same observable behaviour as @c PollBackend — that is what
/// `BackendParity_test` is for. The difference is cost: epoll keeps the watch set in
/// the kernel and reports only what became ready, so a wait is O(ready) rather than
/// O(registered).
///
/// Interest is level-triggered (no `EPOLLET`), matching what the sockets and the
/// accept loop assume: a descriptor that stays ready is reported again on the next
/// wait, so a partial read does not have to drain to `EAGAIN` to stay live.
///
/// Ported from the reactor in the fastcached project (Apache-2.0, same author); see
/// `.agent/reference/provenance.md`.

#include <core/net/IoBackend.hpp>
#include <core/net/detail/ReadyBatch.hpp>
#include <core/net/detail/WakeupChannel.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <unordered_map>

namespace core::net
{

/// An @c IoBackend backed by an epoll instance.
class EpollBackend final: public IoBackend
{
  public:
    /// Creates the epoll instance and the wakeup channel.
    /// @note If `epoll_create1` fails — descriptor exhaustion — @c good() reports
    ///       false and every @c attach refuses, so @c makeBackend answers null and
    ///       @c makeDefaultBackend falls back to @c PollBackend.
    /// @throws std::runtime_error if the wakeup channel cannot be created.
    EpollBackend();
    ~EpollBackend() override;

    EpollBackend(EpollBackend const&) = delete;
    EpollBackend& operator=(EpollBackend const&) = delete;
    EpollBackend(EpollBackend&&) = delete;
    EpollBackend& operator=(EpollBackend&&) = delete;

    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::Epoll; }

    [[nodiscard]] std::expected<void, NetError> attach(ReadinessHandler& handler) override;

    [[nodiscard]] std::expected<void, NetError> setInterest(ReadinessHandler& handler,
                                                            Interest interest) override;

    void detach(ReadinessHandler& handler) noexcept override;

    [[nodiscard]] WaitResult wait(std::optional<platform::SteadyDuration> timeout) override;

    void wake() noexcept override { _wakeup.signal(); }

    /// @return True if the epoll instance was created successfully.
    [[nodiscard]] bool good() const noexcept { return _epollFd >= 0; }

  private:
    /// One registered handler: what it is watched for, and through which descriptor.
    struct Registration
    {
        Interest interest = Interest::None; ///< What it is watched for; None keeps it out of the set.

        /// The descriptor the kernel registration is on.
        ///
        /// Normally the CALLER'S. A `dup()` would share the underlying open file
        /// description, so the caller closing its copy would not release it — no FIN
        /// would reach the peer, whose read would block forever rather than observe
        /// EOF (poll(2), which holds no descriptor of its own, has no such effect). A
        /// duplicate is therefore made only for a genuine SECOND registration of one
        /// descriptor, which an epoll set rejects with `EEXIST` while poll(2) simply
        /// takes two entries; the interface permits it, so this backend must too.
        platform::NativeHandle watched = platform::InvalidHandle;

        bool owned = false; ///< True if @c watched is this backend's dup(), false if the caller's.
        bool armed = false; ///< True while the registration is actually in the epoll set.
    };

    /// Adds or updates @p handler's kernel registration for @p interest.
    /// @param handler The attached handler.
    /// @param registration Its record, updated in place on success.
    /// @param interest The readiness to arm (never @c Interest::None).
    /// @return Nothing, or the errno `epoll_ctl` or `dup` refused with.
    [[nodiscard]] std::expected<void, NetError> arm(ReadinessHandler& handler,
                                                    Registration& registration,
                                                    Interest interest);

    /// Removes @p registration from the epoll set if it is in it. Keeps any private
    /// duplicate, so re-arming the same handler costs no second descriptor.
    void disarm(Registration& registration) const noexcept;

    /// The largest number of ready events one `epoll_wait` reports. A wait that fills the batch
    /// reports the rest on the next one -- level-triggered interest means nothing is lost by
    /// capping it.
    static constexpr std::size_t ReadyBatchSize = 64;

    /// Where `epoll_wait` writes: @c ReadyBatchSize entries, defined beside `wait` so that
    /// `<sys/epoll.h>` stays out of a header consumers include.
    struct ReadyEvents;

    /// Allocated once. A local array was value-initialised -- 768 bytes zeroed -- on every wait,
    /// and a wait is once per request on a busy connection. Declared first, so a failed allocation
    /// leaves nothing behind to close.
    std::unique_ptr<ReadyEvents> _events;

    /// The wakeup channel is declared BEFORE the kernel descriptor, and the order is
    /// load-bearing rather than tidy: @c detail::WakeupChannel's constructor throws
    /// under descriptor exhaustion, a constructor that throws from its mem-init list
    /// does not run the class destructor, and an `int` has none of its own. Built the
    /// other way round, the epoll instance was created first and leaked on exactly the pressure
    /// that makes a descriptor worth having -- which also made
    /// @c makeDefaultBackend()'s documented fallback to @c PollBackend less likely to
    /// succeed. Declared earlier, it is constructed earlier and `epoll_create1` is never reached.
    detail::WakeupChannel _wakeup; ///< How another thread breaks a wait in flight.
    int _epollFd = -1;             ///< The epoll instance (owned).
    detail::ReadyBatch _batch;     ///< What this wait found ready, and what `detach` withdraws from.

    /// The live registrations, keyed by the handler's address — which is the
    /// registration's identity, since two registrations may share a descriptor.
    std::unordered_map<ReadinessHandler const*, Registration> _registrations;
};

} // namespace core::net
